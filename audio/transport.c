/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2009  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>

#include <glib.h>
#include <gdbus.h>

#include "../src/adapter.h"
#include "../src/dbus-common.h"

#include "log.h"
#include "error.h"
#include "device.h"
#include "avdtp.h"
#include "media.h"
#include "transport.h"
#include "a2dp.h"
#include "headset.h"

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

#define MEDIA_TRANSPORT_INTERFACE "org.bluez.MediaTransport"

struct acquire_request {
	DBusMessage		*msg;
	guint			id;
	struct media_owner	*owner;
};

struct media_owner {
	struct media_transport	*transport;
	struct acquire_request *request;
	char			*name;
	char			*accesstype;
	guint			watch;
};

struct media_transport {
	DBusConnection		*conn;
	char			*path;		/* Transport object path */
	struct audio_device	*device;	/* Transport device */
	struct avdtp		*session;	/* Signalling session (a2dp only) */
	struct media_endpoint	*endpoint;	/* Transport endpoint */
	GSList			*owners;	/* Transport owners */
	uint8_t			*configuration; /* Transport configuration */
	int			size;		/* Transport configuration size */
	int			fd;		/* Transport file descriptor */
	uint16_t		imtu;		/* Transport input mtu */
	uint16_t		omtu;		/* Transport output mtu */
	uint16_t		delay;		/* Transport delay (a2dp only) */
	gboolean		read_lock;
	gboolean		write_lock;
	gboolean		in_use;
	guint			(*resume) (struct media_transport *transport,
					struct media_owner *owner);
	void			(*suspend) (struct media_transport *transport);
	void			(*cancel) (struct media_transport *transport,
								guint id);
	void			(*get_properties) (
					struct media_transport *transport,
					DBusMessageIter *dict);
	DBusMessage		*(*set_property) (
					struct media_transport *transport,
					DBusConnection *conn,
					DBusMessage *msg);
};

void media_transport_remove(struct media_transport *transport)
{
	char *path;

	path = g_strdup(transport->path);

	g_dbus_unregister_interface(transport->conn, path,
						MEDIA_TRANSPORT_INTERFACE);

	g_free(path);
}

static void acquire_request_free(struct acquire_request *req)
{
	struct media_owner *owner = req->owner;
	struct media_transport *transport = owner->transport;

	if (req->id)
		transport->cancel(owner->transport, req->id);

	if (req->msg)
		dbus_message_unref(req->msg);

	owner->request = NULL;
	g_free(req);
}

static gboolean media_transport_release(struct media_transport *transport,
					const char *accesstype)
{
	if (g_strstr_len(accesstype, -1, "r") != NULL) {
		transport->read_lock = FALSE;
		DBG("Transport %s: read lock released", transport->path);
	}

	if (g_strstr_len(accesstype, -1, "w") != NULL) {
		transport->write_lock = FALSE;
		DBG("Transport %s: write lock released", transport->path);
	}

	return TRUE;
}

static void media_owner_remove(struct media_owner *owner)
{
	struct media_transport *transport = owner->transport;

	media_transport_release(transport, owner->accesstype);

	if (owner->watch)
		g_dbus_remove_watch(transport->conn, owner->watch);

	if (owner->request) {
		DBusMessage *reply = g_dbus_create_error(owner->request->msg,
						ERROR_INTERFACE ".Failed",
						"%s", strerror(EIO));

		g_dbus_send_message(transport->conn, reply);

		acquire_request_free(owner->request);
	}

	transport->owners = g_slist_remove(transport->owners, owner);

	/* Suspend if the is no longer any owner */
	if (transport->owners == NULL)
		transport->suspend(transport);

	DBG("Renderer removed: sender=%s accesstype=%s", owner->name,
							owner->accesstype);

	g_free(owner->name);
	g_free(owner->accesstype);
	g_free(owner);
}

static gboolean media_transport_set_fd(struct media_transport *transport,
					int fd, uint16_t imtu, uint16_t omtu)
{
	if (transport->fd == fd)
		return TRUE;

	transport->fd = fd;
	transport->imtu = imtu;
	transport->omtu = omtu;

	info("%s: fd(%d) ready", transport->path, fd);

	emit_property_changed(transport->conn, transport->path,
				MEDIA_TRANSPORT_INTERFACE, "IMTU",
				DBUS_TYPE_UINT16, &transport->imtu);

	emit_property_changed(transport->conn, transport->path,
				MEDIA_TRANSPORT_INTERFACE, "OMTU",
				DBUS_TYPE_UINT16, &transport->omtu);

	return TRUE;
}

