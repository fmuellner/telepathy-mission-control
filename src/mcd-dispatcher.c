/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 8 -*- */
/*
 * This file is part of mission-control
 *
 * Copyright (C) 2007 Nokia Corporation. 
 *
 * Contact: Naba Kumar  <naba.kumar@nokia.com>
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

/**
 * SECTION:mcd-dispatcher
 * @title: McdDispatcher
 * @short_description: Dispatcher class to dispatch channels to handlers
 * @see_also: 
 * @stability: Unstable
 * @include: mcd-dispatcher.h
 * 
 * FIXME
 */

#include <dlfcn.h>
#include <glib/gi18n.h>
#include <libtelepathy/tp-chan-iface-group-gen.h>
#include <libtelepathy/tp-ch-gen.h>

#include "mcd-signals-marshal.h"
#include "mcd-connection.h"
#include "mcd-channel.h"
#include "mcd-master.h"
#include "mcd-chan-handler.h"
#include "mcd-dispatcher-context.h"

#define MCD_DISPATCHER_PRIV(dispatcher) (G_TYPE_INSTANCE_GET_PRIVATE ((dispatcher), \
				  MCD_TYPE_DISPATCHER, \
				  McdDispatcherPrivate))

G_DEFINE_TYPE (McdDispatcher, mcd_dispatcher, MCD_TYPE_MISSION);

struct _McdDispatcherContext
{
    McdDispatcher *dispatcher;
    
    /*The actual channel */
    McdChannel *channel;

    /* State-machine internal data fields: */
    GList *chain;

    /* Next function in chain */
    guint next_func_index;
};

typedef struct _McdDispatcherPrivate
{
    /* Pending state machine contexts */
    GSList *state_machine_list;
    
    /* All active channels */
    GList *channels;
    
    GSList *filter_dlhandles;
    gchar *plugin_dir;
    GData *interface_filters;
    DBusGConnection *dbus_connection;

    /* Channel handlers */
    GHashTable *channel_handler_hash;
    /* Array of channel handler's capabilities, stored as a GPtrArray for
     * performance reasons */
    GPtrArray *channel_handler_caps;
    
    McdMaster *master;
 
    gboolean is_disposed;
    
} McdDispatcherPrivate;

struct iface_chains_t
{
    GList *chain_in;
    GList *chain_out;
};

enum
{
    PROP_0,
    PROP_PLUGIN_DIR,
    PROP_DBUS_CONNECTION,
    PROP_MCD_MASTER,
};

enum _McdDispatcherSignalType
{
    CHANNEL_ADDED,
    CHANNEL_REMOVED,
    DISPATCHED,
    DISPATCH_FAILED,
    LAST_SIGNAL
};

static guint mcd_dispatcher_signals[LAST_SIGNAL] = { 0 };

static void mcd_dispatcher_context_free (McdDispatcherContext * ctx);

static void
_mcd_dispatcher_load_filters (McdDispatcher * dispatcher)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (dispatcher);
    GDir *dir = NULL;
    GError *error = NULL;
    void *plugin_handle;
    const gchar *name;

    dir = g_dir_open (priv->plugin_dir, 0, &error);
    if (!dir)
    {
	g_debug ("Could not open plugin directory: %s", error->message);
	g_error_free (error);
	return;
    }

    while ((name = g_dir_read_name (dir)))
    {
	plugin_handle = NULL;
	gchar *path = NULL;
	path = g_strconcat (priv->plugin_dir, G_DIR_SEPARATOR_S, name, NULL);
	/* Skip directories */

	if (g_file_test (path, G_FILE_TEST_IS_DIR))
	{
	    g_free (path);
	    continue;
	}
	/* Is it a library? If yes, add the name to list */

	if (!g_str_has_suffix (path, ".so"))
	{
	    g_free (path);
	    continue;
	}

	/* ? Do we need to check more strictly than by using prefix-check?
	 * Probably not, as failure of dlopen will take care of things
	 * anyway? */

	plugin_handle = dlopen (path, RTLD_NOW);

	if (plugin_handle != NULL)
	{
	    void (*plugin_init) (McdDispatcher * dispatcher);

	    priv->filter_dlhandles = g_slist_prepend (priv->filter_dlhandles,
						      plugin_handle);

	    plugin_init = dlsym (plugin_handle, MCD_PLUGIN_INIT_FUNC);
	    if (plugin_init != NULL)
	    {
		plugin_init (dispatcher);
	    }
	    else
	    {
		g_debug ("Error opening filter plugin: %s: %s", path,
			 dlerror ());
	    }
	}
	else
	{
	    g_debug ("Could not open plugin %s because: %s", path, dlerror ());
	}
	g_free (path);
    }
    g_dir_close (dir);

    return;
}

