#include <config.h>
#include <string.h>

#include "gfilemonitorpriv.h"
#include "gvfs-marshal.h"
#include "gvfs.h"

enum {
  CHANGED,
  LAST_SIGNAL
};

G_DEFINE_TYPE (GFileMonitor, g_file_monitor, G_TYPE_OBJECT);

struct _GFileMonitorPrivate {
  gboolean cancelled;
  int rate_limit_msec;

  /* Rate limiting change events */
  guint32 last_sent_change_time; /* Some monitonic clock in msecs */
  GFile *last_sent_change_file;
  
  guint send_delayed_change_timeout;

  /* Virtual CHANGES_DONE_HINT emission */
  GSource *virtual_changes_done_timeout;
  GFile *virtual_changes_done_file;
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
g_file_monitor_finalize (GObject *object)
{
  GFileMonitor *monitor;

  monitor = G_FILE_MONITOR (object);

  if (monitor->priv->last_sent_change_file)
    g_object_unref (monitor->priv->last_sent_change_file);

  if (monitor->priv->send_delayed_change_timeout != 0)
    g_source_remove (monitor->priv->send_delayed_change_timeout);

  if (monitor->priv->virtual_changes_done_file)
    g_object_unref (monitor->priv->virtual_changes_done_file);

  if (monitor->priv->virtual_changes_done_timeout)
    g_source_destroy (monitor->priv->virtual_changes_done_timeout);
  
  if (G_OBJECT_CLASS (g_file_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_monitor_parent_class)->finalize) (object);
}

static void
g_file_monitor_dispose (GObject *object)
{
  GFileMonitor *monitor;
  
  monitor = G_FILE_MONITOR (object);

  /* Make sure we cancel on last unref */
  if (!monitor->priv->cancelled)
    g_file_monitor_cancel (monitor);
  
  if (G_OBJECT_CLASS (g_file_monitor_parent_class)->dispose)
    (*G_OBJECT_CLASS (g_file_monitor_parent_class)->dispose) (object);
}

static void
g_file_monitor_class_init (GFileMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GFileMonitorPrivate));
  
  gobject_class->finalize = g_file_monitor_finalize;
  gobject_class->dispose = g_file_monitor_dispose;

  signals[CHANGED] =
    g_signal_new (I_("changed"),
		  G_TYPE_FILE_MONITOR,
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GFileMonitorClass, changed),
		  NULL, NULL,
		  _gvfs_marshal_VOID__OBJECT_OBJECT_INT,
		  G_TYPE_NONE,3,
		  G_TYPE_FILE,
		  G_TYPE_FILE,
		  G_TYPE_INT);
}

static void
g_file_monitor_init (GFileMonitor *monitor)
{
  monitor->priv = G_TYPE_INSTANCE_GET_PRIVATE (monitor,
					       G_TYPE_FILE_MONITOR,
					       GFileMonitorPrivate);
  monitor->priv->rate_limit_msec = 800;
}


gboolean
g_file_monitor_is_cancelled (GFileMonitor *monitor)
{
  return monitor->priv->cancelled;
}

gboolean
g_file_monitor_cancel (GFileMonitor* monitor)
{
  GFileMonitorClass *class;
  
  if (monitor->priv->cancelled)
    return TRUE;
  
  monitor->priv->cancelled = TRUE;
  
  class = G_FILE_MONITOR_GET_CLASS (monitor);
  return (* class->cancel) (monitor);
}

void
g_file_monitor_set_rate_limit (GFileMonitor *monitor,
			       int           limit_msecs)
{
  monitor->priv->rate_limit_msec = limit_msecs;
}

static guint32
get_time_msecs (void)
{
  return g_thread_gettime() / (1000 * 1000);
}

static guint32
time_difference (guint32 from, guint32 to)
{
  if (from > to)
    return 0;
  return to - from;
}

/* Change event rate limiting support: */

static void
update_last_sent_change (GFileMonitor *monitor, GFile *file, guint32 time_now)
{
  if (monitor->priv->last_sent_change_file != file)
    {
      if (monitor->priv->last_sent_change_file)
	{
	  g_object_unref (monitor->priv->last_sent_change_file);
	  monitor->priv->last_sent_change_file = NULL;
	}
      if (file)
	monitor->priv->last_sent_change_file = g_object_ref (file);
    }
  
  monitor->priv->last_sent_change_time = time_now;
}

