/* vi: set et sw=4 ts=4 cino=t0,(0: */
/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of mission-control
 *
 * Copyright (C) 2008-2009 Nokia Corporation.
 * Copyright (C) 2009 Collabora Ltd.
 *
 * Contact: Alberto Mardegan  <alberto.mardegan@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include "config.h"
#include "mcd-dispatch-operation-priv.h"

#include <stdio.h>
#include <string.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include <telepathy-glib/defs.h>
#include <telepathy-glib/gtypes.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/svc-channel-dispatch-operation.h>
#include <telepathy-glib/svc-generic.h>
#include <telepathy-glib/util.h>

#include "mcd-channel-priv.h"
#include "mcd-dbusprop.h"
#include "mcd-misc.h"

#include <libmcclient/mc-errors.h>

#define MCD_CLIENT_BASE_NAME "org.freedesktop.Telepathy.Client."
#define MCD_CLIENT_BASE_NAME_LEN (sizeof (MCD_CLIENT_BASE_NAME) - 1)

#define MCD_DISPATCH_OPERATION_PRIV(operation) (MCD_DISPATCH_OPERATION (operation)->priv)

static void
dispatch_operation_iface_init (TpSvcChannelDispatchOperationClass *iface,
                               gpointer iface_data);
static void properties_iface_init (TpSvcDBusPropertiesClass *iface,
                                   gpointer iface_data);

static const McdDBusProp dispatch_operation_properties[];

static const McdInterfaceData dispatch_operation_interfaces[] = {
    MCD_IMPLEMENT_IFACE (tp_svc_channel_dispatch_operation_get_type,
                         dispatch_operation,
                         TP_IFACE_CHANNEL_DISPATCH_OPERATION),
    { G_TYPE_INVALID, }
};

G_DEFINE_TYPE_WITH_CODE (McdDispatchOperation, _mcd_dispatch_operation,
                         G_TYPE_OBJECT,
    MCD_DBUS_INIT_INTERFACES (dispatch_operation_interfaces);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES, properties_iface_init);
    )

struct _McdDispatchOperationPrivate
{
    const gchar *unique_name;   /* borrowed from object_path */
    gchar *object_path;
    GStrv possible_handlers;
    GHashTable *properties;

    /* If FALSE, we're not actually on D-Bus; an object path is reserved,
     * but we're inaccessible. */
    guint needs_approval : 1;

    /* set of handlers we already tried
     * dup'd bus name (string) => dummy non-NULL pointer */
    GHashTable *failed_handlers;

    /* if TRUE, we will emit finished as soon as we can */
    gboolean wants_to_finish;
    gchar *handler;
    gint64 handle_with_time;
    gchar *claimer;
    DBusGMethodInvocation *claim_context;

    /* Reference to a global handler map */
    McdHandlerMap *handler_map;

    /* Reference to a global registry of clients */
    McdClientRegistry *client_registry;

    McdAccount *account;
    McdConnection *connection;

    /* Owned McdChannels we're dispatching */
    GList *channels;
    /* Owned McdChannels for which we can't emit ChannelLost yet, in
     * reverse chronological order */
    GList *lost_channels;

    /* If TRUE, either the channels being dispatched were requested, or they
     * were pre-approved by being returned as a response to another request,
     * or a client approved processing with arbitrary handlers */
    gboolean approved;

    /* If TRUE, at least one Approver accepted this dispatch operation, and
     * we're waiting for one of them to call HandleWith or Claim. This is a
     * client lock; a reference must be held while it is TRUE (in the
     * McdDispatcherContext, CTXREF14 ensures this). */
    gboolean awaiting_approval;

    /* If FALSE, we're still working out what Observers and Approvers to
     * run. This is a temporary client lock; a reference must be held
     * for as long as it is FALSE.
     */
    gboolean invoked_early_clients;

    /* The number of observers that have not yet returned from ObserveChannels.
     * Until they have done so, we can't allow the dispatch operation to
     * finish. This is a client lock.
     *
     * A reference is held for each pending observer (and in the
     * McdDispatcherContext, one instance of CTXREF05 is held for each). */
    gsize observers_pending;

    /* The number of approvers that have not yet returned from
     * AddDispatchOperation. Until they have done so, we can't allow the
     * dispatch operation to finish. This is a client lock.
     *
     * A reference is held for each pending approver (and in the
     * McdDispatcherContext, one instance of CTXREF06 is held for each). */
    gsize ado_pending;

    /* If TRUE, either we've already arranged for the channels to get a
     * handler, or there are no channels left. */
    gboolean channels_handled;

    /* If TRUE, we're dispatching a channel request and it was cancelled */
    gboolean cancelled;

    /* if TRUE, these channels were requested "behind our back", so stop
     * after observers */
    gboolean observe_only;
};

static void _mcd_dispatch_operation_check_finished (
    McdDispatchOperation *self);

static void _mcd_dispatch_operation_check_client_locks (
    McdDispatchOperation *self);

static inline gboolean
mcd_dispatch_operation_may_finish (McdDispatchOperation *self)
{
    return (self->priv->observers_pending == 0 &&
            self->priv->ado_pending == 0);
}

static void
_mcd_dispatch_operation_inc_observers_pending (McdDispatchOperation *self)
{
    g_return_if_fail (!self->priv->wants_to_finish);

    g_object_ref (self);

    DEBUG ("%" G_GSIZE_FORMAT " -> %" G_GSIZE_FORMAT,
           self->priv->observers_pending,
           self->priv->observers_pending + 1);
    self->priv->observers_pending++;
}

static void
_mcd_dispatch_operation_dec_observers_pending (McdDispatchOperation *self)
{
    DEBUG ("%" G_GSIZE_FORMAT " -> %" G_GSIZE_FORMAT,
           self->priv->observers_pending,
           self->priv->observers_pending - 1);
    g_return_if_fail (self->priv->observers_pending > 0);
    self->priv->observers_pending--;

    _mcd_dispatch_operation_check_finished (self);
    _mcd_dispatch_operation_check_client_locks (self);
    g_object_unref (self);
}

static void
_mcd_dispatch_operation_inc_ado_pending (McdDispatchOperation *self)
{
    g_return_if_fail (!self->priv->wants_to_finish);

    g_object_ref (self);

    DEBUG ("%" G_GSIZE_FORMAT " -> %" G_GSIZE_FORMAT,
           self->priv->ado_pending,
           self->priv->ado_pending + 1);
    self->priv->ado_pending++;
}

static void
_mcd_dispatch_operation_dec_ado_pending (McdDispatchOperation *self)
{
    DEBUG ("%" G_GSIZE_FORMAT " -> %" G_GSIZE_FORMAT,
           self->priv->ado_pending,
           self->priv->ado_pending - 1);
    g_return_if_fail (self->priv->ado_pending > 0);
    self->priv->ado_pending--;

    _mcd_dispatch_operation_check_finished (self);

    if (self->priv->ado_pending == 0 &&
        !self->priv->awaiting_approval)
    {
        DEBUG ("No approver accepted the channels; considering them to be "
               "approved");
        self->priv->approved = TRUE;
    }

    _mcd_dispatch_operation_check_client_locks (self);

    g_object_unref (self);
}

