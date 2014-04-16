/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * libmbim-glib -- GLib/GIO based library to control MBIM devices
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2014 Aleksander Morgado <aleksander@lanedo.com>
 * Copyright (C) 2014 Greg Suarez <gsuarez@smithmicro.com>
 */

#include <string.h>
#include <ctype.h>
#include <sys/file.h>
#include <errno.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gunixsocketaddress.h>

#include "mbim-device.h"
#include "mbim-utils.h"
#include "mbim-proxy.h"
#include "mbim-message-private.h"
#include "mbim-cid.h"
#include "mbim-enum-types.h"
#include "mbim-error-types.h"

#define BUFFER_SIZE 512

G_DEFINE_TYPE (MbimProxy, mbim_proxy, G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_N_CLIENTS,
    PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

struct _MbimProxyPrivate {
    /* Unix socket service */
    GSocketService *socket_service;

    /* Clients */
    GList *clients;

    /* Devices */
    GList *devices;
};

/*****************************************************************************/

/**
 * mbim_proxy_get_n_clients:
 * @self: a #MbimProxy.
 *
 * Get the number of clients currently connected to the proxy.
 *
 * Returns: a #guint.
 */
guint
mbim_proxy_get_n_clients (MbimProxy *self)
{
    g_return_val_if_fail (MBIM_IS_PROXY (self), 0);

    return g_list_length (self->priv->clients);
}

/*****************************************************************************/

typedef struct {
    MbimUuid uuid;
} MbimClientInfo;

typedef struct {
    MbimProxy *proxy; /* not full ref */
    GSocketConnection *connection;
    GSource *connection_readable_source;
    GByteArray *buffer;
    MbimDevice *device;
    MbimMessage *internal_proxy_open_request;
    GArray *mbim_client_info_array;
    guint indication_id;
    gchar *device_file_path;
    gboolean opening_device;
} Client;

typedef struct {
    MbimProxy *proxy;
    Client *client;
} DeviceOpenContext;

static gboolean connection_readable_cb (GSocket *socket, GIOCondition condition, Client *client);

static void
client_free (Client *client)
{
    g_source_destroy (client->connection_readable_source);
    g_source_unref (client->connection_readable_source);

    if (client->device) {
        if (g_signal_handler_is_connected (client->device, client->indication_id))
            g_signal_handler_disconnect (client->device, client->indication_id);
        g_object_unref (client->device);
    }

    g_output_stream_close (g_io_stream_get_output_stream (G_IO_STREAM (client->connection)), NULL, NULL);

    if (client->buffer)
        g_byte_array_unref (client->buffer);

    if (client->device_file_path)
        g_free (client->device_file_path);

    if (client->internal_proxy_open_request)
        mbim_message_unref (client->internal_proxy_open_request);

    g_array_unref (client->mbim_client_info_array);

    g_object_unref (client->connection);
    g_slice_free (Client, client);
}

static guint
get_n_clients_with_device (MbimProxy *self,
                           MbimDevice *device)
{
    GList *l;
    guint n = 0;

    for (l = self->priv->clients; l; l = g_list_next (l)) {
        Client *client = l->data;

        if (device == client->device ||
            g_str_equal (mbim_device_get_path (device), mbim_device_get_path (client->device)))
            n++;
    }

    return n;
}

static Client *
get_client (MbimProxy *self,
            Client    *client)
{
    GList  *l;

    l = g_list_find (self->priv->clients, client);
    if (l)
        return l->data;
    return NULL;
}

static void
device_close_ready (MbimDevice   *device,
                    GAsyncResult *result)
{
    GError *error = NULL;

    if (!mbim_device_close_finish (device, result, &error)) {
        g_debug ("error: couldn't close device: %s", error->message);
        g_error_free (error);
    } else
        g_debug ("Device closed");

    g_object_unref (device);
}

static void
device_close (MbimProxy *self,
              MbimDevice *device)
{
    if (!device)
        return;

    /* If no more clients using the device, close and cleanup */
    if (get_n_clients_with_device (self, device) == 0) {
        GList *l;

        for (l = self->priv->devices; l; l = g_list_next (l)) {
            MbimDevice *device_in_list = MBIM_DEVICE (l->data);

            if (device == device_in_list ||
                g_str_equal (mbim_device_get_path (device), mbim_device_get_path (device_in_list))) {
                g_debug ("closing device '%s': no longer used", mbim_device_get_path_display (device));
                mbim_device_close (device_in_list, 
                                   15,
                                   NULL,
                                   (GAsyncReadyCallback) device_close_ready,
                                   NULL);
                self->priv->devices = g_list_remove (self->priv->devices, device_in_list);
                break;
            }
        }
    }
}

static void
connection_close (Client *client)
{
    MbimProxy *self;
    MbimDevice *device;

    g_debug ("Client (%d) connection closed...", g_socket_get_fd (g_socket_connection_get_socket (client->connection)));

    self = client->proxy;
    device = client->device ? g_object_ref (client->device) : NULL;
    client_free (client);
    self->priv->clients = g_list_remove (self->priv->clients, client);
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_N_CLIENTS]);

    device_close (self, device);
    g_object_unref (device);
}

