/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2008 Red Hat, Inc.
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include <gdbusutils.h>

#include "gproxydrive.h"
#include "gproxyvolume.h"
#include "gproxymount.h"

static void signal_emit_in_idle (gpointer object, const char *signal_name, gpointer other_object);

/* Protects all fields of GProxyVolume that can change */
G_LOCK_DEFINE_STATIC(proxy_volume);

struct _GProxyVolume {
  GObject parent;

  GProxyVolumeMonitor *volume_monitor;

  /* non-NULL only if activation_uri != NULL */
  GVolumeMonitor *union_monitor;

  char *id;
  char *name;
  char *uuid;
  char *activation_uri;
  GIcon *icon;
  char *drive_id;
  char *mount_id;
  GHashTable *identifiers;

  gboolean can_mount;
  gboolean should_automount;

  GProxyShadowMount *shadow_mount;
};

static void g_proxy_volume_volume_iface_init (GVolumeIface *iface);

#define _G_IMPLEMENT_INTERFACE_DYNAMIC(TYPE_IFACE, iface_init)       { \
  const GInterfaceInfo g_implement_interface_info = { \
    (GInterfaceInitFunc) iface_init, NULL, NULL \
  }; \
  g_type_module_add_interface (type_module, g_define_type_id, TYPE_IFACE, &g_implement_interface_info); \
}

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GProxyVolume, g_proxy_volume, G_TYPE_OBJECT, 0,
                                _G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_VOLUME,
                                                                g_proxy_volume_volume_iface_init))


static void union_monitor_mount_added (GVolumeMonitor *union_monitor,
                                       GMount         *mount,
                                       GProxyVolume   *volume);

static void union_monitor_mount_removed (GVolumeMonitor *union_monitor,
                                         GMount         *mount,
                                         GProxyVolume   *volume);

static void union_monitor_mount_changed (GVolumeMonitor *union_monitor,
                                         GMount         *mount,
                                         GProxyVolume   *volume);

static void update_shadow_mount (GProxyVolume *volume);

GProxyShadowMount *
g_proxy_volume_get_shadow_mount (GProxyVolume *volume)
{
  if (volume->shadow_mount != NULL)
    return g_object_ref (volume->shadow_mount);
  else
    return NULL;
}

static void
g_proxy_volume_finalize (GObject *object)
{
  GProxyVolume *volume;

  volume = G_PROXY_VOLUME (object);

  g_free (volume->id);
  g_free (volume->name);
  g_free (volume->uuid);
  g_free (volume->activation_uri);
  if (volume->icon != NULL)
    g_object_unref (volume->icon);
  g_free (volume->drive_id);
  g_free (volume->mount_id);
  if (volume->identifiers != NULL)
    g_hash_table_unref (volume->identifiers);

  if (volume->shadow_mount != NULL)
    {
      signal_emit_in_idle (volume->shadow_mount, "unmounted", NULL);
      signal_emit_in_idle (volume->volume_monitor, "mount-removed", volume->shadow_mount);
      g_proxy_shadow_mount_remove (volume->shadow_mount);
      g_object_unref (volume->shadow_mount);
    }

  if (volume->union_monitor != NULL)
    {
      g_signal_handlers_disconnect_by_func (volume->union_monitor, union_monitor_mount_added, volume);
      g_signal_handlers_disconnect_by_func (volume->union_monitor, union_monitor_mount_removed, volume);
      g_signal_handlers_disconnect_by_func (volume->union_monitor, union_monitor_mount_changed, volume);
      g_object_unref (volume->union_monitor);
    }

  if (volume->volume_monitor != NULL)
    {
      g_object_unref (volume->volume_monitor);
    }

  if (G_OBJECT_CLASS (g_proxy_volume_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_proxy_volume_parent_class)->finalize) (object);
}

static void
g_proxy_volume_class_init (GProxyVolumeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_proxy_volume_finalize;
}

static void
g_proxy_volume_class_finalize (GProxyVolumeClass *klass)
{
}