gboolean
_mcd_dispatch_operation_get_cancelled (McdDispatchOperation *self)
{
    g_return_val_if_fail (MCD_IS_DISPATCH_OPERATION (self), FALSE);
    return self->priv->cancelled;
}

static inline gboolean
_mcd_dispatch_operation_is_approved (McdDispatchOperation *self)
{
    return (self->priv->approved || !self->priv->needs_approval);
}

static void _mcd_dispatch_operation_run_handlers (McdDispatchOperation *self);

static void
_mcd_dispatch_operation_check_client_locks (McdDispatchOperation *self)
{
    if (self->priv->invoked_early_clients &&
        self->priv->ado_pending == 0 &&
        self->priv->observers_pending == 0 &&
        _mcd_dispatch_operation_is_approved (self))
    {
        /* no observers etc. left */
        if (!self->priv->channels_handled &&
            !self->priv->observe_only)
        {
            self->priv->channels_handled = TRUE;
            _mcd_dispatch_operation_run_handlers (self);
        }
    }
}

enum
{
    PROP_0,
    PROP_CHANNELS,
    PROP_CLIENT_REGISTRY,
    PROP_HANDLER_MAP,
    PROP_POSSIBLE_HANDLERS,
    PROP_NEEDS_APPROVAL,
    PROP_OBSERVE_ONLY,
};

/*
 * _mcd_dispatch_operation_get_connection_path:
 * @self: the #McdDispatchOperation.
 *
 * Returns: the D-Bus object path of the Connection associated with @self,
 *    or "/" if none.
 */
static const gchar *
_mcd_dispatch_operation_get_connection_path (McdDispatchOperation *self)
{
    const gchar *path;

    g_return_val_if_fail (MCD_IS_DISPATCH_OPERATION (self), "/");

    if (self->priv->connection == NULL)
        return "/";

    path = mcd_connection_get_object_path (self->priv->connection);

    g_return_val_if_fail (path != NULL, "/");

    return path;
}

static void
get_connection (TpSvcDBusProperties *self, const gchar *name, GValue *value)
{
    McdDispatchOperationPrivate *priv = MCD_DISPATCH_OPERATION_PRIV (self);

    DEBUG ("called for %s", priv->unique_name);
    g_value_init (value, DBUS_TYPE_G_OBJECT_PATH);
    g_value_set_boxed (value,
        _mcd_dispatch_operation_get_connection_path
            (MCD_DISPATCH_OPERATION (self)));
}

/*
 * _mcd_dispatch_operation_get_account_path:
 * @operation: the #McdDispatchOperation.
 *
 * Returns: the D-Bus object path of the Account associated with @operation,
 *    or "/" if none.
 */
static const gchar *
_mcd_dispatch_operation_get_account_path (McdDispatchOperation *self)
{
    const gchar *path;

    g_return_val_if_fail (MCD_IS_DISPATCH_OPERATION (self), "/");

    if (self->priv->account == NULL)
        return "/";

    path = mcd_account_get_object_path (self->priv->account);

    g_return_val_if_fail (path != NULL, "/");

    return path;
}

static void
get_account (TpSvcDBusProperties *self, const gchar *name, GValue *value)
{
    g_value_init (value, DBUS_TYPE_G_OBJECT_PATH);
    g_value_set_boxed (value,
        _mcd_dispatch_operation_get_account_path
            (MCD_DISPATCH_OPERATION (self)));
}

static void
get_channels (TpSvcDBusProperties *iface, const gchar *name, GValue *value)
{
    McdDispatchOperation *self = MCD_DISPATCH_OPERATION (iface);

    DEBUG ("called for %s", self->priv->unique_name);

    g_value_init (value, TP_ARRAY_TYPE_CHANNEL_DETAILS_LIST);
    g_value_take_boxed (value,
        _mcd_channel_details_build_from_list (self->priv->channels));
}

static void
get_possible_handlers (TpSvcDBusProperties *self, const gchar *name,
                       GValue *value)
{
    McdDispatchOperationPrivate *priv = MCD_DISPATCH_OPERATION_PRIV (self);

    DEBUG ("called for %s", priv->unique_name);
    g_value_init (value, G_TYPE_STRV);
    g_value_set_boxed (value, priv->possible_handlers);
}


static const McdDBusProp dispatch_operation_properties[] = {
    { "Interfaces", NULL, mcd_dbus_get_interfaces },
    { "Connection", NULL, get_connection },
    { "Account", NULL, get_account },
    { "Channels", NULL, get_channels },
    { "PossibleHandlers", NULL, get_possible_handlers },
    { 0 },
};

static void
properties_iface_init (TpSvcDBusPropertiesClass *iface, gpointer iface_data)
{
#define IMPLEMENT(x) tp_svc_dbus_properties_implement_##x (\
    iface, dbusprop_##x)
    IMPLEMENT(set);
    IMPLEMENT(get);
    IMPLEMENT(get_all);
#undef IMPLEMENT
}

static void
mcd_dispatch_operation_set_channel_handled_by (McdDispatchOperation *self,
                                               McdChannel *channel,
                                               const gchar *unique_name)
{
    const gchar *path;
    TpChannel *tp_channel;

    g_assert (unique_name != NULL);

    path = mcd_channel_get_object_path (channel);
    tp_channel = mcd_channel_get_tp_channel (channel);
    g_return_if_fail (tp_channel != NULL);

    _mcd_channel_set_status (channel, MCD_CHANNEL_STATUS_DISPATCHED);

    _mcd_handler_map_set_channel_handled (self->priv->handler_map,
                                          tp_channel, unique_name);
}

static void _mcd_dispatch_operation_set_approved (McdDispatchOperation *self);

static void
mcd_dispatch_operation_actually_finish (McdDispatchOperation *self)
{
    g_object_ref (self);

    DEBUG ("%s/%p: finished", self->priv->unique_name, self);
    tp_svc_channel_dispatch_operation_emit_finished (self);

    if (self->priv->channels == NULL)
    {
        DEBUG ("Nothing left to dispatch");
        self->priv->channels_handled = TRUE;
    }

    if (self->priv->claimer != NULL)
    {
        const GList *list;

        /* we don't release the client lock, in order to not run the handlers,
         * but we do have to mark all channels as dispatched */
        for (list = self->priv->channels; list != NULL; list = list->next)
        {
            McdChannel *channel = MCD_CHANNEL (list->data);

            mcd_dispatch_operation_set_channel_handled_by (self, channel,
                self->priv->claimer);
        }

        g_assert (!self->priv->channels_handled);
        self->priv->channels_handled = TRUE;
    }

    if (self->priv->awaiting_approval)
    {
        self->priv->awaiting_approval = FALSE;
        _mcd_dispatch_operation_set_approved (self);
    }

    if (self->priv->claim_context != NULL)
    {
        DEBUG ("Replying to Claim call from %s", self->priv->claimer);
        tp_svc_channel_dispatch_operation_return_from_claim (self->priv->claim_context);
        self->priv->claim_context = NULL;
    }

    g_object_unref (self);
}