static void
_mcd_dispatcher_unload_filters (McdDispatcher * dispatcher)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (dispatcher);

    if (priv->filter_dlhandles)
    {
	g_slist_foreach (priv->filter_dlhandles, (GFunc) dlclose, NULL);
	g_slist_free (priv->filter_dlhandles);
	priv->filter_dlhandles = NULL;

	g_datalist_clear (&priv->interface_filters);
	priv->interface_filters = NULL;
    }
}

/* REGISTRATION/DEREGISTRATION of filters*/

/* A convenience function for acquiring the chain for particular channel
type and filter flag combination. */

static GList *
_mcd_dispatcher_get_filter_chain (McdDispatcher * dispatcher,
				  GQuark channel_type_quark,
				  guint filter_flags)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (dispatcher);
    struct iface_chains_t *iface_chains;
    GList *filter_chain = NULL;

    iface_chains =
	(struct iface_chains_t *)
	g_datalist_id_get_data (&(priv->interface_filters), channel_type_quark);

    if (iface_chains == NULL)
    {
	g_debug ("%s: No chains for interface %s", G_STRFUNC,
		 g_quark_to_string (channel_type_quark));
    }
    else
	switch (filter_flags)
	{
	case MCD_FILTER_IN:
	    filter_chain = iface_chains->chain_in;
	    break;
	case MCD_FILTER_OUT:
	    filter_chain = iface_chains->chain_out;
	    break;

	default:
	    g_warning ("Unsupported filter flag value");
	    break;
	}

    return filter_chain;
}

static GList *
chain_add_filter (GList *chain, McdFilterFunc filter, guint priority)
{
    GList *elem;
    McdFilter *filter_data;

    filter_data = g_malloc (sizeof (McdFilter));
    filter_data->func = filter;
    filter_data->priority = priority;
    for (elem = chain; elem; elem = elem->next)
	if (((McdFilter *)elem->data)->priority >= priority) break;

    return g_list_insert_before (chain, elem, filter_data);
}

static GList *
chain_remove_filter (GList *chain, McdFilterFunc func)
{
    GList *elem, *new_chain = NULL;

    /* since in-place modification of a list is error prone (especially if the
     * same filter has been registered in the same chain with different
     * priorities), we build a new list with the remaining elements */
    for (elem = chain; elem; elem = elem->next)
    {
	if (((McdFilter *)elem->data)->func == func)
	    g_free (elem->data);
	else
	    new_chain = g_list_append (new_chain, elem->data);
    }
    g_list_free (chain);

    return new_chain;
}

static void
free_filter_chains (struct iface_chains_t *chains)
{
    if (chains->chain_in)
    {
	g_list_foreach (chains->chain_in, (GFunc)g_free, NULL);
	g_list_free (chains->chain_in);
    }
    if (chains->chain_out)
    {
	g_list_foreach (chains->chain_out, (GFunc)g_free, NULL);
	g_list_free (chains->chain_out);
    }
    g_free (chains);
}

/**
 * mcd_dispatcher_register_filter:
 * @dispatcher: The #McdDispatcher.
 * @filter: the filter function to be registered.
 * @channel_type_quark: Quark indicating the channel type.
 * @filter_flags: The flags for the filter, such as incoming/outgoing.
 * @priority: The priority of the filter.
 *
 * Indicates to Mission Control that we want to register a filter for a unique
 * combination of channel type/filter flags.
 */
void
mcd_dispatcher_register_filter (McdDispatcher *dispatcher,
			       	McdFilterFunc filter,
				GQuark channel_type_quark,
				guint filter_flags, guint priority)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (dispatcher);
    struct iface_chains_t *iface_chains = NULL;

    /* Check if the interface already has stored data, otherwise create it */

    if (!(iface_chains = g_datalist_id_get_data (&(priv->interface_filters),
						 channel_type_quark)))
    {
	iface_chains = g_new0 (struct iface_chains_t, 1);
	g_datalist_id_set_data_full (&(priv->interface_filters),
				     channel_type_quark, iface_chains,
				     (GDestroyNotify)free_filter_chains);
    }

    switch (filter_flags)
    {
    case MCD_FILTER_IN:
	iface_chains->chain_in = chain_add_filter (iface_chains->chain_in,
						   filter, priority);
	break;
    case MCD_FILTER_OUT:
	iface_chains->chain_out = chain_add_filter (iface_chains->chain_out,
						    filter, priority);
	break;
    default:
	g_warning ("Unknown filter flag value!");
    }
}

/**
 * mcd_dispatcher_unregister_filter:
 * @dispatcher: The #McdDispatcher.
 * @filter: the filter function to be registered.
 * @channel_type_quark: Quark indicating the channel type.
 * @filter_flags: The flags for the filter, such as incoming/outgoing.
 *
 * Indicates to Mission Control that we will not want to have a filter
 * for particular unique channel type/filter flags combination anymore.
 */
