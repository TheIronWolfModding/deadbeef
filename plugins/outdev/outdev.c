/*
    DeaDBeeF -- the music player
    Output device selector widget plugin

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software
    in a product, an acknowledgment in the product documentation would be
    appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
    misrepresented as being the original software.

    3. This notice may not be removed or altered from any source distribution.
*/

// A small main-window widget (added via View -> Design Mode) that lets the
// user pick the current output plugin's device: PulseAudio sink, ALSA card,
// etc. It stores the selection in the same "<output_plugin>_soundcard" conf
// key that Preferences -> Sound uses, so the two stay in sync.

#include <deadbeef/deadbeef.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif
#include "../gtkui/gtkui_api.h"
#include "../../gettext.h"

DB_functions_t *deadbeef;
static ddb_gtkui_t *gtkui_plugin;

typedef struct {
    ddb_gtkui_widget_t base;
    GtkWidget *combobox;
    // combo index -> device id (g_strdup), index 0 is always "default"
    GSList *device_names;
    int updating;
    // output plugin the device list was built for; used to tell whether a
    // config change requires re-enumerating the devices
    char out_id[64];
    // device filter the device list was built with (see "pulse.device_filter"
    // in plugins/pulse/pulse.c -- the only output plugin with such a filter),
    // for the same reason
    char filter[256];
} w_outdev_t;

#if GTK_CHECK_VERSION(3, 0, 0)
#define ddb_hbox_new() gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4)
#define ddb_combo_new() gtk_combo_box_text_new ()
#else
#define ddb_hbox_new() gtk_hbox_new (FALSE, 4)
#define ddb_combo_new() gtk_combo_box_new_text ()
#endif

static void
ddb_combo_append (GtkWidget *combo, const char *text) {
#if GTK_CHECK_VERSION(3, 0, 0)
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), text);
#else
    gtk_combo_box_append_text (GTK_COMBO_BOX (combo), text);
#endif
}

static void
ddb_combo_clear (GtkWidget *combo) {
#if GTK_CHECK_VERSION(3, 0, 0)
    gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (combo));
#else
    GtkTreeModel *model = gtk_combo_box_get_model (GTK_COMBO_BOX (combo));
    int n = gtk_tree_model_iter_n_children (model, NULL);
    for (int i = n - 1; i >= 0; i--) {
        gtk_combo_box_remove_text (GTK_COMBO_BOX (combo), i);
    }
#endif
}

static const char *
_get_out_id (void) {
    DB_output_t *out = deadbeef->get_output ();
    return out ? out->plugin.id : "";
}

// the conf key used by the current output plugin to store its device
static void
_get_conf_name (char *buf, size_t size) {
    snprintf (buf, size, "%s_soundcard", _get_out_id ());
}

static void
_get_conf_value (char *buf, size_t size) {
    char conf[128];
    _get_conf_name (conf, sizeof (conf));
    deadbeef->conf_get_str (conf, "default", buf, size);
}

// The "Default Audio Device" entry is not a device reported by the output plugin: it
// means "use whatever the sound server picks". It is subject to the device filter as
// well -- the expression is matched against the conf token "default", not the
// localized label -- unless that is the device currently in use.
static int
_default_device_allowed (void) {
    char selected[256];
    _get_conf_value (selected, sizeof (selected));
    if (!strcmp (selected, "default")) {
        return 1;
    }

    char filter[256] = "";
    deadbeef->conf_get_str ("pulse.device_filter", "", filter, sizeof (filter));
    if (!*filter) {
        return 1;
    }

    GError *error = NULL;
    GRegex *regex = g_regex_new (filter, G_REGEX_CASELESS, 0, &error);
    if (error != NULL) {
        // invalid expression: hide nothing
        g_error_free (error);
        return 1;
    }
    int allowed = g_regex_match (regex, "default", 0, NULL) ? 1 : 0;
    g_regex_unref (regex);
    return allowed;
}

static void
_free_names (w_outdev_t *w) {
    for (GSList *l = w->device_names; l; l = l->next) {
        g_free (l->data);
    }
    g_slist_free (w->device_names);
    w->device_names = NULL;
}

static void
_enum_cb (const char *name, const char *desc, void *userdata) {
    w_outdev_t *w = userdata;
    ddb_combo_append (w->combobox, desc);
    w->device_names = g_slist_append (w->device_names, g_strdup (name));
}