static void
_mcd_dispatch_operation_finish (McdDispatchOperation *operation)
{
    McdDispatchOperationPrivate *priv = operation->priv;

    if (priv->wants_to_finish)
    {
        DEBUG ("already finished (or about to)!");
        return;
    }

    priv->wants_to_finish = TRUE;

    if (mcd_dispatch_operation_may_finish (operation))
    {
        DEBUG ("%s/%p has finished", priv->unique_name, operation);
        mcd_dispatch_operation_actually_finish (operation);
    }
    else
    {
        DEBUG ("%s/%p not finishing just yet", priv->unique_name,
               operation);
    }
}

static gboolean mcd_dispatch_operation_check_handle_with (
    McdDispatchOperation *self, const gchar *handler_name, GError **error);

static void
dispatch_operation_handle_with (TpSvcChannelDispatchOperation *cdo,
                                const gchar *handler_name,
                                DBusGMethodInvocation *context)
{
    GError *error = NULL;
    McdDispatchOperation *self = MCD_DISPATCH_OPERATION (cdo);
    GTimeVal now = { 0, 0 };

    DEBUG ("%s/%p", self->priv->unique_name, self);

    if (!mcd_dispatch_operation_check_handle_with (self, handler_name, &error))
    {
        dbus_g_method_return_error (context, error);
        g_error_free (error);
        return;
    }

    g_get_current_time (&now);
    self->priv->handle_with_time = now.tv_sec;

    if (handler_name != NULL && handler_name[0] != '\0')
    {
        self->priv->handler = g_strdup (handler_name +
                                        MCD_CLIENT_BASE_NAME_LEN);
    }

    _mcd_dispatch_operation_finish (self);
    tp_svc_channel_dispatch_operation_return_from_handle_with (context);
}

static void
dispatch_operation_claim (TpSvcChannelDispatchOperation *self,
                          DBusGMethodInvocation *context)
{
    McdDispatchOperationPrivate *priv;

    priv = MCD_DISPATCH_OPERATION_PRIV (self);
    if (priv->wants_to_finish)
    {
        GError *error = g_error_new (TP_ERRORS, TP_ERROR_NOT_YOURS,
                                     "CDO already finished (or trying to)");
        DEBUG ("Giving error to %s: %s", dbus_g_method_get_sender (context),
               error->message);
        dbus_g_method_return_error (context, error);
        g_error_free (error);
        return;
    }

    g_assert (priv->claimer == NULL);
    g_assert (priv->claim_context == NULL);
    priv->claimer = dbus_g_method_get_sender (context);
    priv->claim_context = context;
    DEBUG ("Claiming on behalf of %s", priv->claimer);

    _mcd_dispatch_operation_finish (MCD_DISPATCH_OPERATION (self));
}

static void
dispatch_operation_iface_init (TpSvcChannelDispatchOperationClass *iface,
                               gpointer iface_data)
{
#define IMPLEMENT(x) tp_svc_channel_dispatch_operation_implement_##x (\
    iface, dispatch_operation_##x)
    IMPLEMENT(handle_with);
    IMPLEMENT(claim);
#undef IMPLEMENT
}

static void
create_object_path (McdDispatchOperationPrivate *priv)
{
    static guint cpt = 0;
    priv->object_path =
        g_strdup_printf (MC_DISPATCH_OPERATION_DBUS_OBJECT_BASE "do%u",
                         cpt++);
    priv->unique_name = priv->object_path +
        (sizeof (MC_DISPATCH_OPERATION_DBUS_OBJECT_BASE) - 1);
}

static GObject *
mcd_dispatch_operation_constructor (GType type, guint n_params,
                                    GObjectConstructParam *params)
{
    GObjectClass *object_class =
        (GObjectClass *)_mcd_dispatch_operation_parent_class;
    GObject *object;
    McdDispatchOperation *operation;
    McdDispatchOperationPrivate *priv;

    object = object_class->constructor (type, n_params, params);
    operation = MCD_DISPATCH_OPERATION (object);

    g_return_val_if_fail (operation != NULL, NULL);
    priv = operation->priv;

    if (!priv->client_registry || !priv->handler_map)
        goto error;

    if (priv->possible_handlers == NULL && !priv->observe_only)
    {
        g_critical ("!observe_only => possible_handlers must not be NULL");
        goto error;
    }

    if (priv->needs_approval && priv->observe_only)
    {
        g_critical ("observe_only => needs_approval must not be TRUE");
        goto error;
    }

    create_object_path (priv);

    DEBUG ("%s/%p: needs_approval=%c", priv->unique_name, object,
           priv->needs_approval ? 'T' : 'F');

    if (DEBUGGING)
    {
        GList *list;

        for (list = priv->channels; list != NULL; list = list->next)
        {
            DEBUG ("Channel: %s", mcd_channel_get_object_path (list->data));
        }
    }

    /* If approval is not needed, we don't appear on D-Bus (and approvers
     * don't run) */
    if (priv->needs_approval)
    {
        TpDBusDaemon *dbus_daemon;
        DBusGConnection *dbus_connection;

        g_object_get (priv->client_registry,
                      "dbus-daemon", &dbus_daemon,
                      NULL);

        /* can be NULL if we have fallen off the bus (in the real MC libdbus
         * would exit in this situation, but in the debug build, we stay
         * active briefly) */
        dbus_connection = tp_proxy_get_dbus_connection (dbus_daemon);

        if (G_LIKELY (dbus_connection != NULL))
            dbus_g_connection_register_g_object (dbus_connection,
                                                 priv->object_path, object);

        g_object_unref (dbus_daemon);
    }

    return object;
error:
    g_object_unref (object);
    g_return_val_if_reached (NULL);
}

static void _mcd_dispatch_operation_lose_channel (McdDispatchOperation *self,
                                                  McdChannel *channel);

static void
mcd_dispatch_operation_channel_aborted_cb (McdChannel *channel,
                                           McdDispatchOperation *self)
{
    const GError *error;

    g_object_ref (self);    /* FIXME: use a GObject closure or something */

    DEBUG ("Channel %p aborted while in a dispatch operation", channel);

    /* if it was a channel request, and it was cancelled, then the whole
     * context should be aborted */
    error = mcd_channel_get_error (channel);
    if (error && error->code == TP_ERROR_CANCELLED)
        self->priv->cancelled = TRUE;

    _mcd_dispatch_operation_lose_channel (self, channel);

    if (_mcd_dispatch_operation_peek_channels (self) == NULL)
    {
        DEBUG ("Nothing left in this context");
    }

    g_object_unref (self);
}