void
mcd_dispatcher_unregister_filter (McdDispatcher * dispatcher,
				  McdFilterFunc filter,
				  GQuark channel_type_quark,
				  guint filter_flags)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (dispatcher);

    /* First, do we have anything registered for that channel type? */
    struct iface_chains_t *chains =
	(struct iface_chains_t *)
	g_datalist_id_get_data (&(priv->interface_filters),
				channel_type_quark);
    if (chains == NULL)
    {
	g_warning ("Attempting to unregister from an empty filter chain");
	return;
    }

    switch (filter_flags)
    {
    case MCD_FILTER_IN:
	/* No worries about memory leaks, as these are function pointers */
	chains->chain_in = chain_remove_filter(chains->chain_in, filter);
	break;
    case MCD_FILTER_OUT:
	chains->chain_out = chain_remove_filter(chains->chain_out, filter);
	break;
    default:
	g_warning ("Unknown filter flag value!");
    }

    /* Both chains are empty? We may as well free the struct then */

    if (chains->chain_in == NULL && chains->chain_out == NULL)
    {
	/* ? Should we dlclose the plugin as well..? */
	g_datalist_id_remove_data (&(priv->interface_filters),
				   channel_type_quark);
    }

    return;
}

/**
 * mcd_dispatcher_register_filters:
 * @dispatcher: The #McdDispatcher.
 * @filters: a zero-terminated array of #McdFilter elements.
 * @channel_type_quark: Quark indicating the channel type.
 * @filter_flags: The flags for the filter, such as incoming/outgoing.
 *
 * Convenience function to register a batch of filters at once.
 */
void
mcd_dispatcher_register_filters (McdDispatcher *dispatcher,
				 McdFilter *filters,
				 GQuark channel_type_quark,
				 guint filter_flags)
{
    McdFilter *filter;

    g_return_if_fail (filters != NULL);

    for (filter = filters; filter->func != NULL; filter++)
	mcd_dispatcher_register_filter (dispatcher, filter->func,
				       	channel_type_quark,
					filter_flags, filter->priority);
}

/* Returns # of times particular channel type  has been used */
gint
mcd_dispatcher_get_channel_type_usage (McdDispatcher * dispatcher,
				       GQuark chan_type_quark)
{
    GList *node;
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (dispatcher);
    gint usage_counter = 0;
    
    node = priv->channels;
    while (node)
    {
	McdChannel *chan = (McdChannel*) node->data;
	if (chan && chan_type_quark == mcd_channel_get_channel_type_quark (chan))
	    usage_counter++;
	node = node->next;
    }

    return usage_counter;
}

/* The callback is called on channel Closed signal */
static void
on_channel_abort_list (McdChannel *channel, McdDispatcher *dispatcher)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (dispatcher);
    
    g_debug ("Abort Channel; Removing channel from list");
    priv->channels = g_list_remove (priv->channels, channel);
    g_signal_emit_by_name (dispatcher, "channel-removed", channel);
    g_object_unref (channel);
}

static void
on_master_abort (McdMaster *master, McdDispatcherPrivate *priv)
{
    g_object_unref (master);
    priv->master = NULL;
}

/* CHANNEL HANDLING */

/* Ensure that when the channelhandler dies, the channels do not be
 * left around (e.g. when VOIP UI dies, the call used to hang
 * around)
 */
static void
_mcd_dispatcher_channel_handler_destroy_cb (DBusGProxy * channelhandler,
					   gpointer userdata)
{
    McdChannel *channel;

    /* If the channel has already been destroyed, do not bother doing
     * anything. */
    if (!userdata || !(G_IS_OBJECT (userdata)) || !(MCD_IS_CHANNEL (userdata)))
    {
	g_debug ("Channel has already been closed. No need to clean up.");
	return;
    }

    channel = MCD_CHANNEL (userdata);

    g_debug ("Channelhandler object been destroyed, chan still valid.");
    mcd_mission_abort (MCD_MISSION (channel));
}

static void
disconnect_proxy_destry_cb (McdChannel *channel, DBusGProxy *channelhandler)
{
    g_signal_handlers_disconnect_by_func (channelhandler,
				    _mcd_dispatcher_channel_handler_destroy_cb,
				    channel);
    g_object_unref (channelhandler);
}

static void
cancel_proxy_call (McdChannel *channel, DBusGProxyCall *call)
{
    DBusGProxy *proxy = g_object_steal_data (G_OBJECT (channel),
					     "cancel_proxy_call_userdata");
    dbus_g_proxy_cancel_call (proxy, call);
}