static MbimDevice *
find_device_for_path (MbimProxy *self,
                      const gchar *path)
{
    GList *l;

    for (l = self->priv->devices; l; l = g_list_next (l)) {
        MbimDevice *device;

        device = (MbimDevice *)l->data;

        /* Return if found */
        if (g_str_equal (mbim_device_get_path (device), path))
            return device;
    }

    return NULL;
}

static gboolean
send_message (Client *client,
              MbimMessage *message,
              GError **error)
{
    g_debug ("Client (%d) TX: %u bytes", g_socket_get_fd (g_socket_connection_get_socket (client->connection)), message->len);
    if (!g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (client->connection)),
                                    message->data,
                                    message->len,
                                    NULL, /* bytes_written */
                                    NULL, /* cancellable */
                                    error)) {
        g_prefix_error (error, "Cannot send message to client: ");
        return FALSE;
    }

    return TRUE;
}

static void
complete_internal_proxy_open (Client *client)
{
    MbimMessage *response;
    GError *error = NULL;

    g_debug ("connection to MBIM device '%s' established", mbim_device_get_path (client->device));

    g_assert (client->internal_proxy_open_request != NULL);
    response = mbim_message_open_done_new (mbim_message_get_transaction_id (client->internal_proxy_open_request), 
                                           MBIM_STATUS_ERROR_NONE);

    if (!send_message (client, response, &error)) {
        mbim_message_unref (response);
        connection_close (client);
        return;
    }

    mbim_message_unref (response);
    mbim_message_unref (client->internal_proxy_open_request);
    client->internal_proxy_open_request = NULL;
}

static void
indication_cb (MbimDevice *device,
               MbimMessage *message,
               Client *client)
{
    guint i;
    GError *error = NULL;
    gboolean forward_indication = FALSE;

    if (mbim_message_indicate_status_get_service (message) == MBIM_SERVICE_INVALID) {
        for (i = 0; i < client->mbim_client_info_array->len; i++) {
            MbimClientInfo *info;

            info = &g_array_index (client->mbim_client_info_array, MbimClientInfo, i);
            /* If service UUID match, forward to the remote client */
            if (mbim_uuid_cmp(mbim_message_indicate_status_get_service_id (message), &info->uuid)) {
                forward_indication = TRUE;
                break;
            }
        }
    } else
        forward_indication = TRUE;

    if (forward_indication) {
        if (!send_message (client, message, &error)) {
            g_warning ("couldn't forward indication to client");
            g_error_free (error);
        }
    }
}