static gboolean remove_owner(gpointer data)
{
	media_owner_remove(data);

	return FALSE;
}

static void a2dp_resume_complete(struct avdtp *session,
				struct avdtp_error *err, void *user_data)
{
	struct media_owner *owner = user_data;
	struct acquire_request *req = owner->request;
	struct media_transport *transport = owner->transport;
	struct a2dp_sep *sep = media_endpoint_get_sep(transport->endpoint);
	struct avdtp_stream *stream;
	int fd;
	uint16_t imtu, omtu;

	req->id = 0;

	if (err)
		goto fail;

	stream = a2dp_sep_get_stream(sep);
	if (stream == NULL)
		goto fail;

	if (avdtp_stream_get_transport(stream, &fd, &imtu, &omtu, NULL) ==
			FALSE)
		goto fail;

	media_transport_set_fd(transport, fd, imtu, omtu);

	if (g_dbus_send_reply(transport->conn, req->msg,
				DBUS_TYPE_UNIX_FD, &fd,
				DBUS_TYPE_INVALID) == FALSE)
		goto fail;

	return;

fail:
	/* Let the stream state change before removing the owner */
	g_idle_add(remove_owner, owner);
}

static guint resume_a2dp(struct media_transport *transport,
				struct media_owner *owner)
{
	struct media_endpoint *endpoint = transport->endpoint;
	struct audio_device *device = transport->device;
	struct a2dp_sep *sep = media_endpoint_get_sep(endpoint);

	if (transport->session == NULL) {
		transport->session = avdtp_get(&device->src, &device->dst);
		if (transport->session == NULL)
			return 0;
	}

	if (transport->in_use == TRUE)
		goto done;

	transport->in_use = a2dp_sep_lock(sep, transport->session);
	if (transport->in_use == FALSE)
		return 0;

done:
	return a2dp_resume(transport->session, sep, a2dp_resume_complete,
				owner);
}

static void suspend_a2dp(struct media_transport *transport)
{
	struct media_endpoint *endpoint = transport->endpoint;
	struct a2dp_sep *sep = media_endpoint_get_sep(endpoint);

	a2dp_sep_unlock(sep, transport->session);
	transport->in_use = FALSE;
}

static void cancel_a2dp(struct media_transport *transport, guint id)
{
	a2dp_cancel(transport->device, id);
}

static void headset_resume_complete(struct audio_device *dev, void *user_data)
{
	struct media_owner *owner = user_data;
	struct acquire_request *req = owner->request;
	struct media_transport *transport = owner->transport;
	int fd;

	req->id = 0;

	if (dev == NULL)
		goto fail;

	fd = headset_get_sco_fd(dev);
	if (fd < 0)
		goto fail;

	media_transport_set_fd(transport, fd, 48, 48);

	if (g_dbus_send_reply(transport->conn, req->msg,
				DBUS_TYPE_UNIX_FD, &fd,
				DBUS_TYPE_INVALID) == FALSE)
		goto fail;

	return;

fail:
	media_owner_remove(owner);
}

static guint resume_headset(struct media_transport *transport,
				struct media_owner *owner)
{
	struct audio_device *device = transport->device;

	if (transport->in_use == TRUE)
		goto done;

	transport->in_use = headset_lock(device, HEADSET_LOCK_READ |
						HEADSET_LOCK_WRITE);
	if (transport->in_use == FALSE)
		return 0;

done:
	return headset_request_stream(device, headset_resume_complete,
					owner);
}

static void suspend_headset(struct media_transport *transport)
{
	struct audio_device *device = transport->device;

	headset_unlock(device, HEADSET_LOCK_READ | HEADSET_LOCK_WRITE);
	transport->in_use = FALSE;
}

static void cancel_headset(struct media_transport *transport, guint id)
{
	headset_cancel_stream(transport->device, id);
}

static void media_owner_exit(DBusConnection *connection, void *user_data)
{
	struct media_owner *owner = user_data;

	owner->watch = 0;
	if (owner->request != NULL)
		acquire_request_free(owner->request);

	media_owner_remove(owner);
}