static void
_mcd_dispatcher_handle_channel_async_cb (DBusGProxy * proxy, GError * error,
					 gpointer userdata)
{
    McdDispatcherContext *context = userdata;
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (context->dispatcher);
    McdChannel *channel;

    channel = mcd_dispatcher_context_get_channel (context);
    McdChannelHandler *chandler = g_hash_table_lookup (priv->channel_handler_hash,
						    mcd_channel_get_channel_type (channel));

    g_object_steal_data (G_OBJECT (channel), "cancel_proxy_call_userdata");
    g_signal_handlers_disconnect_matched (channel, G_SIGNAL_MATCH_FUNC,	0, 0,
					  NULL, cancel_proxy_call, NULL);

    /* We'll no longer need this proxy instance. */
    if (proxy && DBUS_IS_G_PROXY (proxy))
    {
	g_object_unref (proxy);
    }

    if (error != NULL)
    {
	GError *mc_error = NULL;
	
	g_warning ("Handle channel failed: %s", error->message);
	
	/* We can't reliably map channel handler error codes to MC error
	 * codes. So just using generic error message.
	 */
	mc_error = g_error_new (MC_ERROR, MC_CHANNEL_REQUEST_GENERIC_ERROR,
				"Handle channel failed: %s", error->message);
	
	g_signal_emit_by_name (context->dispatcher, "dispatch-failed",
			       context->channel, mc_error);
	
	g_error_free (mc_error);
	g_error_free (error);
	if (context->channel)
	    mcd_mission_abort (MCD_MISSION (context->channel));
	return;
    }

    /* In case the VOIP channel handler dies unexpectedly, we
     * may end up in very confused state if we do
     * nothing. Thus, we'll try to handle the death */
    
    if (mcd_channel_get_channel_type_quark (channel)
	== TELEPATHY_CHAN_IFACE_STREAMED_QUARK)
    {
	const McdConnection *connection;
	DBusGConnection *dbus_connection;
	GError *unique_proxy_error = NULL;
	
	connection = mcd_dispatcher_context_get_connection (context);
        g_object_get (G_OBJECT (connection), "dbus-connection", &dbus_connection, NULL);
	g_debug ("Aha! A streamed media channel, need to guard it.");
	
	DBusGProxy *unique_name_proxy =
	    dbus_g_proxy_new_for_name_owner (dbus_connection,
					     chandler->bus_name,
					     chandler->obj_path,
					     "org.freedesktop.Telepathy.ChannelHandler",
					     &unique_proxy_error);
	if (unique_proxy_error == NULL)
	{
	    g_debug ("Adding the destroy handler support.");
	    g_signal_connect (unique_name_proxy,
			      "destroy",
			      G_CALLBACK (_mcd_dispatcher_channel_handler_destroy_cb),
			      context->channel);
	    g_signal_connect (context->channel, "abort",
			      G_CALLBACK(disconnect_proxy_destry_cb),
			      unique_name_proxy);
	}
    }

    g_signal_emit_by_name (context->dispatcher, "dispatched", channel);
    mcd_dispatcher_context_free (context);
}

/* Happens at the end of successful filter chain execution (empty chain
 * is always successful)
 */
static void
_mcd_dispatcher_start_channel_handler (McdDispatcherContext * context)
{
    McdChannelHandler *chandler;
    McdDispatcherPrivate *priv;
    McdChannel *channel;

    g_return_if_fail (context);

    priv = MCD_DISPATCHER_PRIV (context->dispatcher);
    channel = mcd_dispatcher_context_get_channel (context); 

    /* we need to know where's the channel handler and queue */
    /* drop from the queue */
    /*FIXME: Use Quarks in hashtable */
    chandler =
	g_hash_table_lookup (priv->channel_handler_hash,
			     mcd_channel_get_channel_type (channel));
    if (chandler == NULL)
    {
	GError *mc_error;
	g_debug ("No handler for channel type %s",
		 mcd_channel_get_channel_type (channel));
	
	mc_error = g_error_new (MC_ERROR, MC_CHANNEL_REQUEST_GENERIC_ERROR,
				"No handler for channel type %s",
				mcd_channel_get_channel_type (channel));
	g_signal_emit_by_name (context->dispatcher, "dispatch-failed", channel,
			       mc_error);
	g_error_free (mc_error);
    }
    else
    {
	DBusGProxyCall *call;
	TpConn *tp_conn;
	
	const McdConnection *connection = mcd_dispatcher_context_get_connection (context);
	DBusGConnection *dbus_connection;

        g_object_get (G_OBJECT (connection),
                      "dbus-connection", &dbus_connection,
                      "tp-connection", &tp_conn, NULL);

	DBusGProxy *handler_proxy = dbus_g_proxy_new_for_name (dbus_connection,
							       chandler->bus_name,
							       chandler->obj_path,
				"org.freedesktop.Telepathy.ChannelHandler");
	
	g_debug ("Starting chan handler (bus = %s, obj = '%s'): conn = %s, chan_type = %s,"
		 " obj_path = %s, handle_type = %d, handle = %d",
		 chandler->bus_name,
		 chandler->obj_path,
		 dbus_g_proxy_get_path (DBUS_G_PROXY (tp_conn)),
		 mcd_channel_get_channel_type (channel),
		 mcd_channel_get_object_path (channel),
		 mcd_channel_get_handle_type (channel),
		 mcd_channel_get_handle (channel));
		 
	call = tp_ch_handle_channel_async (handler_proxy,
				    /*Connection bus */
				    dbus_g_proxy_get_bus_name (DBUS_G_PROXY
							       (tp_conn)),
				    /*Connection path */
				    dbus_g_proxy_get_path (DBUS_G_PROXY
							   (tp_conn)),
				    /*Channel type */
				    mcd_channel_get_channel_type (channel),
				    /*Object path */
				    mcd_channel_get_object_path (channel),
				    mcd_channel_get_handle_type (channel),
				    mcd_channel_get_handle (channel),
				    _mcd_dispatcher_handle_channel_async_cb,
				    context);
	g_object_set_data (G_OBJECT (channel), "cancel_proxy_call_userdata",
			   handler_proxy);
	g_signal_connect (channel, "abort", G_CALLBACK(cancel_proxy_call),
			  call);
        g_object_unref (tp_conn);
    }
}