static void
mcd_dispatch_operation_set_property (GObject *obj, guint prop_id,
                                     const GValue *val, GParamSpec *pspec)
{
    McdDispatchOperation *operation = MCD_DISPATCH_OPERATION (obj);
    McdDispatchOperationPrivate *priv = operation->priv;
    GList *list;

    switch (prop_id)
    {
    case PROP_CLIENT_REGISTRY:
        g_assert (priv->client_registry == NULL); /* construct-only */
        priv->client_registry = MCD_CLIENT_REGISTRY (g_value_dup_object (val));
        break;

    case PROP_HANDLER_MAP:
        g_assert (priv->handler_map == NULL); /* construct-only */
        priv->handler_map = MCD_HANDLER_MAP (g_value_dup_object (val));
        break;

    case PROP_CHANNELS:
        g_assert (priv->channels == NULL);
        priv->channels = g_list_copy (g_value_get_pointer (val));

        if (G_LIKELY (priv->channels))
        {
            /* get the connection and account from the first channel */
            McdChannel *channel = MCD_CHANNEL (priv->channels->data);

            priv->connection = (McdConnection *)
                mcd_mission_get_parent (MCD_MISSION (channel));

            if (G_LIKELY (priv->connection))
            {
                g_object_ref (priv->connection);
            }
            else
            {
                /* shouldn't happen? */
                g_warning ("Channel has no Connection?!");
            }

            priv->account = mcd_channel_get_account (channel);

            if (G_LIKELY (priv->account != NULL))
            {
                g_object_ref (priv->account);
            }
            else
            {
                /* shouldn't happen? */
                g_warning ("Channel given to McdDispatchOperation has no "
                           "Account?!");
            }

            /* reference the channels and connect to their signals */
            for (list = priv->channels; list != NULL; list = list->next)
            {
                g_object_ref (list->data);

                g_signal_connect_after (list->data, "abort",
                    G_CALLBACK (mcd_dispatch_operation_channel_aborted_cb),
                    operation);

            }
        }
        break;

    case PROP_POSSIBLE_HANDLERS:
        g_assert (priv->possible_handlers == NULL);
        priv->possible_handlers = g_value_dup_boxed (val);
        break;

    case PROP_NEEDS_APPROVAL:
        priv->needs_approval = g_value_get_boolean (val);
        break;

    case PROP_OBSERVE_ONLY:
        priv->observe_only = g_value_get_boolean (val);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
        break;
    }
}

static void
mcd_dispatch_operation_get_property (GObject *obj, guint prop_id,
                                     GValue *val, GParamSpec *pspec)
{
    McdDispatchOperationPrivate *priv = MCD_DISPATCH_OPERATION_PRIV (obj);

    switch (prop_id)
    {
    case PROP_CLIENT_REGISTRY:
        g_value_set_object (val, priv->client_registry);
        break;

    case PROP_HANDLER_MAP:
        g_value_set_object (val, priv->handler_map);
        break;

    case PROP_POSSIBLE_HANDLERS:
        g_value_set_boxed (val, priv->possible_handlers);
        break;

    case PROP_NEEDS_APPROVAL:
        g_value_set_boolean (val, priv->needs_approval);
        break;

    case PROP_OBSERVE_ONLY:
        g_value_set_boolean (val, priv->observe_only);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
        break;
    }
}

static void
mcd_dispatch_operation_finalize (GObject *object)
{
    McdDispatchOperationPrivate *priv = MCD_DISPATCH_OPERATION_PRIV (object);

    g_strfreev (priv->possible_handlers);
    priv->possible_handlers = NULL;

    if (priv->properties)
        g_hash_table_unref (priv->properties);

    if (priv->failed_handlers != NULL)
    {
        g_hash_table_unref (priv->failed_handlers);
    }

    g_free (priv->handler);
    g_free (priv->object_path);
    g_free (priv->claimer);

    G_OBJECT_CLASS (_mcd_dispatch_operation_parent_class)->finalize (object);
}

static void
mcd_dispatch_operation_dispose (GObject *object)
{
    McdDispatchOperationPrivate *priv = MCD_DISPATCH_OPERATION_PRIV (object);
    GList *list;

    if (priv->channels)
    {
        for (list = priv->channels; list != NULL; list = list->next)
        {
            g_signal_handlers_disconnect_by_func (list->data,
                mcd_dispatch_operation_channel_aborted_cb, object);
            g_object_unref (list->data);
        }

        g_list_free (priv->channels);
        priv->channels = NULL;
    }

    if (priv->lost_channels != NULL)
    {
        for (list = priv->lost_channels; list != NULL; list = list->next)
            g_object_unref (list->data);
        g_list_free (priv->lost_channels);
        priv->lost_channels = NULL;
    }

    if (priv->connection)
    {
        g_object_unref (priv->connection);
        priv->connection = NULL;
    }

    if (priv->account != NULL)
    {
        g_object_unref (priv->account);
        priv->account = NULL;
    }

    if (priv->handler_map != NULL)
    {
        g_object_unref (priv->handler_map);
        priv->handler_map = NULL;
    }

    if (priv->client_registry != NULL)
    {
        g_object_unref (priv->client_registry);
        priv->client_registry = NULL;
    }
    G_OBJECT_CLASS (_mcd_dispatch_operation_parent_class)->dispose (object);
}