static gboolean media_transport_acquire(struct media_transport *transport,
							const char *accesstype)
{
	gboolean read_lock = FALSE, write_lock = FALSE;

	if (g_strstr_len(accesstype, -1, "r") != NULL) {
		if (transport->read_lock == TRUE)
			return FALSE;
		read_lock = TRUE;
	}

	if (g_strstr_len(accesstype, -1, "w") != NULL) {
		if (transport->write_lock == TRUE)
			return FALSE;
		write_lock = TRUE;
	}

	/* Check invalid accesstype */
	if (read_lock == FALSE && write_lock == FALSE)
		return FALSE;

	if (read_lock) {
		transport->read_lock = read_lock;
		DBG("Transport %s: read lock acquired", transport->path);
	}

	if (write_lock) {
		transport->write_lock = write_lock;
		DBG("Transport %s: write lock acquired", transport->path);
	}


	return TRUE;
}

static struct media_owner *media_owner_create(
					struct media_transport *transport,
					DBusMessage *msg,
					const char *accesstype)
{
	struct media_owner *owner;

	owner = g_new0(struct media_owner, 1);
	owner->transport = transport;
	owner->name = g_strdup(dbus_message_get_sender(msg));
	owner->accesstype = g_strdup(accesstype);
	owner->watch = g_dbus_add_disconnect_watch(transport->conn,
							owner->name,
							media_owner_exit,
							owner, NULL);
	transport->owners = g_slist_append(transport->owners, owner);

	DBG("Renderer created: sender=%s accesstype=%s", owner->name,
			accesstype);

	return owner;
}

static struct media_owner *media_transport_find_owner(
					struct media_transport *transport,
					const char *name)
{
	GSList *l;

	for (l = transport->owners; l; l = l->next) {
		struct media_owner *owner = l->data;

		if (g_strcmp0(owner->name, name) == 0)
			return owner;
	}

	return NULL;
}

static DBusMessage *acquire(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct media_transport *transport = data;
	struct media_owner *owner;
	struct acquire_request *req;
	const char *accesstype, *sender;

	if (!dbus_message_get_args(msg, NULL,
				DBUS_TYPE_STRING, &accesstype,
				DBUS_TYPE_INVALID))
		return NULL;

	sender = dbus_message_get_sender(msg);

	owner = media_transport_find_owner(transport, sender);
	if (owner != NULL)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".Failed",
						"Permission denied");

	if (media_transport_acquire(transport, accesstype) == FALSE)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".Failed",
						"Permission denied");

	owner = media_owner_create(transport, msg, accesstype);
	req = g_new0(struct acquire_request, 1);
	req->msg = dbus_message_ref(msg);
	req->owner = owner;
	req->id = transport->resume(transport, owner);
	owner->request = req;
	if (req->id == 0)
		media_owner_remove(owner);

	return NULL;
}

static DBusMessage *release(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct media_transport *transport = data;
	struct media_owner *owner;
	const char *accesstype, *sender;

	if (!dbus_message_get_args(msg, NULL,
				DBUS_TYPE_STRING, &accesstype,
				DBUS_TYPE_INVALID))
		return NULL;

	sender = dbus_message_get_sender(msg);

	owner = media_transport_find_owner(transport, sender);
	if (owner == NULL)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".Failed",
						"Permission denied");

	if (g_strcmp0(owner->accesstype, accesstype) == 0)
		media_owner_remove(owner);
	else if (g_strstr_len(owner->accesstype, -1, accesstype) != NULL) {
		media_transport_release(transport, accesstype);
		g_strdelimit(owner->accesstype, accesstype, ' ');
	} else
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".Failed",
						"Permission denied");

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *set_property_a2dp(struct media_transport *transport,
					DBusConnection *conn, DBusMessage *msg)
{
	return NULL;
}

static DBusMessage *set_property_headset(struct media_transport *transport,
					DBusConnection *conn, DBusMessage *msg)
{
	return NULL;
}

static DBusMessage *set_property(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct media_transport *transport = data;

	return transport->set_property(transport, conn, msg);
}

static void get_properties_a2dp(struct media_transport *transport,
						DBusMessageIter *dict)
{
	dict_append_entry(dict, "Delay", DBUS_TYPE_UINT16, &transport->delay);
}

static void get_properties_headset(struct media_transport *transport,
						DBusMessageIter *dict)
{
	gboolean nrec, inband;

	nrec = headset_get_nrec(transport->device);
	dict_append_entry(dict, "NREC", DBUS_TYPE_BOOLEAN, &nrec);

	inband = headset_get_inband(transport->device);
	dict_append_entry(dict, "InbandRingtone", DBUS_TYPE_BOOLEAN, &inband);
}