static void
_mcd_dispatcher_drop_channel_handler (McdDispatcherContext * context)
{
    g_return_if_fail(context);
    
    /* drop from the queue and close channel */
    
    /* FIXME: The queue functionality is still missing. Add support for
    it, once it's available. */
    
    if (context->channel != NULL)
    {
	/* Context will be destroyed on this emission, so be careful
	 * not to access it after this.
	 */
	mcd_mission_abort (MCD_MISSION (context->channel));
    }
}

/* STATE MACHINE */

static void
_mcd_dispatcher_leave_state_machine (McdDispatcherContext * context)
{
    McdDispatcherPrivate *priv =
	MCD_DISPATCHER_PRIV (context->dispatcher);

    /* _mcd_dispatcher_drop_channel_handler (context); */

    priv->state_machine_list =
	g_slist_remove (priv->state_machine_list, context);

    mcd_dispatcher_context_free (context);
}

static void
on_channel_abort_context (McdChannel *channel, McdDispatcherContext *context)
{
    g_debug ("Abort Channel; Destroying state machine context.");
    _mcd_dispatcher_leave_state_machine (context);
}

/* Entering the state machine */
static void
_mcd_dispatcher_enter_state_machine (McdDispatcher *dispatcher,
				     McdChannel *channel)
{
    McdDispatcherContext *context;
    GList *chain;
    GQuark chan_type_quark;
    gboolean outgoing;
    gint filter_flags;
    
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (dispatcher);
    
    g_object_get (G_OBJECT (channel),
		  "channel-type-quark", &chan_type_quark,
		  "outgoing", &outgoing,
		  NULL);

    filter_flags = outgoing ? MCD_FILTER_OUT: MCD_FILTER_IN;
    chain = _mcd_dispatcher_get_filter_chain (dispatcher,
					      chan_type_quark,
					      filter_flags);
    
    /* Preparing and filling the context */
    context = g_new0 (McdDispatcherContext, 1);
    context->dispatcher = dispatcher;
    context->channel = channel;
    context->chain = chain;

    /* Context must be destroyed when the channel is destroyed */
    g_object_ref (channel); /* We hold separate refs for state machine */
    g_signal_connect (channel, "abort", G_CALLBACK (on_channel_abort_context),
		      context);
    
    if (chain)
    {
        g_debug ("entering state machine for channel of type: %s",
             g_quark_to_string (chan_type_quark));

	priv->state_machine_list =
	    g_slist_prepend (priv->state_machine_list, context);
	mcd_dispatcher_context_process (context, TRUE);
    }
    else
    {
	g_debug ("No filters found for type %s, starting the channel handler", g_quark_to_string (chan_type_quark));
	_mcd_dispatcher_start_channel_handler (context);
    }
}

static gint
channel_on_state_machine (McdDispatcherContext *context, McdChannel *channel)
{
    return (context->channel == channel) ? 0 : 1;
}