// select the entry that matches the conf value, without touching the list
static void
_sync_selection (w_outdev_t *w) {
    if (!w->combobox) {
        return;
    }
    char selected[256];
    _get_conf_value (selected, sizeof (selected));

    w->updating = 1;
    int active = -1;
    int i = 0;
    for (GSList *l = w->device_names; l; l = l->next, i++) {
        if (!strcmp ((const char *)l->data, selected)) {
            active = i;
        }
    }
    // an empty combo means the selected device is not in the list, i.e. the
    // device filter hides it -- the device is still used for playback
    gtk_combo_box_set_active (GTK_COMBO_BOX (w->combobox), active);
    w->updating = 0;
}

// rebuild the device list by querying the current output plugin
static void
_refill (w_outdev_t *w) {
    if (!w->combobox) {
        return;
    }

    _free_names (w);
    ddb_combo_clear (w->combobox);
    if (_default_device_allowed ()) {
        ddb_combo_append (w->combobox, _ ("Default Audio Device"));
        w->device_names = g_slist_append (w->device_names, g_strdup ("default"));
    }

    DB_output_t *out = deadbeef->get_output ();
    if (out && out->enum_soundcards) {
        // NOTE: this is a blocking round-trip to the sound server, so it is
        // only done when the device list may actually have changed
        out->enum_soundcards (_enum_cb, w);
    }

    snprintf (w->out_id, sizeof (w->out_id), "%s", _get_out_id ());
    deadbeef->conf_get_str ("pulse.device_filter", "", w->filter, sizeof (w->filter));
    _sync_selection (w);
}

static void
_on_changed (GtkComboBox *combo, gpointer user_data) {
    w_outdev_t *w = user_data;
    if (w->updating) {
        return;
    }
    int active = gtk_combo_box_get_active (combo);
    if (active < 0) {
        return;
    }
    const char *name = g_slist_nth_data (w->device_names, active);
    if (!name) {
        return;
    }

    char conf[128];
    _get_conf_name (conf, sizeof (conf));

    char cur[256];
    deadbeef->conf_get_str (conf, "default", cur, sizeof (cur));
    if (strcmp (cur, name)) {
        deadbeef->conf_set_str (conf, name);
        deadbeef->sendmessage (DB_EV_CONFIGCHANGED, 0, 0, 0);
    }
}

static gboolean
_refill_idle_cb (gpointer data) {
    _refill (data);
    return FALSE;
}

static gboolean
_configchanged_idle_cb (gpointer data) {
    w_outdev_t *w = data;
    char filter[256] = "";
    deadbeef->conf_get_str ("pulse.device_filter", "", filter, sizeof (filter));
    if (strcmp (w->out_id, _get_out_id ()) || strcmp (w->filter, filter)) {
        // the output plugin itself changed, or the device filter changed: the
        // device list is different
        _refill (w);
    }
    else {
        // only the selected device may have changed (e.g. from Preferences)
        _sync_selection (w);
    }
    return FALSE;
}

static int
w_outdev_message (ddb_gtkui_widget_t *base, uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    switch (id) {
    case DB_EV_REINIT_SOUND:
        g_idle_add (_refill_idle_cb, base);
        break;
    case DB_EV_CONFIGCHANGED:
        g_idle_add (_configchanged_idle_cb, base);
        break;
    }
    return 0;
}

static void
w_outdev_init (struct ddb_gtkui_widget_s *base) {
    _refill ((w_outdev_t *)base);
}

static void
w_outdev_destroy (struct ddb_gtkui_widget_s *base) {
    _free_names ((w_outdev_t *)base);
}

ddb_gtkui_widget_t *
w_outdev_create (void) {
    w_outdev_t *w = calloc (1, sizeof (w_outdev_t));
    w->base.widget = gtk_event_box_new ();
    w->base.flags = DDB_GTKUI_WIDGET_FLAG_NON_EXPANDABLE;
    w->base.init = w_outdev_init;
    w->base.destroy = w_outdev_destroy;
    w->base.message = w_outdev_message;

    GtkWidget *hbox = ddb_hbox_new ();
    gtk_widget_show (hbox);
    gtk_container_add (GTK_CONTAINER (w->base.widget), hbox);

    GtkWidget *label = gtk_label_new_with_mnemonic (_ ("_Output device:"));
    gtk_widget_show (label);
    gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);

    w->combobox = ddb_combo_new ();
    gtk_widget_show (w->combobox);
    gtk_box_pack_start (GTK_BOX (hbox), w->combobox, TRUE, TRUE, 0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (label), w->combobox);
    g_signal_connect ((gpointer)w->combobox, "changed", G_CALLBACK (_on_changed), w);

    // the list itself is filled from w_outdev_init, which gtkui calls once the
    // widget is added to a container

    gtkui_plugin->w_override_signals (w->base.widget, w);

    return (ddb_gtkui_widget_t *)w;
}