static void
g_proxy_volume_init (GProxyVolume *proxy_volume)
{
}

GProxyVolume *
g_proxy_volume_new (GProxyVolumeMonitor *volume_monitor)
{
  GProxyVolume *volume;
  volume = g_object_new (G_TYPE_PROXY_VOLUME, NULL);
  volume->volume_monitor = g_object_ref (volume_monitor);
  g_object_set_data (G_OBJECT (volume),
                     "g-proxy-volume-volume-monitor-name",
                     (gpointer) g_type_name (G_TYPE_FROM_INSTANCE (volume_monitor)));
  return volume;
}


static void
union_monitor_mount_added (GVolumeMonitor *union_monitor,
                           GMount         *mount,
                           GProxyVolume   *volume)
{
  update_shadow_mount (volume);
}

static void
union_monitor_mount_removed (GVolumeMonitor *union_monitor,
                             GMount         *mount,
                             GProxyVolume   *volume)
{
  update_shadow_mount (volume);
}

static void
union_monitor_mount_changed (GVolumeMonitor *union_monitor,
                             GMount         *mount,
                             GProxyVolume   *volume)
{
  if (volume->shadow_mount != NULL)
    {
      GMount *real_mount;
      real_mount = g_proxy_shadow_mount_get_real_mount (volume->shadow_mount);
      if (mount == real_mount)
        {
          signal_emit_in_idle (volume->shadow_mount, "changed", NULL);
          signal_emit_in_idle (volume->volume_monitor, "mount-changed", volume->shadow_mount);
        }
      g_object_unref (real_mount);
    }
}

static void
update_shadow_mount (GProxyVolume *volume)
{
  GFile *activation_root;
  GList *mounts;
  GList *l;
  GMount *mount_to_shadow;

  activation_root = NULL;
  mount_to_shadow = NULL;

  if (volume->activation_uri == NULL)
    goto out;

  activation_root = g_file_new_for_uri (volume->activation_uri);

  if (volume->union_monitor == NULL)
    {
      volume->union_monitor = g_volume_monitor_get ();
      g_signal_connect (volume->union_monitor, "mount-added", (GCallback) union_monitor_mount_added, volume);
      g_signal_connect (volume->union_monitor, "mount-removed", (GCallback) union_monitor_mount_removed, volume);
      g_signal_connect (volume->union_monitor, "mount-changed", (GCallback) union_monitor_mount_changed, volume);
    }

  mounts = g_volume_monitor_get_mounts (volume->union_monitor);
  for (l = mounts; l != NULL; l = l->next)
    {
      GMount *mount = G_MOUNT (l->data);
      GFile *mount_root;

      /* don't consider our (possibly) existing shadow mount */
      if (G_IS_PROXY_SHADOW_MOUNT (mount))
        continue;

      mount_root = g_mount_get_root (mount);
      if (g_file_has_prefix (activation_root, mount_root))
        {
          mount_to_shadow = g_object_ref (mount);
          break;
        }
    }
  g_list_foreach (mounts, (GFunc) g_object_unref, NULL);
  g_list_free (mounts);

  if (mount_to_shadow != NULL)
    {
      /* there's now a mount to shadow, if we don't have a GProxyShadowMount then create one */
      if (volume->shadow_mount == NULL)
        {
          volume->shadow_mount = g_proxy_shadow_mount_new (volume->volume_monitor,
                                                           volume,
                                                           mount_to_shadow);
          signal_emit_in_idle (volume->volume_monitor, "mount-added", volume->shadow_mount);
        }
      else
        {
          GFile *current_activation_root;

          /* we have a GProxyShadowMount already. However, we need to replace it if the
           * activation root has changed.
           */
          current_activation_root = g_proxy_shadow_mount_get_activation_root (volume->shadow_mount);
          if (!g_file_equal (current_activation_root, activation_root))
            {
              signal_emit_in_idle (volume->shadow_mount, "unmounted", NULL);
              signal_emit_in_idle (volume->volume_monitor, "mount-removed", volume->shadow_mount);
              g_proxy_shadow_mount_remove (volume->shadow_mount);
              g_object_unref (volume->shadow_mount);
              volume->shadow_mount = NULL;

              volume->shadow_mount = g_proxy_shadow_mount_new (volume->volume_monitor,
                                                               volume,
                                                               mount_to_shadow);
              signal_emit_in_idle (volume->volume_monitor, "mount-added", volume->shadow_mount);
            }
          g_object_unref (current_activation_root);
        }
    }
  else
    {
      /* no mount to shadow; if we have a GProxyShadowMount then remove it */
      if (volume->shadow_mount != NULL)
        {
          signal_emit_in_idle (volume->shadow_mount, "unmounted", NULL);
          signal_emit_in_idle (volume->volume_monitor, "mount-removed", volume->shadow_mount);
          g_proxy_shadow_mount_remove (volume->shadow_mount);
          g_object_unref (volume->shadow_mount);
          volume->shadow_mount = NULL;
        }
    }

 out:

  if (activation_root != NULL)
    g_object_unref (activation_root);

  if (mount_to_shadow != NULL)
    g_object_unref (mount_to_shadow);
}