static void
_mcd_dispatcher_send (McdDispatcher * dispatcher, McdChannel * channel)
{
    McdDispatcherPrivate *priv;
    g_return_if_fail (MCD_IS_DISPATCHER (dispatcher));
    g_return_if_fail (MCD_IS_CHANNEL (channel));
    
    priv = MCD_DISPATCHER_PRIV (dispatcher);

    /* it can happen that this function gets called when the same channel has
     * already entered the state machine or even when it has already been
     * dispatched; so, check if this channel is already known to the
     * dispatcher: */
    if (g_list_find (priv->channels, channel))
    {
	McdDispatcherContext *context = NULL;
	GSList *list;
	g_debug ("%s: channel is already in dispatcher", G_STRFUNC);

	/* check if channel has already been dispatched (if it's still in the
	 * state machine list, this means that it hasn't) */
	list = g_slist_find_custom (priv->state_machine_list, channel,
				    (GCompareFunc)channel_on_state_machine);
	if (list) context = list->data;
	if (context)
	{
	    gboolean outgoing;
	    g_debug ("%s: channel found in the state machine (%p)", G_STRFUNC, context);
	    g_object_get (G_OBJECT (channel),
			  "outgoing", &outgoing,
			  NULL);

	    g_debug ("channel is %s", outgoing ? "outgoing" : "incoming");
	    /* this channel has not been dispatched; we can get to this point if:
	     * 1) the channel is incoming (i.e. the contacts plugin icon is
	     *    blinking) but the user didn't realize that and instead
	     *    requested the same channel again;
	     * 2) theif channel is outgoing, and it was requested again before
	     *    it could be created; I'm not sure this can really happen,
	     *    though. In this case we don't have to do anything, just ignore
	     *    this second request */
	    if (!outgoing)
	    {
		/* incoming channel: the state machine is probably stucked
		 * waiting for the user to acknowledge the channel. We bypass
		 * all that and instead launch the channel handler; this will
		 * free the context, but we still have to remove it from the
		 * machine state list ourselves.
		 * The filters should connect to the "dispatched" signal to
		 * catch this particular situation and clean-up gracefully. */
		_mcd_dispatcher_start_channel_handler (context);
		priv->state_machine_list =
		    g_slist_remove(priv->state_machine_list, context);

	    }
	}
	else
	{
	    /* The channel was not found in the state machine, hence it must
	     * have been already dispatched.
	     * We could get to this point if the UI crashed while this channel
	     * was open, and now the user is requesting it again. just go straight
	     * and start the channel handler. */
	    g_debug ("%s: channel is already dispatched, starting handler", G_STRFUNC);
	    /* Preparing and filling the context */
	    context = g_new0 (McdDispatcherContext, 1);
	    context->dispatcher = dispatcher;
	    context->channel = channel;

	    /* We must ref() the channel, because mcd_dispatcher_context_free()
	     * will unref() it */
	    g_object_ref (channel);
	    _mcd_dispatcher_start_channel_handler (context);
	}
	return;
    }
    
    /* Get hold of it in our all channels list */
    g_object_ref (channel); /* We hold separate refs for channels list */
    g_signal_connect (channel, "abort", G_CALLBACK (on_channel_abort_list),
		      dispatcher);
    priv->channels = g_list_prepend (priv->channels, channel);
    
    g_signal_emit_by_name (dispatcher, "channel-added", channel);
    _mcd_dispatcher_enter_state_machine (dispatcher, channel);
}

static void
_mcd_dispatcher_set_property (GObject * obj, guint prop_id,
			      const GValue * val, GParamSpec * pspec)
{
    McdDispatcher *dispatcher = MCD_DISPATCHER (obj);
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (obj);
    DBusGConnection *dbus_connection;
    McdMaster *master;

    switch (prop_id)
    {
    case PROP_PLUGIN_DIR:
	g_free (priv->plugin_dir);

	priv->plugin_dir = g_value_dup_string (val);

	_mcd_dispatcher_unload_filters (dispatcher);
	_mcd_dispatcher_load_filters (dispatcher);
	break;
    case PROP_DBUS_CONNECTION:
	dbus_connection = g_value_get_pointer (val);
	dbus_g_connection_ref (dbus_connection);
	if (priv->dbus_connection)
	    dbus_g_connection_unref (priv->dbus_connection);
	priv->dbus_connection = dbus_connection;
	break;
    case PROP_MCD_MASTER:
	master = g_value_get_object (val);
	g_object_ref (G_OBJECT (master));
	if (priv->master)
        {
            g_signal_handlers_disconnect_by_func (G_OBJECT (master), G_CALLBACK (on_master_abort), NULL);
	    g_object_unref (priv->master);
        }
	priv->master = master;
        g_signal_connect (G_OBJECT (master), "abort", G_CALLBACK (on_master_abort), priv);
	break;
    default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
	break;
    }
}

static void
_mcd_dispatcher_get_property (GObject * obj, guint prop_id,
			      GValue * val, GParamSpec * pspec)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (obj);

    switch (prop_id)
    {
    case PROP_PLUGIN_DIR:
	g_value_set_string (val, priv->plugin_dir);
	break;
    case PROP_DBUS_CONNECTION:
	g_value_set_pointer (val, priv->dbus_connection);
	break;
    case PROP_MCD_MASTER:
	g_value_set_object (val, priv->master);
	break;
    default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
	break;
    }
}

static void
_mcd_dispatcher_finalize (GObject * object)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (object);
    GType type;
    gint i;

    g_hash_table_destroy (priv->channel_handler_hash);

    type = dbus_g_type_get_struct ("GValueArray", G_TYPE_STRING,
				   G_TYPE_UINT, G_TYPE_INVALID);
    for (i = 0; i < priv->channel_handler_caps->len; i++)
	g_boxed_free (type, g_ptr_array_index (priv->channel_handler_caps, i));
    g_ptr_array_free (priv->channel_handler_caps, TRUE);

    G_OBJECT_CLASS (mcd_dispatcher_parent_class)->finalize (object);
}