static DBusMessage *get_properties(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct media_transport *transport = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *uuid;
	uint8_t codec;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	/* Device */
	dict_append_entry(&dict, "Device", DBUS_TYPE_OBJECT_PATH,
						&transport->device->path);

	dict_append_entry(&dict, "ReadLock", DBUS_TYPE_BOOLEAN,
						&transport->read_lock);

	dict_append_entry(&dict, "WriteLock", DBUS_TYPE_BOOLEAN,
						&transport->write_lock);

	dict_append_entry(&dict, "IMTU", DBUS_TYPE_UINT16,
						&transport->imtu);

	dict_append_entry(&dict, "OMTU", DBUS_TYPE_UINT16,
						&transport->omtu);

	uuid = media_endpoint_get_uuid(transport->endpoint);
	dict_append_entry(&dict, "UUID", DBUS_TYPE_STRING, &uuid);

	codec = media_endpoint_get_codec(transport->endpoint);
	dict_append_entry(&dict, "Codec", DBUS_TYPE_BYTE, &codec);

	dict_append_array(&dict, "Configuration", DBUS_TYPE_BYTE,
				&transport->configuration, transport->size);

	if (transport->get_properties)
		transport->get_properties(transport, &dict);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static GDBusMethodTable transport_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	get_properties },
	{ "Acquire",		"s",	"h",		acquire,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "Release",		"s",	"",		release },
	{ "SetProperty",	"sv",	"",		set_property },
	{ },
};

static GDBusSignalTable transport_signals[] = {
	{ "PropertyChanged",	"sv"	},
	{ }
};

static void media_transport_free(void *data)
{
	struct media_transport *transport = data;

	g_slist_foreach(transport->owners, (GFunc) media_owner_remove,
				NULL);
	g_slist_free(transport->owners);

	if (transport->session)
		avdtp_unref(transport->session);

	if (transport->conn)
		dbus_connection_unref(transport->conn);

	g_free(transport->configuration);
	g_free(transport->path);
	g_free(transport);
}

struct media_transport *media_transport_create(DBusConnection *conn,
						struct media_endpoint *endpoint,
						struct audio_device *device,
						uint8_t *configuration,
						size_t size)
{
	struct media_transport *transport;
	const char *uuid;
	static int fd = 0;

	transport = g_new0(struct media_transport, 1);
	transport->conn = dbus_connection_ref(conn);
	transport->device = device;
	transport->endpoint = endpoint;
	transport->configuration = g_new(uint8_t, size);
	memcpy(transport->configuration, configuration, size);
	transport->size = size;
	transport->path = g_strdup_printf("%s/fd%d", device->path, fd++);
	transport->fd = -1;

	uuid = media_endpoint_get_uuid(endpoint);
	if (g_strcmp0(uuid, A2DP_SOURCE_UUID) == 0 ||
			g_strcmp0(uuid, A2DP_SINK_UUID) == 0) {
		transport->resume = resume_a2dp;
		transport->suspend = suspend_a2dp;
		transport->cancel = cancel_a2dp;
		transport->get_properties = get_properties_a2dp;
		transport->set_property = set_property_a2dp;
	} else if (g_strcmp0(uuid, HFP_AG_UUID) == 0 ||
			g_strcmp0(uuid, HSP_AG_UUID) == 0) {
		transport->resume = resume_headset;
		transport->suspend = suspend_headset;
		transport->cancel = cancel_headset;
		transport->get_properties = get_properties_headset;
		transport->set_property = set_property_headset;
	} else
		goto fail;

	if (g_dbus_register_interface(transport->conn, transport->path,
				MEDIA_TRANSPORT_INTERFACE,
				transport_methods, transport_signals, NULL,
				transport, media_transport_free) == FALSE) {
		error("Could not register transport %s", transport->path);
		goto fail;
	}

	return transport;

fail:
	media_transport_free(transport);
	return NULL;
}

const char *media_transport_get_path(struct media_transport *transport)
{
	return transport->path;
}

void media_transport_update_delay(struct media_transport *transport,
							uint16_t delay)
{
	/* Check if delay really changed */
	if (transport->delay == delay)
		return;

	transport->delay = delay;

	emit_property_changed(transport->conn, transport->path,
				MEDIA_TRANSPORT_INTERFACE, "Delay",
				DBUS_TYPE_UINT16, &transport->delay);
}