static gboolean
update_shadow_mount_in_idle_do (GProxyVolume *volume)
{
  update_shadow_mount (volume);
  g_object_unref (volume);
  return FALSE;
}

static void
update_shadow_mount_in_idle (GProxyVolume *volume)
{
  g_idle_add ((GSourceFunc) update_shadow_mount_in_idle_do, g_object_ref (volume));
}

/* string               id
 * string               name
 * string               gicon_data
 * string               uuid
 * string               activation_uri
 * boolean              can-mount
 * boolean              should-automount
 * string               drive-id
 * string               mount-id
 * dict:string->string  identifiers
 */

void g_proxy_volume_update (GProxyVolume    *volume,
                            DBusMessageIter *iter)
{
  DBusMessageIter iter_struct;
  const char *id;
  const char *name;
  const char *gicon_data;
  const char *uuid;
  const char *activation_uri;
  const char *drive_id;
  const char *mount_id;
  dbus_bool_t can_mount;
  dbus_bool_t should_automount;
  GHashTable *identifiers;

  dbus_message_iter_recurse (iter, &iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &id);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &name);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &gicon_data);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &uuid);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &activation_uri);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &can_mount);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &should_automount);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &drive_id);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &mount_id);
  dbus_message_iter_next (&iter_struct);

  identifiers = _get_identifiers (&iter_struct);
  dbus_message_iter_next (&iter_struct);

  if (volume->id != NULL && strcmp (volume->id, id) != 0)
    {
      g_warning ("id mismatch during update of volume");
      goto out;
    }

  if (strlen (name) == 0)
    name = NULL;
  if (strlen (uuid) == 0)
    uuid = NULL;
  if (strlen (activation_uri) == 0)
    activation_uri = NULL;

  /* out with the old */
  g_free (volume->id);
  g_free (volume->name);
  g_free (volume->uuid);
  g_free (volume->activation_uri);
  if (volume->icon != NULL)
    g_object_unref (volume->icon);
  g_free (volume->drive_id);
  g_free (volume->mount_id);
  if (volume->identifiers != NULL)
    g_hash_table_unref (volume->identifiers);

  /* in with the new */
  volume->id = g_strdup (id);
  volume->name = g_strdup (name);
  volume->uuid = g_strdup (uuid);
  volume->activation_uri = g_strdup (activation_uri);
  if (*gicon_data == 0)
    volume->icon = NULL;
  else
    volume->icon = g_icon_new_for_string (gicon_data, NULL);
  volume->drive_id = g_strdup (drive_id);
  volume->mount_id = g_strdup (mount_id);
  volume->can_mount = can_mount;
  volume->should_automount = should_automount;
  volume->identifiers = identifiers != NULL ? g_hash_table_ref (identifiers) : NULL;

  /* this calls into the union monitor; do it in idle to avoid locking issues */
  update_shadow_mount_in_idle (volume);

 out:
  g_hash_table_unref (identifiers);
}