// --- "Next output device" action -------------------------------------------
//
// Advances the current output plugin's device to the next one in the
// enumerated list, wrapping around at the end. The selection is stored in the
// same "<output_plugin>_soundcard" conf key as the widget and Preferences, so
// the widget combo reflects the change once DB_EV_CONFIGCHANGED is delivered.

static void
_next_enum_cb (const char *name, const char *desc, void *userdata) {
    GSList **list = userdata;
    *list = g_slist_append (*list, g_strdup (name));
}

static void
_free_device_list (GSList *list) {
    for (GSList *l = list; l; l = l->next) {
        g_free (l->data);
    }
    g_slist_free (list);
}

static void
outdev_cycle_device (void) {
    DB_output_t *out = deadbeef->get_output ();
    if (!out) {
        return;
    }

    // the list starts with the default device, like the widget's list does, unless
    // the device filter hides it
    GSList *names = NULL;
    if (_default_device_allowed ()) {
        names = g_slist_append (names, g_strdup ("default"));
    }
    if (out->enum_soundcards) {
        out->enum_soundcards (_next_enum_cb, &names);
    }

    int count = g_slist_length (names);
    if (count < 2) {
        // no alternative device to switch to
        _free_device_list (names);
        return;
    }

    char conf[128];
    _get_conf_name (conf, sizeof (conf));
    char cur[256];
    deadbeef->conf_get_str (conf, "default", cur, sizeof (cur));

    int idx = 0;
    int i = 0;
    for (GSList *l = names; l; l = l->next, i++) {
        if (!strcmp ((const char *)l->data, cur)) {
            idx = i;
            break;
        }
    }

    int next = (idx + 1) % count;
    const char *name = g_slist_nth_data (names, next);
    if (name && strcmp (name, cur)) {
        deadbeef->conf_set_str (conf, name);
        deadbeef->sendmessage (DB_EV_CONFIGCHANGED, 0, 0, 0);
    }

    _free_device_list (names);
}

static int
outdev_action_next_device (DB_plugin_action_t *act, ddb_action_context_t ctx) {
    outdev_cycle_device ();
    return 0;
}

static DB_plugin_action_t action_next_output_device = {
    .title = "Playback/Next Output Device",
    .name = "next_output_device",
    .flags = DB_ACTION_COMMON,
    .callback2 = outdev_action_next_device,
    .next = NULL,
};

static DB_plugin_action_t *
outdev_get_actions (DB_playItem_t *it) {
    return &action_next_output_device;
}

static int
outdev_connect (void) {
    gtkui_plugin = (ddb_gtkui_t *)deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);
    if (!gtkui_plugin) {
        return -1;
    }
    gtkui_plugin->w_reg_widget (_ ("Output device"), 0, w_outdev_create, "outdev", NULL);
    return 0;
}

static int
outdev_disconnect (void) {
    if (gtkui_plugin) {
        gtkui_plugin->w_unreg_widget ("outdev");
    }
    return 0;
}

static DB_misc_t plugin = {
    DDB_PLUGIN_SET_API_VERSION
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type = DB_PLUGIN_MISC,
#if GTK_CHECK_VERSION(3, 0, 0)
    .plugin.id = "outdev_gtk3",
#else
    .plugin.id = "outdev_gtk2",
#endif
    .plugin.name = "Output Device Selector",
    .plugin.descr = "Audio output device selector (PulseAudio sinks, ALSA devices, ...). "
                    "Use View -> Design Mode to add it into the main window.",
    .plugin.copyright =
        "Output device selector widget plugin for DeaDBeeF Player\n"
        "\n"
        "This software is provided 'as-is', without any express or implied\n"
        "warranty.  In no event will the authors be held liable for any damages\n"
        "arising from the use of this software.\n"
        "\n"
        "Permission is granted to anyone to use this software for any purpose,\n"
        "including commercial applications, and to alter it and redistribute it\n"
        "freely, subject to the following restrictions:\n"
        "\n"
        "1. The origin of this software must not be misrepresented; you must not\n"
        " claim that you wrote the original software. If you use this software\n"
        " in a product, an acknowledgment in the product documentation would be\n"
        " appreciated but is not required.\n"
        "\n"
        "2. Altered source versions must be plainly marked as such, and must not be\n"
        " misrepresented as being the original software.\n"
        "\n"
        "3. This notice may not be removed or altered from any source distribution.\n"
    ,
    .plugin.website = "http://deadbeef.sf.net",
    .plugin.connect = outdev_connect,
    .plugin.disconnect = outdev_disconnect,
    .plugin.get_actions = outdev_get_actions,
};

DB_plugin_t *
#if GTK_CHECK_VERSION(3, 0, 0)
outdev_gtk3_load (DB_functions_t *api) {
#else
outdev_gtk2_load (DB_functions_t *api) {
#endif
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}