static void
device_open_ready (MbimDevice *device,
                   GAsyncResult *res,
                   DeviceOpenContext *ctx)
{
    MbimProxy *self = ctx->proxy;
    MbimDevice *existing;
    Client *client;
    GError *error = NULL;

    client = get_client (self, ctx->client);
    if (!client) {
        /* client must've been disconnected */
        mbim_device_open_finish (device, res, NULL);
        device_close (self, device);
        g_slice_free (DeviceOpenContext, ctx);
        return;
    }

    if (!mbim_device_open_finish (device, res, &error)) {
        g_debug ("couldn't open MBIM device: %s", error->message);
        connection_close (client);
        g_error_free (error);
        g_slice_free (DeviceOpenContext, ctx);
        return;
    }

    /* Store device in the proxy independently */
    existing = find_device_for_path (self, mbim_device_get_path (client->device));
    if (existing) {
        /* Race condition, we created two MbimDevices for the same port, just skip ours, no big deal */
        g_object_unref (client->device);
        client->device = g_object_ref (existing);
    } else {
        /* Keep the newly added device in the proxy */
        self->priv->devices = g_list_append (self->priv->devices, g_object_ref (client->device));
    }

    /* Register for device indications */
    client->indication_id = g_signal_connect (client->device,
                                              MBIM_DEVICE_SIGNAL_INDICATE_STATUS,
                                              G_CALLBACK (indication_cb),
                                              client);

    g_slice_free (DeviceOpenContext, ctx);
    complete_internal_proxy_open (client);
}

static void
device_new_ready (GObject *source,
                  GAsyncResult *res,
                  DeviceOpenContext *ctx)
{
    GError *error = NULL;
    Client *client;
    MbimDevice *device;

    device = mbim_device_new_finish (res, &error);
    client = get_client (ctx->proxy, ctx->client);
    if (!client) {
        /* client must've been disconnected */
        if (!device)
            g_error_free (error);
        device_close (ctx->proxy, device);
        g_slice_free (DeviceOpenContext, ctx);
        return;
    }

    client->device = device;
    if (!client->device) {
        g_debug ("couldn't open MBIM device: %s", error->message);
        connection_close (client);
        g_error_free (error);
        g_slice_free (DeviceOpenContext, ctx);
        return;
    }

    mbim_device_open (client->device,
                      30,
                      NULL,
                      (GAsyncReadyCallback)device_open_ready,
                      ctx);
}

static gboolean
process_internal_proxy_open (Client *client,
                             MbimMessage *message)
{
    MbimProxy *self = client->proxy;
    DeviceOpenContext  *ctx;


    /* Keep it */
    client->internal_proxy_open_request = mbim_message_ref (message);

    if (client->device_file_path) {
        client->device = find_device_for_path (self, client->device_file_path);
    } else if (!client->device_file_path && g_list_length (self->priv->devices)) {
        /* no device file path set, just use first device found */
        client->device = (MbimDevice *)(g_list_first (self->priv->devices))->data;
    } else {
        g_debug ("missing device file path");
        complete_internal_proxy_open (client);
        return FALSE;
    }

    /* Need to create a device ourselves */
    if (!client->device) {
        GFile *file;

        ctx = g_slice_new0 (DeviceOpenContext);
        ctx->proxy = self;
        ctx->client = client;

        file = g_file_new_for_path (client->device_file_path);
        mbim_device_new (file,
                         NULL,
                         (GAsyncReadyCallback)device_new_ready,
                         ctx);
        g_object_unref (file);
        return TRUE;
    } else {
        /* register client for notifications */
        client->indication_id = g_signal_connect (client->device,
                                                  MBIM_DEVICE_SIGNAL_INDICATE_STATUS,
                                                  G_CALLBACK (indication_cb),
                                                  client);
    }

    /* Keep a reference to the device in the client */
    g_object_ref (client->device);

    complete_internal_proxy_open (client);
    return FALSE;
}