static void
_mcd_dispatcher_dispose (GObject * object)
{
    McdDispatcher *dispatcher = MCD_DISPATCHER (object);
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (object);
    
    if (priv->is_disposed)
    {
	return;
    }
    priv->is_disposed = TRUE;
    
    if (priv->master)
    {
	g_object_unref (priv->master);
	priv->master = NULL;
    }

    if (priv->dbus_connection)
    {
	dbus_g_connection_unref (priv->dbus_connection);
	priv->dbus_connection = NULL;
    }

    if (priv->channels)
    {
	g_list_free (priv->channels);
	priv->channels = NULL;
    }
    g_free (priv->plugin_dir);
    priv->plugin_dir = NULL;

    _mcd_dispatcher_unload_filters (dispatcher);
    G_OBJECT_CLASS (mcd_dispatcher_parent_class)->dispose (object);
}

gboolean
mcd_dispatcher_send (McdDispatcher * dispatcher, McdChannel *channel)
{
    g_return_val_if_fail (MCD_IS_DISPATCHER (dispatcher), FALSE);
    g_return_val_if_fail (MCD_IS_CHANNEL (channel), FALSE);
    MCD_DISPATCHER_GET_CLASS (dispatcher)->send (dispatcher, channel);
    return TRUE;
}

static void
mcd_dispatcher_class_init (McdDispatcherClass * klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (McdDispatcherPrivate));

    object_class->set_property = _mcd_dispatcher_set_property;
    object_class->get_property = _mcd_dispatcher_get_property;
    object_class->finalize = _mcd_dispatcher_finalize;
    object_class->dispose = _mcd_dispatcher_dispose;
    klass->send = _mcd_dispatcher_send;

    mcd_dispatcher_signals[CHANNEL_ADDED] =
	g_signal_new ("channel_added",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      G_STRUCT_OFFSET (McdDispatcherClass,
				       channel_added_signal),
		      NULL, NULL, mcd_marshal_VOID__OBJECT,
		      G_TYPE_NONE, 1, MCD_TYPE_CHANNEL);
    
    mcd_dispatcher_signals[CHANNEL_REMOVED] =
	g_signal_new ("channel_removed",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      G_STRUCT_OFFSET (McdDispatcherClass,
				       channel_removed_signal),
		      NULL, NULL, mcd_marshal_VOID__OBJECT,
		      G_TYPE_NONE, 1, MCD_TYPE_CHANNEL);
    
    mcd_dispatcher_signals[DISPATCHED] =
	g_signal_new ("dispatched",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      G_STRUCT_OFFSET (McdDispatcherClass,
				       dispatched_signal),
		      NULL, NULL, mcd_marshal_VOID__OBJECT,
		      G_TYPE_NONE, 1, MCD_TYPE_CHANNEL);
    
    mcd_dispatcher_signals[DISPATCH_FAILED] =
	g_signal_new ("dispatch-failed",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      G_STRUCT_OFFSET (McdDispatcherClass,
				       dispatch_failed_signal),
		      NULL, NULL, mcd_marshal_VOID__OBJECT_POINTER,
		      G_TYPE_NONE, 2, MCD_TYPE_CHANNEL, G_TYPE_POINTER);
    
    /* Properties */
    g_object_class_install_property (object_class,
				     PROP_PLUGIN_DIR,
				     g_param_spec_string ("plugin-dir",
							  _("Plugin Directory"),
							  _("The Directory to load filter plugins from"),
							  MCD_DEFAULT_FILTER_PLUGIN_DIR,
							  G_PARAM_READWRITE |
							  G_PARAM_CONSTRUCT));
    g_object_class_install_property (object_class,
				     PROP_DBUS_CONNECTION,
				     g_param_spec_pointer ("dbus-connection",
							   _("DBus Connection"),
							   _("DBus connection to use by us"),
							   G_PARAM_READWRITE |
							   G_PARAM_CONSTRUCT));
    g_object_class_install_property (object_class,
				     PROP_MCD_MASTER,
				     g_param_spec_object ("mcd-master",
							   _("McdMaster"),
							   _("McdMaster"),
							   MCD_TYPE_MASTER,
							   G_PARAM_READWRITE |
							   G_PARAM_CONSTRUCT));
}

static void
_build_channel_capabilities (gchar *channel_type, McdChannelHandler *handler,
			     GPtrArray *capabilities)
{
    GValue cap = {0,};
    GType cap_type;

    cap_type = dbus_g_type_get_struct ("GValueArray", G_TYPE_STRING,
				       G_TYPE_UINT, G_TYPE_INVALID);
    g_value_init (&cap, cap_type);
    g_value_take_boxed (&cap, dbus_g_type_specialized_construct (cap_type));

    dbus_g_type_struct_set (&cap,
			    0, channel_type,
			    1, handler->capabilities,
			    G_MAXUINT);

    g_ptr_array_add (capabilities, g_value_get_boxed (&cap));
}