static void
_mcd_dispatch_operation_class_init (McdDispatchOperationClass * klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    g_type_class_add_private (object_class,
                              sizeof (McdDispatchOperationPrivate));

    object_class->constructor = mcd_dispatch_operation_constructor;
    object_class->dispose = mcd_dispatch_operation_dispose;
    object_class->finalize = mcd_dispatch_operation_finalize;
    object_class->set_property = mcd_dispatch_operation_set_property;
    object_class->get_property = mcd_dispatch_operation_get_property;

    g_object_class_install_property (object_class, PROP_CLIENT_REGISTRY,
        g_param_spec_object ("client-registry", "Client registry",
            "Reference to a global registry of Telepathy clients",
            MCD_TYPE_CLIENT_REGISTRY,
            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
            G_PARAM_STATIC_STRINGS));

    g_object_class_install_property (object_class, PROP_HANDLER_MAP,
        g_param_spec_object ("handler-map", "Handler map",
            "Reference to a global map from handled channels to handlers",
            MCD_TYPE_HANDLER_MAP,
            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
            G_PARAM_STATIC_STRINGS));

    g_object_class_install_property (object_class, PROP_CHANNELS,
        g_param_spec_pointer ("channels", "channels", "channels",
                              G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class, PROP_POSSIBLE_HANDLERS,
        g_param_spec_boxed ("possible-handlers", "Possible handlers",
                            "Well-known bus names of possible handlers",
                            G_TYPE_STRV,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                            G_PARAM_STATIC_STRINGS));

    g_object_class_install_property (object_class, PROP_NEEDS_APPROVAL,
        g_param_spec_boolean ("needs-approval", "Needs approval?",
                              "TRUE if this CDO should run Approvers and "
                              "appear on D-Bus", FALSE,
                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                              G_PARAM_STATIC_STRINGS));

    g_object_class_install_property (object_class, PROP_OBSERVE_ONLY,
        g_param_spec_boolean ("observe-only", "Observe only?",
                              "TRUE if this CDO should stop dispatching "
                              "as soon as Observers have been run",
                              FALSE,
                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                              G_PARAM_STATIC_STRINGS));
}

static void
_mcd_dispatch_operation_init (McdDispatchOperation *operation)
{
    McdDispatchOperationPrivate *priv;

    priv = G_TYPE_INSTANCE_GET_PRIVATE ((operation),
                                        MCD_TYPE_DISPATCH_OPERATION,
                                        McdDispatchOperationPrivate);
    operation->priv = priv;

    /* initializes the interfaces */
    mcd_dbus_init_interfaces_instances (operation);
}

/*
 * _mcd_dispatch_operation_new:
 * @client_registry: the client registry.
 * @handler_map: the handler map
 * @channels: a #GList of #McdChannel elements to dispatch.
 * @possible_handlers: the bus names of possible handlers for these channels.
 *
 * Creates a #McdDispatchOperation. The #GList @channels will be no longer
 * valid after this function has been called.
 */
McdDispatchOperation *
_mcd_dispatch_operation_new (McdClientRegistry *client_registry,
                             McdHandlerMap *handler_map,
                             gboolean needs_approval,
                             gboolean observe_only,
                             GList *channels,
                             const gchar * const *possible_handlers)
{
    gpointer *obj;

    /* possible-handlers is only allowed to be NULL if we're only observing */
    g_return_val_if_fail (possible_handlers != NULL || observe_only, NULL);
    /* channels that we will only observe should not need approval - so at
     * least one must be false */
    g_return_val_if_fail (!(observe_only && needs_approval), NULL);

    obj = g_object_new (MCD_TYPE_DISPATCH_OPERATION,
                        "client-registry", client_registry,
                        "handler-map", handler_map,
                        "channels", channels,
                        "possible-handlers", possible_handlers,
                        "needs-approval", needs_approval,
                        "observe-only", observe_only,
                        NULL);

    return MCD_DISPATCH_OPERATION (obj);
}

/*
 * _mcd_dispatch_operation_get_path:
 * @operation: the #McdDispatchOperation.
 *
 * Returns: the D-Bus object path of @operation.
 */
const gchar *
_mcd_dispatch_operation_get_path (McdDispatchOperation *operation)
{
    g_return_val_if_fail (MCD_IS_DISPATCH_OPERATION (operation), NULL);
    return operation->priv->object_path;
}

/*
 * _mcd_dispatch_operation_get_properties:
 * @operation: the #McdDispatchOperation.
 *
 * Gets the immutable properties of @operation.
 *
 * Returns: a #GHashTable with the operation properties. The reference count is
 * not incremented.
 */
GHashTable *
_mcd_dispatch_operation_get_properties (McdDispatchOperation *operation)
{
    McdDispatchOperationPrivate *priv;

    g_return_val_if_fail (MCD_IS_DISPATCH_OPERATION (operation), NULL);
    priv = operation->priv;
    if (!priv->properties)
    {
        const McdDBusProp *property;

        priv->properties =
            g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify)tp_g_value_slice_free);

        for (property = dispatch_operation_properties;
             property->name != NULL;
             property++)
        {
            GValue *value;
            gchar *name;

            if (!property->getprop) continue;

            /* The Channels property is mutable, so cannot be returned
             * here */
            if (!tp_strdiff (property->name, "Channels")) continue;

            value = g_slice_new0 (GValue);
            property->getprop ((TpSvcDBusProperties *)operation,
                               property->name, value);
            name = g_strconcat (TP_IFACE_CHANNEL_DISPATCH_OPERATION, ".",
                                property->name, NULL);
            g_hash_table_insert (priv->properties, name, value);
        }
    }
    return priv->properties;
}

gboolean
_mcd_dispatch_operation_needs_approval (McdDispatchOperation *self)
{
    g_return_val_if_fail (MCD_IS_DISPATCH_OPERATION (self), FALSE);

    return self->priv->needs_approval;
}

/*
 * _mcd_dispatch_operation_is_finished:
 * @self: the #McdDispatchOperation.
 *
 * Returns: %TRUE if the operation has finished, %FALSE otherwise.
 */
gboolean
_mcd_dispatch_operation_is_finished (McdDispatchOperation *self)
{
    g_return_val_if_fail (MCD_IS_DISPATCH_OPERATION (self), FALSE);
    /* if we want to finish, and we can, then we have */
    return (self->priv->wants_to_finish &&
            mcd_dispatch_operation_may_finish (self));
}

static gboolean
mcd_dispatch_operation_check_handle_with (McdDispatchOperation *self,
                                          const gchar *handler_name,
                                          GError **error)
{
    g_return_val_if_fail (MCD_IS_DISPATCH_OPERATION (self), FALSE);

    if (self->priv->wants_to_finish)
    {
        DEBUG ("NotYours: already finished");
        g_set_error (error, TP_ERRORS, TP_ERROR_NOT_YOURS,
                     "CDO already finished");
        return FALSE;
    }

    if (handler_name == NULL || handler_name[0] == '\0')
    {
        /* no handler name given */
        return TRUE;
    }

    if (!g_str_has_prefix (handler_name, MCD_CLIENT_BASE_NAME) ||
        !tp_dbus_check_valid_bus_name (handler_name,
                                       TP_DBUS_NAME_TYPE_WELL_KNOWN, NULL))
    {
        DEBUG ("InvalidArgument: handler name %s is bad", handler_name);
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Invalid handler name");
        return FALSE;
    }

    return TRUE;
}

void
_mcd_dispatch_operation_approve (McdDispatchOperation *self)
{
    g_return_if_fail (MCD_IS_DISPATCH_OPERATION (self));

    DEBUG ("%s/%p", self->priv->unique_name, self);

    if (self->priv->ado_pending > 0
        || self->priv->awaiting_approval)
    {
        /* the existing channel is waiting for approval; but since the
         * same channel has been requested, the approval operation must
         * terminate */
        if (!mcd_dispatch_operation_check_handle_with (self, NULL, NULL))
        {
            return;
        }

        _mcd_dispatch_operation_finish (self);
    }
    else
    {
        _mcd_dispatch_operation_set_approved (self);
    }
}