static gboolean
process_internal_proxy_close (Client *client,
                              MbimMessage *message)
{
    MbimProxy *self = client->proxy;
    MbimDevice *device;
    MbimMessage *response;
    GError *error = NULL;

    device = client->device ? g_object_ref (client->device) : NULL;
    device_close (self, device);
    g_object_unref (device);

    response = mbim_message_close_done_new (mbim_message_get_transaction_id (message), MBIM_STATUS_ERROR_NONE);
    if (!send_message (client, response, &error)) {
        mbim_message_unref (response);
        connection_close (client);
        return FALSE;
    }

    mbim_message_unref (response);
    return TRUE;
}

static gboolean
process_internal_proxy_config (Client *client,
                               MbimMessage *message)
{
    MbimMessage *response;
    MbimStatusError error_status_code;
    struct command_done_message *command_done;
    GError *error = NULL;


    if (mbim_message_command_get_command_type(message) == MBIM_MESSAGE_COMMAND_TYPE_SET) {
        if (client->device_file_path)
            g_free (client->device_file_path);

        client->device_file_path = _mbim_message_read_string (message, 0, 0);
        error_status_code = MBIM_STATUS_ERROR_NONE;
    } else
        error_status_code = MBIM_STATUS_ERROR_INVALID_PARAMETERS;
        
    response = (MbimMessage *)_mbim_message_allocate (MBIM_MESSAGE_TYPE_COMMAND_DONE, 
                                                      mbim_message_get_transaction_id(message), 
                                                      sizeof (struct command_done_message));
    command_done = &(((struct full_message *)(response->data))->message.command_done);
    command_done->fragment_header.total   = GUINT32_TO_LE (1);
    command_done->fragment_header.current = 0;
    memcpy (command_done->service_id, MBIM_UUID_PROXY_CONTROL, sizeof(MbimUuid));
    command_done->command_id  = GUINT32_TO_LE (mbim_message_command_get_cid(message));
    command_done->status_code = GUINT32_TO_LE (error_status_code);

    if (!send_message (client, response, &error)) {
        mbim_message_unref (response);
        connection_close (client);
        return FALSE;
    }

    mbim_message_unref (response);
    return TRUE;
}

static void
track_uuid (Client *client,
            gboolean track,
            MbimMessage *message)
{
    gchar *uuid_display;
    MbimClientInfo info;
    guint i;
    gboolean exists;

    memcpy (&info.uuid, mbim_message_command_done_get_service_id(message), sizeof(info.uuid));
    uuid_display = mbim_uuid_get_printable (&info.uuid);

    /* Check if it already exists */
    for (i = 0; i < client->mbim_client_info_array->len; i++) {
        MbimClientInfo *existing;

        existing = &g_array_index (client->mbim_client_info_array, MbimClientInfo, i);
        if (mbim_uuid_cmp(&info.uuid, &existing->uuid))
            break;
    }
    exists = (i < client->mbim_client_info_array->len);

    if (track && !exists) {
        g_debug ("MBIM client tracked [%s,%s]",
                 mbim_device_get_path_display (client->device),
        uuid_display);
        g_array_append_val (client->mbim_client_info_array, info);
    } else if (!track && exists) {
        g_debug ("MBIM client untracked [%s,%s]",
                 mbim_device_get_path_display (client->device),
                 uuid_display);
        g_array_remove_index (client->mbim_client_info_array, i);
    }

    g_free (uuid_display);
}

typedef struct {
    Client *client;
    guint32 transaction_id;
} Request;

static void
device_command_ready (MbimDevice *device,
                      GAsyncResult *res,
                      Request *request)
{
    MbimMessage *response;
    GError *error = NULL;

    response = mbim_device_command_finish (device, res, &error);
    if (!response) {
        g_warning ("sending request to device failed: %s", error->message);
	if (!mbim_device_is_open(device)) {
	    g_debug ("device is closed");
	    connection_close (request->client);
	}

        g_error_free (error);
        g_slice_free (Request, request);
        return;
    }

    /* track custom uuids */
    if (mbim_message_get_message_type (response) == MBIM_MESSAGE_TYPE_COMMAND_DONE) {
        if (mbim_message_command_done_get_service (response) == MBIM_SERVICE_INVALID)
            track_uuid (request->client, TRUE, response);
    }

    /* replace reponse transaction id with the requested transaction id */
    ((struct header *)(response->data))->transaction_id = GUINT32_TO_LE (request->transaction_id);

    if (!send_message (request->client, response, &error))
        connection_close (request->client);

    mbim_message_unref (response);
    g_slice_free (Request, request);
}