static void
mcd_dispatcher_init (McdDispatcher * dispatcher)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (dispatcher);

    priv->plugin_dir = g_strdup (MCD_DEFAULT_FILTER_PLUGIN_DIR);

    g_datalist_init (&(priv->interface_filters));
    
    priv->channel_handler_hash = mcd_get_channel_handlers ();
 
    priv->channel_handler_caps = g_ptr_array_new();
    g_hash_table_foreach (priv->channel_handler_hash,
			  (GHFunc)_build_channel_capabilities,
			  priv->channel_handler_caps);

    _mcd_dispatcher_load_filters (dispatcher);
}

McdDispatcher *
mcd_dispatcher_new (DBusGConnection * dbus_connection, McdMaster * master)
{
    McdDispatcher *obj;
    obj = MCD_DISPATCHER (g_object_new (MCD_TYPE_DISPATCHER,
					"dbus-connection",
					dbus_connection,
					"mcd-master", 
					master, 
					NULL));
    return obj;
}

/* The new state machine walker function for pluginized filters*/

void
mcd_dispatcher_context_process (McdDispatcherContext * context, gboolean result)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (context->dispatcher);
    
    if (result)
    {
	McdFilter *filter;

	filter = g_list_nth_data (context->chain, context->next_func_index);
	/* Do we still have functions to go through? */
	if (filter)
	{
	    context->next_func_index++;
	    
	    g_debug ("Next filter");
	    filter->func (context);
	    return; /*State machine goes on...*/
	}
	else
	{
	    /* Context would be destroyed somewhere in this call */
	    _mcd_dispatcher_start_channel_handler (context);
	}
    }
    else
    {
	g_debug ("Filters failed, disposing request");
	
	/* Some filter failed. The request shall not be handled. */
	/* Context would be destroyed somewhere in this call */
	_mcd_dispatcher_drop_channel_handler (context);
    }
    
    /* FIXME: Should we remove the request in other cases? */
    priv->state_machine_list =
	g_slist_remove(priv->state_machine_list, context);
}

static void
mcd_dispatcher_context_free (McdDispatcherContext * context)
{
    /* FIXME: check for leaks */
    g_return_if_fail (context);

    /* Freeing context data */
    if (context->channel)
    {
	g_signal_handlers_disconnect_by_func (context->channel,
					      G_CALLBACK (on_channel_abort_context),
					      context);
	g_object_unref (context->channel);
    }
    g_free (context);
}

/* CONTEXT API */

/* Context getters */
const TpChan *
mcd_dispatcher_context_get_channel_object (McdDispatcherContext * ctx)
{
    TpChan *tp_chan;
    g_return_val_if_fail (ctx, 0);
    g_object_get (G_OBJECT (ctx->channel), "tp-channel", &tp_chan, NULL);
    g_object_unref (G_OBJECT (tp_chan));
    return tp_chan;
}

McdDispatcher*
mcd_dispatcher_context_get_dispatcher (McdDispatcherContext * ctx)
{
    return ctx->dispatcher;
}

const McdConnection *
mcd_dispatcher_context_get_connection (McdDispatcherContext * context)
{
    McdConnection *connection;

    g_return_val_if_fail (context, NULL);

    g_object_get (G_OBJECT (context->channel),
		  "connection", &connection,
		  NULL);
    g_object_unref (G_OBJECT (connection));

    return connection;
}

const TpConn *
mcd_dispatcher_context_get_connection_object (McdDispatcherContext * ctx)
{
    const McdConnection *connection;
    TpConn *tp_conn;
   
    connection = mcd_dispatcher_context_get_connection (ctx); 
    g_object_get (G_OBJECT (connection), "tp-connection",
		  &tp_conn, NULL);
   
    g_object_unref (tp_conn); 
    return tp_conn;
}

McdChannel *
mcd_dispatcher_context_get_channel (McdDispatcherContext * ctx)
{
    g_return_val_if_fail (ctx, 0);
    return ctx->channel;
}

McdChannelHandler *
mcd_dispatcher_context_get_chan_handler (McdDispatcherContext * ctx)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (ctx->dispatcher);
    McdChannel *channel;

    channel = mcd_dispatcher_context_get_channel (ctx);
    return g_hash_table_lookup (priv->channel_handler_hash,
	                        mcd_channel_get_channel_type (channel));
}

/*Returns an array of the participants in the channel*/
GPtrArray *
mcd_dispatcher_context_get_members (McdDispatcherContext * ctx)
{
    return mcd_channel_get_members (ctx->channel);
}

GPtrArray *mcd_dispatcher_get_channel_capabilities (McdDispatcher * dispatcher)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (dispatcher);

    return priv->channel_handler_caps;
}