static void
_mcd_dispatch_operation_lose_channel (McdDispatchOperation *self,
                                      McdChannel *channel)
{
    GList *li = g_list_find (self->priv->channels, channel);
    const gchar *object_path;

    if (li == NULL)
    {
        return;
    }

    self->priv->channels = g_list_delete_link (self->priv->channels, li);

    object_path = mcd_channel_get_object_path (channel);

    if (object_path == NULL)
    {
        /* This shouldn't happen, but McdChannel is twisty enough that I
         * can't be sure */
        g_critical ("McdChannel has already lost its TpChannel: %p",
            channel);
    }
    else if (!mcd_dispatch_operation_may_finish (self))
    {
        /* We're still invoking approvers, so we're not allowed to talk
         * about it right now. Instead, save the signal for later. */
        DEBUG ("%s/%p not losing channel %s just yet", self->priv->unique_name,
               self, object_path);
        self->priv->lost_channels =
            g_list_prepend (self->priv->lost_channels,
                            g_object_ref (channel));
    }
    else
    {
        const GError *error = mcd_channel_get_error (channel);
        gchar *error_name = _mcd_build_error_string (error);

        DEBUG ("%s/%p losing channel %s: %s: %s",
               self->priv->unique_name, self, object_path, error_name,
               error->message);
        tp_svc_channel_dispatch_operation_emit_channel_lost (self, object_path,
                                                             error_name,
                                                             error->message);
        g_free (error_name);
    }

    /* We previously had a ref in the linked list - drop it */
    g_object_unref (channel);

    if (self->priv->channels == NULL)
    {
        /* no channels left, so the CDO finishes (if it hasn't already) */
        _mcd_dispatch_operation_finish (self);
    }
}

static void
_mcd_dispatch_operation_check_finished (McdDispatchOperation *self)
{
    if (mcd_dispatch_operation_may_finish (self))
    {
        GList *lost_channels;

        /* get the lost channels into chronological order, and steal them from
         * the object*/
        lost_channels = g_list_reverse (self->priv->lost_channels);
        self->priv->lost_channels = NULL;

        while (lost_channels != NULL)
        {
            McdChannel *channel = lost_channels->data;
            const gchar *object_path = mcd_channel_get_object_path (channel);

            if (object_path == NULL)
            {
                /* This shouldn't happen, but McdChannel is twisty enough
                 * that I can't be sure */
                g_critical ("McdChannel has already lost its TpChannel: %p",
                    channel);
            }
            else
            {
                const GError *error = mcd_channel_get_error (channel);
                gchar *error_name = _mcd_build_error_string (error);

                DEBUG ("%s/%p losing channel %s: %s: %s",
                       self->priv->unique_name, self, object_path, error_name,
                       error->message);
                tp_svc_channel_dispatch_operation_emit_channel_lost (self,
                    object_path, error_name, error->message);
                g_free (error_name);
            }

            g_object_unref (channel);
            lost_channels = g_list_delete_link (lost_channels, lost_channels);
        }

        if (self->priv->wants_to_finish)
        {
            DEBUG ("%s/%p finished", self->priv->unique_name, self);
            mcd_dispatch_operation_actually_finish (self);
        }
    }
}

static void
_mcd_dispatch_operation_set_handler_failed (McdDispatchOperation *self,
                                            const gchar *bus_name)
{
    if (self->priv->failed_handlers == NULL)
    {
        self->priv->failed_handlers = g_hash_table_new_full (g_str_hash,
                                                             g_str_equal,
                                                             g_free, NULL);
    }

    /* the value is an arbitrary non-NULL pointer - the hash table itself
     * will do nicely */
    g_hash_table_insert (self->priv->failed_handlers, g_strdup (bus_name),
                         self->priv->failed_handlers);
}

static gboolean
_mcd_dispatch_operation_get_handler_failed (McdDispatchOperation *self,
                                            const gchar *bus_name)
{
    g_assert (MCD_IS_DISPATCH_OPERATION (self));
    g_assert (bus_name != NULL);

    if (self->priv->failed_handlers == NULL)
        return FALSE;

    return (g_hash_table_lookup (self->priv->failed_handlers, bus_name)
            != NULL);
}

static gboolean
_mcd_dispatch_operation_handlers_can_bypass_approval (
    McdDispatchOperation *self)
{
    gchar **iter;

    for (iter = self->priv->possible_handlers;
         iter != NULL && *iter != NULL;
         iter++)
    {
        McdClientProxy *handler = _mcd_client_registry_lookup (
            self->priv->client_registry, *iter);

        /* If the best handler that still exists bypasses approval, then
         * we're going to bypass approval.
         *
         * Also, because handlers are sorted with the best ones first, and
         * handlers with BypassApproval are "better", we can be sure that if
         * we've found a handler that still exists and does not bypass
         * approval, no handler bypasses approval. */
        if (handler != NULL)
        {
            gboolean bypass = _mcd_client_proxy_get_bypass_approval (
                handler);

            DEBUG ("%s has BypassApproval=%c", *iter, bypass ? 'T' : 'F');
            return bypass;
        }
    }

    /* If no handler still exists, we don't bypass approval, although if that
     * happens we're basically doomed anyway. */
    return FALSE;
}

static void
_mcd_dispatch_operation_set_approved (McdDispatchOperation *self)
{
    g_return_if_fail (MCD_IS_DISPATCH_OPERATION (self));
    self->priv->approved = TRUE;
    _mcd_dispatch_operation_check_client_locks (self);
}

gboolean
_mcd_dispatch_operation_has_channel (McdDispatchOperation *self,
                                     McdChannel *channel)
{
    g_return_val_if_fail (MCD_IS_DISPATCH_OPERATION (self), FALSE);
    return (g_list_find (self->priv->channels, channel) != NULL);
}

const GList *
_mcd_dispatch_operation_peek_channels (McdDispatchOperation *self)
{
    g_return_val_if_fail (MCD_IS_DISPATCH_OPERATION (self), NULL);
    return self->priv->channels;
}

GList *
_mcd_dispatch_operation_dup_channels (McdDispatchOperation *self)
{
    GList *copy;

    g_return_val_if_fail (MCD_IS_DISPATCH_OPERATION (self), NULL);
    copy = g_list_copy (self->priv->channels);
    g_list_foreach (copy, (GFunc) g_object_ref, NULL);
    return copy;
}