static gboolean
process_message (Client *client,
                 MbimMessage *message)
{
    guint timeout = 10;
    Request *request;

    /* Accept only command messages from the client */
    if (mbim_message_get_message_type (message) != MBIM_MESSAGE_TYPE_COMMAND &&
        mbim_message_get_message_type (message) != MBIM_MESSAGE_TYPE_OPEN    &&
        mbim_message_get_message_type (message) != MBIM_MESSAGE_TYPE_CLOSE) {
        g_debug ("invalid message from client: not a command message");
        return FALSE;
    }

    if (mbim_message_get_message_type (message) == MBIM_MESSAGE_TYPE_OPEN) {
        return process_internal_proxy_open (client, message);
    }
    else if (mbim_message_get_message_type (message) == MBIM_MESSAGE_TYPE_CLOSE) {
        return process_internal_proxy_close (client, message);
    }
    else if (mbim_message_command_get_service (message) == MBIM_SERVICE_PROXY_CONTROL &&
             mbim_message_command_get_cid (message) == MBIM_CID_PROXY_CONTROL_CONFIGURATION) {
        return process_internal_proxy_config (client, message);
    }

    request = g_slice_new0 (Request);
    request->client = client;
    request->transaction_id = mbim_message_get_transaction_id (message);

    /* replace command transaction id with internal proxy transaction id to avoid collision */
    mbim_message_set_transaction_id (message, mbim_device_get_next_transaction_id (client->device));

    if (mbim_message_command_get_service (message) == MBIM_SERVICE_BASIC_CONNECT) {
        if (mbim_message_command_get_cid (message) == MBIM_CID_BASIC_CONNECT_VISIBLE_PROVIDERS) {
            /* increase timeout for network scan */
            timeout = 120;
        } else if (mbim_message_command_get_cid (message) == MBIM_CID_BASIC_CONNECT_REGISTER_STATE &&
                   mbim_message_command_get_command_type (message) == MBIM_MESSAGE_COMMAND_TYPE_SET) {
            /* increase timeout for network registration */
            timeout = 60;
        } else if (mbim_message_command_get_cid (message) == MBIM_CID_BASIC_CONNECT_RADIO_STATE &&
                   mbim_message_command_get_command_type (message) == MBIM_MESSAGE_COMMAND_TYPE_SET) {
            /* increase timeout for setting radio state */
            timeout = 30;
        } else if (mbim_message_command_get_cid (message) == MBIM_CID_BASIC_CONNECT_PACKET_SERVICE &&
                   mbim_message_command_get_command_type (message) == MBIM_MESSAGE_COMMAND_TYPE_SET) {
            /* increase timeout for setting packet service */
            timeout = 30;
        } else if (mbim_message_command_get_cid (message) == MBIM_CID_BASIC_CONNECT_CONNECT &&
                   mbim_message_command_get_command_type (message) == MBIM_MESSAGE_COMMAND_TYPE_SET) {
            /* increase timeout for connecting */
            timeout = 60;
        } else if (mbim_message_command_get_cid (message) == MBIM_CID_BASIC_CONNECT_IP_CONFIGURATION &&
                   mbim_message_command_get_command_type (message) == MBIM_MESSAGE_COMMAND_TYPE_QUERY) {
            /* increase timeout for querying ip configuration */
            timeout = 60;
        }
    }

    mbim_device_command (client->device,
                         message,
                         timeout,
                         NULL,
                         (GAsyncReadyCallback)device_command_ready,
                         request);
    return TRUE;
}