const char *
g_proxy_volume_get_id (GProxyVolume *volume)
{
  return volume->id;
}

static GIcon *
g_proxy_volume_get_icon (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GIcon *icon;

  G_LOCK (proxy_volume);
  icon = proxy_volume->icon != NULL ? g_object_ref (proxy_volume->icon) : NULL;
  G_UNLOCK (proxy_volume);
  return icon;
}

static char *
g_proxy_volume_get_name (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  char *name;

  G_LOCK (proxy_volume);
  name = g_strdup (proxy_volume->name);
  G_UNLOCK (proxy_volume);
  return name;
}

static char *
g_proxy_volume_get_uuid (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  char *uuid;

  G_LOCK (proxy_volume);
  uuid = g_strdup (proxy_volume->uuid);
  G_UNLOCK (proxy_volume);
  return uuid;
}

static gboolean
g_proxy_volume_can_mount (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  gboolean res;

  G_LOCK (proxy_volume);
  res = proxy_volume->can_mount;
  G_UNLOCK (proxy_volume);
  return res;
}

static gboolean
g_proxy_volume_can_eject (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GProxyDrive *drive;
  gboolean res;

  G_LOCK (proxy_volume);
  res = FALSE;
  if (proxy_volume->drive_id != NULL && strlen (proxy_volume->drive_id) > 0)
    {
      drive = g_proxy_volume_monitor_get_drive_for_id (proxy_volume->volume_monitor,
                                                       proxy_volume->drive_id);
      if (drive != NULL)
        {
          res = g_drive_can_eject (G_DRIVE (drive));
          g_object_unref (drive);
        }
    }
  G_UNLOCK (proxy_volume);

  return res;
}

static gboolean
g_proxy_volume_should_automount (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  gboolean res;

  G_LOCK (proxy_volume);
  res = proxy_volume->should_automount;
  G_UNLOCK (proxy_volume);

  return res;
}

static GDrive *
g_proxy_volume_get_drive (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GProxyDrive *drive;

  G_LOCK (proxy_volume);
  drive = NULL;
  if (proxy_volume->drive_id != NULL && strlen (proxy_volume->drive_id) > 0)
    drive = g_proxy_volume_monitor_get_drive_for_id (proxy_volume->volume_monitor,
                                                     proxy_volume->drive_id);
  G_UNLOCK (proxy_volume);

  return drive != NULL ? G_DRIVE (drive) : NULL;
}

static GMount *
g_proxy_volume_get_mount (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GMount *mount;

  mount = NULL;

  G_LOCK (proxy_volume);

  if (proxy_volume->shadow_mount != NULL)
    {
      mount = g_object_ref (proxy_volume->shadow_mount);
    }
  else if (proxy_volume->mount_id != NULL && strlen (proxy_volume->mount_id) > 0)
    {
      GProxyMount *proxy_mount;
      proxy_mount = g_proxy_volume_monitor_get_mount_for_id (proxy_volume->volume_monitor,
                                                             proxy_volume->mount_id);
      if (proxy_mount != NULL)
        mount = G_MOUNT (proxy_mount);
    }
  G_UNLOCK (proxy_volume);

  return mount;
}

typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
} EjectWrapperOp;

static void
eject_wrapper_callback (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  EjectWrapperOp *data  = user_data;
  data->callback (data->object, res, data->user_data);
  g_object_unref (data->object);
  g_free (data);
}