static void
_mcd_dispatch_operation_handle_channels_cb (TpClient *client,
                                            const GError *error,
                                            gpointer user_data,
                                            GObject *weak G_GNUC_UNUSED)
{
    McdDispatchOperation *self = user_data;

    if (error)
    {
        DEBUG ("error: %s", error->message);

        _mcd_dispatch_operation_set_handler_failed (self,
            tp_proxy_get_bus_name (client));

        /* try again */
        _mcd_dispatch_operation_run_handlers (self);
    }
    else
    {
        const GList *list;

        for (list = self->priv->channels; list != NULL; list = list->next)
        {
            McdChannel *channel = list->data;
            const gchar *unique_name;

            unique_name = _mcd_client_proxy_get_unique_name (MCD_CLIENT_PROXY (client));

            /* This should always be false in practice - either we already know
             * the handler's unique name (because active handlers' unique names
             * are discovered before their handler filters), or the handler
             * is activatable and was not running, the handler filter came
             * from a .client file, and the bus daemon activated the handler
             * as a side-effect of HandleChannels (in which case
             * NameOwnerChanged should have already been emitted by the time
             * we got a reply to HandleChannels).
             *
             * We recover by whining to stderr and closing the channels, in the
             * interests of at least failing visibly.
             *
             * If dbus-glib exposed more of the details of the D-Bus message
             * passing system, then we could just look at the sender of the
             * reply and bypass this rubbish...
             */
            if (G_UNLIKELY (unique_name == NULL || unique_name[0] == '\0'))
            {
                g_warning ("Client %s returned successfully but doesn't "
                           "exist? dbus-daemon bug suspected",
                           tp_proxy_get_bus_name (client));
                g_warning ("Closing channel %s as a result",
                           mcd_channel_get_object_path (channel));
                _mcd_channel_undispatchable (channel);
                continue;
            }

            mcd_dispatch_operation_set_channel_handled_by (self, channel,
                                                           unique_name);
        }

        /* emit Finished, if we haven't already */
        _mcd_dispatch_operation_finish (self);
    }
}

static void
observe_channels_cb (TpClient *proxy, const GError *error,
                     gpointer user_data, GObject *weak_object)
{
    McdDispatchOperation *self = user_data;

    /* we display the error just for debugging, but we don't really care */
    if (error)
        DEBUG ("Observer %s returned error: %s",
               tp_proxy_get_object_path (proxy), error->message);
    else
        DEBUG ("success from %s", tp_proxy_get_object_path (proxy));

    _mcd_dispatch_operation_dec_observers_pending (self);
}

/* The returned GPtrArray is allocated, but the contents are borrowed. */
static GPtrArray *
collect_satisfied_requests (GList *channels)
{
    const GList *c, *r;
    GHashTable *set = g_hash_table_new (g_str_hash, g_str_equal);
    GHashTableIter iter;
    gpointer path;
    GPtrArray *ret;

    /* collect object paths into a hash table, to drop duplicates */
    for (c = channels; c != NULL; c = c->next)
    {
        const GList *reqs = _mcd_channel_get_satisfied_requests (c->data,
                                                                 NULL);

        for (r = reqs; r != NULL; r = r->next)
        {
            g_hash_table_insert (set, r->data, r->data);
        }
    }

    /* serialize them into a pointer array, which is what dbus-glib wants */
    ret = g_ptr_array_sized_new (g_hash_table_size (set));

    g_hash_table_iter_init (&iter, set);

    while (g_hash_table_iter_next (&iter, &path, NULL))
    {
        g_ptr_array_add (ret, path);
    }

    g_hash_table_destroy (set);

    return ret;
}

static void
_mcd_dispatch_operation_run_observers (McdDispatchOperation *self)
{
    const GList *cl;
    const gchar *dispatch_operation_path = "/";
    GHashTable *observer_info;
    GHashTableIter iter;
    gpointer client_p;

    observer_info = g_hash_table_new (g_str_hash, g_str_equal);

    _mcd_client_registry_init_hash_iter (self->priv->client_registry, &iter);

    while (g_hash_table_iter_next (&iter, NULL, &client_p))
    {
        McdClientProxy *client = MCD_CLIENT_PROXY (client_p);
        GList *observed = NULL;
        const gchar *account_path, *connection_path;
        GPtrArray *channels_array, *satisfied_requests;

        if (!tp_proxy_has_interface_by_id (client,
                                           TP_IFACE_QUARK_CLIENT_OBSERVER))
            continue;

        for (cl = self->priv->channels; cl != NULL; cl = cl->next)
        {
            McdChannel *channel = MCD_CHANNEL (cl->data);
            GHashTable *properties;

            properties = _mcd_channel_get_immutable_properties (channel);
            g_assert (properties != NULL);

            if (_mcd_client_match_filters (properties,
                _mcd_client_proxy_get_observer_filters (client),
                FALSE))
                observed = g_list_prepend (observed, channel);
        }
        if (!observed) continue;

        /* build up the parameters and invoke the observer */

        connection_path = _mcd_dispatch_operation_get_connection_path (self);
        account_path = _mcd_dispatch_operation_get_account_path (self);

        /* TODO: there's room for optimization here: reuse the channels_array,
         * if the observed list is the same */
        channels_array = _mcd_channel_details_build_from_list (observed);

        satisfied_requests = collect_satisfied_requests (observed);

        if (_mcd_dispatch_operation_needs_approval (self))
        {
            dispatch_operation_path = _mcd_dispatch_operation_get_path (self);
        }

        _mcd_dispatch_operation_inc_observers_pending (self);

        DEBUG ("calling ObserveChannels on %s for CDO %p",
               tp_proxy_get_bus_name (client), self);
        tp_cli_client_observer_call_observe_channels (
            (TpClient *) client, -1,
            account_path, connection_path, channels_array,
            dispatch_operation_path, satisfied_requests, observer_info,
            observe_channels_cb,
            g_object_ref (self), g_object_unref, NULL);

        /* don't free the individual object paths, which are borrowed from the
         * McdChannel objects */
        g_ptr_array_free (satisfied_requests, TRUE);

        _mcd_channel_details_free (channels_array);

        g_list_free (observed);
    }

    g_hash_table_destroy (observer_info);
}

static void
add_dispatch_operation_cb (TpClient *proxy,
                           const GError *error,
                           gpointer user_data,
                           GObject *weak_object)
{
    McdDispatchOperation *self = user_data;

    if (error)
    {
        DEBUG ("AddDispatchOperation %s (%p) on approver %s failed: "
               "%s",
               _mcd_dispatch_operation_get_path (self), self,
               tp_proxy_get_object_path (proxy), error->message);
    }
    else
    {
        DEBUG ("Approver %s accepted AddDispatchOperation %s (%p)",
               tp_proxy_get_object_path (proxy),
               _mcd_dispatch_operation_get_path (self), self);

        if (!self->priv->awaiting_approval)
        {
            self->priv->awaiting_approval = TRUE;
        }
    }

    /* If all approvers fail to add the DO, then we behave as if no
     * approver was registered: i.e., we continue dispatching. If at least
     * one approver accepted it, then we can still continue dispatching,
     * since it will be stalled until awaiting_approval becomes FALSE. */
    _mcd_dispatch_operation_dec_ado_pending (self);
}