static void
parse_request (Client *client)
{
    do {
        MbimMessage *message;
        guint32 len = 0;

        if (client->buffer->len >= sizeof (struct header) &&
            (len = GUINT32_FROM_LE(((struct header *)client->buffer->data)->length)) > client->buffer->len) {
            /* have not received complete message */
            return;
        }

        if (!len)
            return;

        message = mbim_message_new(client->buffer->data, len);
        if (!message) {
            return;
        } else {
            g_byte_array_remove_range (client->buffer, 0, len);

            /* Play with the received message */
            process_message (client, message);
            mbim_message_unref (message);
        }
    } while (client->buffer->len > 0);
}

static gboolean
connection_readable_cb (GSocket *socket,
                        GIOCondition condition,
                        Client *client)
{
    guint8 buffer[BUFFER_SIZE];
    GError *error = NULL;
    gssize r;

    if (condition & G_IO_HUP || condition & G_IO_ERR) {
        connection_close (client);
        return FALSE;
    }

    if (!(condition & G_IO_IN || condition & G_IO_PRI))
        return TRUE;

    r = g_input_stream_read (g_io_stream_get_input_stream (G_IO_STREAM (client->connection)),
                             buffer,
                             BUFFER_SIZE,
                             NULL,
                             &error);
    if (r < 0) {
        g_warning ("Error reading from istream: %s", error ? error->message : "unknown");
        if (error)
            g_error_free (error);
        /* Close the device */
        connection_close (client);
        return FALSE;
    }

    if (r == 0)
        return TRUE;

    /* else, r > 0 */
    if (!G_UNLIKELY (client->buffer))
        client->buffer = g_byte_array_sized_new (r);
    g_byte_array_append (client->buffer, buffer, r);

    /* Try to parse input messages */
    parse_request (client);

    return TRUE;
}

static void
incoming_cb (GSocketService *service,
             GSocketConnection *connection,
             GObject *unused,
             MbimProxy *self)
{
    Client *client;
    GCredentials *credentials;
    GError *error = NULL;
    uid_t uid;

    g_debug ("Client (%d) connection open...", g_socket_get_fd (g_socket_connection_get_socket (connection)));

    credentials = g_socket_get_credentials (g_socket_connection_get_socket (connection), &error);
    if (!credentials) {
        g_warning ("Client not allowed: Error getting socket credentials: %s", error->message);
        g_error_free (error);
        return;
    }

    uid = g_credentials_get_unix_user (credentials, &error);
    g_object_unref (credentials);
    if (error) {
        g_warning ("Client not allowed: Error getting unix user id: %s", error->message);
        g_error_free (error);
        return;
    }

    if (uid != 0) {
        g_warning ("Client not allowed: Not enough privileges");
        return;
    }

    /* Create client */
    client = g_slice_new0 (Client);
    client->proxy = self;
    client->connection = g_object_ref (connection);
    client->connection_readable_source = g_socket_create_source (g_socket_connection_get_socket (client->connection),
                                                                 G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP,
                                                                 NULL);
    g_source_set_callback (client->connection_readable_source,
                           (GSourceFunc)connection_readable_cb,
                           client,
                           NULL);
    g_source_attach (client->connection_readable_source, NULL);
    client->mbim_client_info_array = g_array_sized_new (FALSE, FALSE, sizeof (MbimClientInfo), 8);

    /* Keep the client info around */
    self->priv->clients = g_list_append (self->priv->clients, client);
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_N_CLIENTS]);
}