static void
g_proxy_volume_eject (GVolume              *volume,
                    GMountUnmountFlags   flags,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GProxyDrive *drive;

  drive = NULL;
  G_LOCK (proxy_volume);
  if (proxy_volume->drive_id != NULL && strlen (proxy_volume->drive_id) > 0)
    {
      drive = g_proxy_volume_monitor_get_drive_for_id (proxy_volume->volume_monitor,
                                                       proxy_volume->drive_id);
    }
  G_UNLOCK (proxy_volume);

  if (drive != NULL)
    {
      EjectWrapperOp *data;
      data = g_new0 (EjectWrapperOp, 1);
      data->object = g_object_ref (volume);
      data->callback = callback;
      data->user_data = user_data;
      g_drive_eject (G_DRIVE (drive), flags, cancellable, eject_wrapper_callback, data);
      g_object_unref (drive);
    }
}

static gboolean
g_proxy_volume_eject_finish (GVolume        *volume,
                          GAsyncResult  *result,
                          GError       **error)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GProxyDrive *drive;
  gboolean res;

  G_LOCK (proxy_volume);
  res = TRUE;
  drive = NULL;
  if (proxy_volume->drive_id != NULL && strlen (proxy_volume->drive_id) > 0)
    drive = g_proxy_volume_monitor_get_drive_for_id (proxy_volume->volume_monitor,
                                                     proxy_volume->drive_id);
  G_UNLOCK (proxy_volume);

  if (drive != NULL)
    {
      res = g_drive_eject_finish (G_DRIVE (drive), result, error);
      g_object_unref (drive);
    }

  return res;
}

static char *
g_proxy_volume_get_identifier (GVolume              *volume,
                               const char          *kind)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  char *res;

  G_LOCK (proxy_volume);
  if (proxy_volume->identifiers != NULL)
    res = g_strdup (g_hash_table_lookup (proxy_volume->identifiers, kind));
  else
    res = NULL;
  G_UNLOCK (proxy_volume);

  return res;
}

static void
add_identifier_key (const char *key, const char *value, GPtrArray *res)
{
  g_ptr_array_add (res, g_strdup (key));
}

static char **
g_proxy_volume_enumerate_identifiers (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GPtrArray *res;

  res = g_ptr_array_new ();

  G_LOCK (proxy_volume);
  if (proxy_volume->identifiers != NULL)
    g_hash_table_foreach (proxy_volume->identifiers, (GHFunc) add_identifier_key, res);
  G_UNLOCK (proxy_volume);

  /* Null-terminate */
  g_ptr_array_add (res, NULL);

  return (char **) g_ptr_array_free (res, FALSE);
}

typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
} DBusOp;

static void
mount_cb (DBusMessage *reply,
          GError *error,
          DBusOp *data)
{
  GSimpleAsyncResult *simple;
  if (error != NULL)
    simple = g_simple_async_result_new_from_error (data->object,
                                                   data->callback,
                                                   data->user_data,
                                                   error);
  else
    simple = g_simple_async_result_new (data->object,
                                        data->callback,
                                        data->user_data,
                                        NULL);
  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);

  g_object_unref (data->object);
  g_free (data);
}

typedef struct
{
  GProxyVolume *enclosing_volume;
  GAsyncReadyCallback  callback;
  gpointer user_data;
} ForeignMountOp;

static void
mount_foreign_callback (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  ForeignMountOp *data = user_data;
  data->callback (G_OBJECT (data->enclosing_volume), res, data->user_data);
  g_object_unref (data->enclosing_volume);
  g_free (data);
}