static void
send_delayed_change_now (GFileMonitor *monitor)
{
  if (monitor->priv->send_delayed_change_timeout)
    {
      g_print ("emitting delayed changed event\n");
      g_signal_emit (monitor, signals[CHANGED], 0,
		     monitor->priv->last_sent_change_file, NULL,
		     G_FILE_MONITOR_EVENT_CHANGED);
      
      g_source_remove (monitor->priv->send_delayed_change_timeout);
      monitor->priv->send_delayed_change_timeout = 0;

      /* Same file, new last_sent time */
      monitor->priv->last_sent_change_time = get_time_msecs ();
    }
}

static gboolean
delayed_changed_event_timeout (gpointer data)
{
  GFileMonitor *monitor = data;

  send_delayed_change_now (monitor);
  
  return FALSE;
}

static void
schedule_delayed_change (GFileMonitor *monitor, GFile *file, guint32 delay_msec)
{
  if (monitor->priv->send_delayed_change_timeout == 0) /* Only set the timeout once */
    {
      monitor->priv->send_delayed_change_timeout = 
	g_timeout_add (delay_msec, delayed_changed_event_timeout, monitor);
    }
}

static void
cancel_delayed_change (GFileMonitor *monitor)
{
  if (monitor->priv->send_delayed_change_timeout != 0)
    {
      g_source_remove (monitor->priv->send_delayed_change_timeout);
      monitor->priv->send_delayed_change_timeout = 0;
    }
}

/* Virtual changes_done_hint support: */

static void
send_virtual_changes_done_now (GFileMonitor *monitor)
{
  if (monitor->priv->virtual_changes_done_timeout)
    {
      g_signal_emit (monitor, signals[CHANGED], 0,
		     monitor->priv->virtual_changes_done_file, NULL,
		     G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT);
      
      g_source_destroy (monitor->priv->virtual_changes_done_timeout);
      monitor->priv->virtual_changes_done_timeout = NULL;

      g_object_unref (monitor->priv->virtual_changes_done_file);
      monitor->priv->virtual_changes_done_file = NULL;
    }
}

static gboolean
virtual_changes_done_timeout (gpointer data)
{
  GFileMonitor *monitor = data;

  send_virtual_changes_done_now (monitor);
  
  return FALSE;
}

static void
schedule_virtual_change_done (GFileMonitor *monitor, GFile *file)
{
  GSource *source;
  
  source = g_timeout_source_new_seconds (3);
  
  g_source_set_callback (source, virtual_changes_done_timeout, monitor, NULL);
  g_source_attach (source, NULL);
  monitor->priv->virtual_changes_done_timeout = source;
  monitor->priv->virtual_changes_done_file = g_object_ref (file);
  g_source_unref (source);
}

static void
cancel_virtual_changes_done (GFileMonitor *monitor)
{
  if (monitor->priv->virtual_changes_done_timeout)
    {
      g_source_destroy (monitor->priv->virtual_changes_done_timeout);
      monitor->priv->virtual_changes_done_timeout = NULL;
      
      g_object_unref (monitor->priv->virtual_changes_done_file);
      monitor->priv->virtual_changes_done_file = NULL;
    }
}

void
g_file_monitor_emit_event (GFileMonitor *monitor,
			   GFile *file,
			   GFile *other_file,
			   GFileMonitorEvent event_type)
{
  guint32 time_now, since_last;
  gboolean emit_now;

  if (event_type != G_FILE_MONITOR_EVENT_CHANGED)
    {
      send_delayed_change_now (monitor);
      update_last_sent_change (monitor, NULL, 0);
      if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
	cancel_virtual_changes_done (monitor);
      else
	send_virtual_changes_done_now (monitor);
      g_signal_emit (monitor, signals[CHANGED], 0, file, other_file, event_type);
    }
  else
    {
      time_now = get_time_msecs ();
      emit_now = TRUE;
      
      if (monitor->priv->last_sent_change_file)
	{
	  since_last = time_difference (monitor->priv->last_sent_change_time, time_now);
	  if (since_last < monitor->priv->rate_limit_msec)
	    {
	      /* We ignore this change, but arm a timer so that we can fire it later if we
		 don't get any other events (that kill this timeout) */
	      emit_now = FALSE;
	      schedule_delayed_change (monitor, file,
				       monitor->priv->rate_limit_msec - since_last);
	    }
	}
      
      if (emit_now)
	{
	  g_print ("emitting real changed event\n");
	  g_signal_emit (monitor, signals[CHANGED], 0, file, other_file, event_type);
	  
	  cancel_delayed_change (monitor);
	  update_last_sent_change (monitor, file, time_now);
	}

      /* Schedule a virtual change done. This is removed if we get a real one, and
	 postponed if we get more change events. */
      cancel_virtual_changes_done (monitor);
      schedule_virtual_change_done (monitor, file);
    }
}