static void
_mcd_dispatch_operation_run_approvers (McdDispatchOperation *self)
{
    const GList *cl;
    GHashTableIter iter;
    gpointer client_p;

    /* we temporarily increment this count and decrement it at the end of the
     * function, to make sure it won't become 0 while we are still invoking
     * approvers */
    _mcd_dispatch_operation_inc_ado_pending (self);

    _mcd_client_registry_init_hash_iter (self->priv->client_registry, &iter);
    while (g_hash_table_iter_next (&iter, NULL, &client_p))
    {
        McdClientProxy *client = MCD_CLIENT_PROXY (client_p);
        GPtrArray *channel_details;
        const gchar *dispatch_operation;
        GHashTable *properties;
        gboolean matched = FALSE;

        if (!tp_proxy_has_interface_by_id (client,
                                           TP_IFACE_QUARK_CLIENT_APPROVER))
            continue;

        for (cl = self->priv->channels; cl != NULL; cl = cl->next)
        {
            McdChannel *channel = MCD_CHANNEL (cl->data);
            GHashTable *channel_properties;

            channel_properties = _mcd_channel_get_immutable_properties (channel);
            g_assert (channel_properties != NULL);

            if (_mcd_client_match_filters (channel_properties,
                _mcd_client_proxy_get_approver_filters (client),
                FALSE))
            {
                matched = TRUE;
                break;
            }
        }
        if (!matched) continue;

        dispatch_operation = _mcd_dispatch_operation_get_path (self);
        properties = _mcd_dispatch_operation_get_properties (self);
        channel_details =
            _mcd_channel_details_build_from_list (self->priv->channels);

        DEBUG ("Calling AddDispatchOperation on approver %s for CDO %s @ %p",
               tp_proxy_get_bus_name (client), dispatch_operation, self);

        _mcd_dispatch_operation_inc_ado_pending (self);

        tp_cli_client_approver_call_add_dispatch_operation (
            (TpClient *) client, -1,
            channel_details, dispatch_operation, properties,
            add_dispatch_operation_cb,
            g_object_ref (self), g_object_unref, NULL);

        g_boxed_free (TP_ARRAY_TYPE_CHANNEL_DETAILS_LIST, channel_details);
    }

    /* This matches the approvers count set to 1 at the beginning of the
     * function */
    _mcd_dispatch_operation_dec_ado_pending (self);
}

void
_mcd_dispatch_operation_run_clients (McdDispatchOperation *self)
{
    g_object_ref (self);

    _mcd_dispatch_operation_run_observers (self);

    /* if the dispatch operation thinks the channels were not
     * requested, start the Approvers */
    if (_mcd_dispatch_operation_needs_approval (self))
    {
        /* but if the handlers have the BypassApproval flag set, then don't
         *
         * FIXME: we should really run BypassApproval handlers as a separate
         * stage, rather than considering the existence of a BypassApproval
         * handler to constitute approval - this is fd.o #23687 */
        if (_mcd_dispatch_operation_handlers_can_bypass_approval (self))
            _mcd_dispatch_operation_set_approved (self);

        if (!_mcd_dispatch_operation_is_approved (self))
            _mcd_dispatch_operation_run_approvers (self);
    }

    self->priv->invoked_early_clients = TRUE;
    _mcd_dispatch_operation_check_client_locks (self);

    g_object_unref (self);
}

/*
 * mcd_dispatch_operation_handle_channels:
 * @self: the dispatch operation
 * @handler: the selected handler
 *
 * Invoke the handler for the given channels.
 */
static void
mcd_dispatch_operation_handle_channels (McdDispatchOperation *self,
                                        McdClientProxy *handler)
{
    const gchar *account_path;
    GHashTable *handler_info;
    const GList *cl;

    account_path = _mcd_dispatch_operation_get_account_path (self);

    for (cl = self->priv->channels; cl != NULL; cl = cl->next)
    {
        _mcd_channel_set_status (cl->data,
                                 MCD_CHANNEL_STATUS_HANDLER_INVOKED);
    }

    handler_info = g_hash_table_new (g_str_hash, g_str_equal);

    DEBUG ("calling HandleChannels on %s for op %p",
           tp_proxy_get_bus_name (handler), self);
    _mcd_client_proxy_handle_channels (handler,
        -1, account_path,
        self->priv->channels, self->priv->handle_with_time,
        handler_info, _mcd_dispatch_operation_handle_channels_cb,
        g_object_ref (self), g_object_unref, NULL);

    g_hash_table_unref (handler_info);
}

void
_mcd_dispatch_operation_run_handlers (McdDispatchOperation *self)
{
    GList *channels, *list;
    gchar **iter;

    /* If there is an approved handler chosen by the Approver, it's the only
     * one we'll consider. */

    if (self->priv->handler != NULL && self->priv->handler[0] != '\0')
    {
        gchar *bus_name = g_strconcat (TP_CLIENT_BUS_NAME_BASE,
                                       self->priv->handler, NULL);
        McdClientProxy *handler = _mcd_client_registry_lookup (
            self->priv->client_registry, bus_name);
        gboolean failed = _mcd_dispatch_operation_get_handler_failed (self,
            bus_name);

        DEBUG ("Approved handler is %s (still exists: %c, "
               "already failed: %c)", bus_name,
               handler != NULL ? 'Y' : 'N',
               failed ? 'Y' : 'N');

        g_free (bus_name);

        /* Maybe the handler has exited since we chose it, or maybe we
         * already tried it? Otherwise, it's the right choice. */
        if (handler != NULL && !failed)
        {
            mcd_dispatch_operation_handle_channels (self, handler);
            return;
        }

        /* The approver asked for a particular handler, but that handler
         * has vanished. If MC was fully spec-compliant, it wouldn't have
         * replied to the Approver yet, so it could just return an error.
         * However, that particular part of the flying-car future has not
         * yet arrived, so try to recover by dispatching to *something*. */
    }

    for (iter = self->priv->possible_handlers;
         iter != NULL && *iter != NULL;
         iter++)
    {
        McdClientProxy *handler = _mcd_client_registry_lookup (
            self->priv->client_registry, *iter);
        gboolean failed = _mcd_dispatch_operation_get_handler_failed
            (self, *iter);

        DEBUG ("Possible handler: %s (still exists: %c, already failed: %c)",
               *iter, handler != NULL ? 'Y' : 'N', failed ? 'Y' : 'N');

        if (handler != NULL && !failed)
        {
            mcd_dispatch_operation_handle_channels (self, handler);
            return;
        }
    }

    /* All of the usable handlers vanished while we were thinking about it
     * (this can only happen if non-activatable handlers exit after we
     * include them in the list of possible handlers, but before we .
     * We should recover in some better way, perhaps by asking all the
     * approvers again (?), but for now we'll just close all the channels. */

    DEBUG ("No possible handler still exists, giving up");

    channels = _mcd_dispatch_operation_dup_channels (self);

    for (list = channels; list != NULL; list = list->next)
    {
        McdChannel *channel = MCD_CHANNEL (list->data);
        GError e = { MC_ERROR, MC_CHANNEL_REQUEST_GENERIC_ERROR,
            "Handler no longer available" };

        mcd_channel_take_error (channel, g_error_copy (&e));
        _mcd_channel_undispatchable (channel);
        g_object_unref (channel);
    }

    g_list_free (channels);
}