static void
g_proxy_volume_mount (GVolume             *volume,
                      GMountMountFlags     flags,
                      GMountOperation     *mount_operation,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);

  G_LOCK (proxy_volume);
  if (proxy_volume->activation_uri != NULL)
    {
      ForeignMountOp *data;
      GFile *root;

      data = g_new0 (ForeignMountOp, 1);
      data->enclosing_volume = g_object_ref (volume);
      data->callback = callback;
      data->user_data = user_data;

      root = g_file_new_for_uri (proxy_volume->activation_uri);

      G_UNLOCK (proxy_volume);

      g_file_mount_enclosing_volume (root,
                                     flags,
                                     mount_operation,
                                     cancellable,
                                     mount_foreign_callback,
                                     data);

      g_object_unref (root);
    }
  else
    {
      DBusOp *data;
      DBusConnection *connection;
      const char *name;
      DBusMessage *message;
      dbus_uint32_t _flags = flags;
      dbus_bool_t use_mount_operation = mount_operation != NULL;

      /* TODO: support mount_operation */

      data = g_new0 (DBusOp, 1);
      data->object = g_object_ref (volume);
      data->callback = callback;
      data->user_data = user_data;
      data->cancellable = cancellable;

      connection = g_proxy_volume_monitor_get_dbus_connection (proxy_volume->volume_monitor);
      name = g_proxy_volume_monitor_get_dbus_name (proxy_volume->volume_monitor);

      message = dbus_message_new_method_call (name,
                                              "/",
                                              "org.gtk.Private.RemoteVolumeMonitor",
                                              "VolumeMount");
      dbus_message_append_args (message,
                                DBUS_TYPE_STRING,
                                &(proxy_volume->id),
                                DBUS_TYPE_UINT32,
                                &_flags,
                                DBUS_TYPE_BOOLEAN,
                                &use_mount_operation,
                                DBUS_TYPE_INVALID);
      G_UNLOCK (proxy_volume);

      _g_dbus_connection_call_async (connection,
                                     message,
                                     -1,
                                     (GAsyncDBusCallback) mount_cb,
                                     data);
      dbus_message_unref (message);
      dbus_connection_unref (connection);
    }
}

static gboolean
g_proxy_volume_mount_finish (GVolume        *volume,
                             GAsyncResult  *result,
                             GError       **error)
{
  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error))
    return FALSE;
  return TRUE;
}

static GFile *
g_proxy_volume_get_activation_root (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  if (proxy_volume->activation_uri == NULL)
    return NULL;
  else
    return g_file_new_for_uri (proxy_volume->activation_uri);
}

static void
g_proxy_volume_volume_iface_init (GVolumeIface *iface)
{
  iface->get_name = g_proxy_volume_get_name;
  iface->get_icon = g_proxy_volume_get_icon;
  iface->get_uuid = g_proxy_volume_get_uuid;
  iface->get_drive = g_proxy_volume_get_drive;
  iface->get_mount = g_proxy_volume_get_mount;
  iface->can_mount = g_proxy_volume_can_mount;
  iface->can_eject = g_proxy_volume_can_eject;
  iface->should_automount = g_proxy_volume_should_automount;
  iface->mount_fn = g_proxy_volume_mount;
  iface->mount_finish = g_proxy_volume_mount_finish;
  iface->eject = g_proxy_volume_eject;
  iface->eject_finish = g_proxy_volume_eject_finish;
  iface->get_identifier = g_proxy_volume_get_identifier;
  iface->enumerate_identifiers = g_proxy_volume_enumerate_identifiers;
  iface->get_activation_root = g_proxy_volume_get_activation_root;
}

void
g_proxy_volume_register (GIOModule *module)
{
  g_proxy_volume_register_type (G_TYPE_MODULE (module));
}

typedef struct {
  const char *signal_name;
  GObject *object;
  GObject *other_object;
} SignalEmitIdleData;

static gboolean
signal_emit_in_idle_do (SignalEmitIdleData *data)
{
  if (data->other_object != NULL)
    {
      g_signal_emit_by_name (data->object, data->signal_name, data->other_object);
      g_object_unref (data->other_object);
    }
  else
    {
      g_signal_emit_by_name (data->object, data->signal_name);
    }
  g_object_unref (data->object);
  g_free (data);

  return FALSE;
}

static void
signal_emit_in_idle (gpointer object, const char *signal_name, gpointer other_object)
{
  SignalEmitIdleData *data;

  data = g_new0 (SignalEmitIdleData, 1);
  data->signal_name = signal_name;
  data->object = g_object_ref (G_OBJECT (object));
  data->other_object = other_object != NULL ? g_object_ref (G_OBJECT (other_object)) : NULL;
  g_idle_add ((GSourceFunc) signal_emit_in_idle_do, data);
}