static gboolean
setup_socket_service (MbimProxy *self,
                      GError **error)
{
    GSocketAddress *socket_address;
    GSocket *socket;

    socket = g_socket_new (G_SOCKET_FAMILY_UNIX,
                           G_SOCKET_TYPE_STREAM,
                           G_SOCKET_PROTOCOL_DEFAULT,
                           error);
    if (!socket)
        return FALSE;

    /* Bind to address */
    socket_address = (g_unix_socket_address_new_with_type (
                          MBIM_PROXY_SOCKET_PATH,
                          -1,
                          G_UNIX_SOCKET_ADDRESS_ABSTRACT));
    if (!g_socket_bind (socket, socket_address, TRUE, error)) {
        g_object_unref (socket);
        return FALSE;
    }
    g_object_unref (socket_address);

    g_debug ("creating UNIX socket service...");

    /* Listen */
    if (!g_socket_listen (socket, error)) {
        g_object_unref (socket);
        return FALSE;
    }

    /* Create socket service */
    self->priv->socket_service = g_socket_service_new ();
    g_signal_connect (self->priv->socket_service, "incoming", G_CALLBACK (incoming_cb), self);
    if (!g_socket_listener_add_socket (G_SOCKET_LISTENER (self->priv->socket_service),
                                       socket,
                                       NULL, /* don't pass an object, will take a reference */
                                       error)) {
        g_prefix_error (error, "Error adding socket at '%s' to socket service: ", MBIM_PROXY_SOCKET_PATH);
        g_object_unref (socket);
        return FALSE;
    }

    g_debug ("starting UNIX socket service at '%s'...", MBIM_PROXY_SOCKET_PATH);
    g_socket_service_start (self->priv->socket_service);
    g_object_unref (socket);
    return TRUE;
}

/*****************************************************************************/

MbimProxy *
mbim_proxy_new (GError **error)
{
    MbimProxy *self;

    /* Only root can run the mbim-proxy */
    if (getuid () != 0) {
        g_set_error (error,
                     MBIM_CORE_ERROR,
                     MBIM_CORE_ERROR_FAILED,
                     "Not enough privileges");
        return NULL;
    }

    self = g_object_new (MBIM_TYPE_PROXY, NULL);
    if (!setup_socket_service (self, error))
        g_clear_object (&self);
    return self;
}

static void
mbim_proxy_init (MbimProxy *self)
{
    /* Setup private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MBIM_TYPE_PROXY,
                                              MbimProxyPrivate);
}

static void
get_property (GObject *object,
              guint prop_id,
              GValue *value,
              GParamSpec *pspec)
{
    MbimProxy *self = MBIM_PROXY (object);

    switch (prop_id) {
    case PROP_N_CLIENTS:
        g_value_set_uint (value, g_list_length (self->priv->clients));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
dispose (GObject *object)
{
    MbimProxyPrivate *priv = MBIM_PROXY (object)->priv;

    if (priv->clients) {
        g_list_free_full (priv->clients, (GDestroyNotify) client_free);
        priv->clients = NULL;
    }

    if (priv->socket_service) {
        if (g_socket_service_is_active (priv->socket_service))
            g_socket_service_stop (priv->socket_service);
        g_clear_object (&priv->socket_service);
        g_unlink (MBIM_PROXY_SOCKET_PATH);
        g_debug ("UNIX socket service at '%s' stopped", MBIM_PROXY_SOCKET_PATH);
    }

    G_OBJECT_CLASS (mbim_proxy_parent_class)->dispose (object);
}

static void
mbim_proxy_class_init (MbimProxyClass *proxy_class)
{
    GObjectClass *object_class = G_OBJECT_CLASS (proxy_class);

    g_type_class_add_private (object_class, sizeof (MbimProxyPrivate));

    /* Virtual methods */
    object_class->get_property = get_property;
    object_class->dispose = dispose;

    /* Properties */
    properties[PROP_N_CLIENTS] =
        g_param_spec_uint (MBIM_PROXY_N_CLIENTS,
                           "Number of clients",
                           "Number of clients currently connected to the proxy",
                           0,
                           G_MAXUINT,
                           0,
                           G_PARAM_READABLE);
    g_object_class_install_property (object_class, PROP_N_CLIENTS, properties[PROP_N_CLIENTS]);
}