/*
    DeaDBeeF -- the music player
    Copyright (C) 2009-2021 Oleksiy Yakovenko and other contributors

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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif
#include <assert.h>
#include <gtk/gtk.h>
#include <string.h>
#include <strings.h>
#include "../support.h"
#include "../interface.h"
#include "../ctmapping.h"
#include "../gtkui.h"
#include "../pluginconf.h"
#include "prefwin.h"
#include "prefwinplugins.h"


enum {
    PLUGIN_LIST_COL_TITLE,
    PLUGIN_LIST_COL_IDX,
    PLUGIN_LIST_COL_BUILTIN,
    PLUGIN_LIST_COL_HASCONFIG,
    PLUGIN_LIST_COL_LOADED,
    PLUGIN_LIST_COL_FNAME,
    PLUGIN_LIST_COL_CAN_TOGGLE,
    PLUGIN_LIST_COL_COUNT
};

// Plugins named here (file name without the extension, space separated) are
// skipped by the plugin loader when it scans the plugin folders. The core only
// reads this key while starting up, which is why the checkbox below says that
// it applies on the next start.
static const char *blacklist_key = "blacklist_plugins";

// The core reads the key into a buffer of this size, so anything longer would
// be silently cut short at startup.
static const int blacklist_max_len = 900;

static GtkWidget *prefwin;
static GtkListStore *pluginliststore;
static GtkTreeModelFilter *pluginliststore_filtered;
static GtkMenu *pluginlistmenu;
static GtkWidget *copyright_window;
static GtkWidget *plugin_state_label;

static void
set_plugin_state_label (const char *text) {
    if (plugin_state_label) {
        gtk_label_set_text (GTK_LABEL (plugin_state_label), text);
    }
}

// Strips the directory and the ".so", which is the form the loader's blacklist
// expects.
static void
blacklist_name_from_path (const char *path, char *out, size_t outlen) {
    const char *base = strrchr (path, '/');
    base = base ? base + 1 : path;
    size_t l = strlen (base);
    if (l > 3 && !strcasecmp (base + l - 3, ".so")) {
        l -= 3;
    }
    if (l > outlen - 1) {
        l = outlen - 1;
    }
    memcpy (out, base, l);
    out[l] = 0;
}

static gboolean
plugin_is_blacklisted (const char *name) {
    char buf[4096];
    deadbeef->conf_get_str (blacklist_key, "", buf, sizeof (buf));
    const char *p = buf;
    while (*p) {
        while (*p && *p <= ' ') {
            p++;
        }
        const char *e = p;
        while (*e && *e > ' ') {
            e++;
        }
        if (e > p && strlen (name) == (size_t)(e - p) && !strncmp (p, name, e - p)) {
            return TRUE;
        }
        p = e;
    }
    return FALSE;
}

// Rewrites the key with the name added or removed, and saves the config right
// away so the choice survives even if the player is killed.
static gboolean
plugin_set_blacklisted (const char *name, gboolean blacklisted) {
    char buf[4096];
    deadbeef->conf_get_str (blacklist_key, "", buf, sizeof (buf));

    GString *out = g_string_new (NULL);
    const char *p = buf;
    while (*p) {
        while (*p && *p <= ' ') {
            p++;
        }
        const char *e = p;
        while (*e && *e > ' ') {
            e++;
        }
        int same = e > p && strlen (name) == (size_t)(e - p) && !strncmp (p, name, e - p);
        if (e > p && !same) {
            if (out->len) {
                g_string_append_c (out, ' ');
            }
            g_string_append_len (out, p, e - p);
        }
        p = e;
    }
    if (blacklisted) {
        if (out->len) {
            g_string_append_c (out, ' ');
        }
        g_string_append (out, name);
    }

    gboolean ok = out->len < blacklist_max_len;
    if (ok) {
        deadbeef->conf_set_str (blacklist_key, out->str);
        deadbeef->conf_save ();
        deadbeef->sendmessage (DB_EV_CONFIGCHANGED, 0, 0, 0);
    }
    g_string_free (out, TRUE);
    return ok;
}

void
on_only_show_plugins_with_configuration1_activate
(GtkMenuItem     *menuitem,
 gpointer         user_data)
{
    GtkWidget *w = prefwin;
    GtkTreeView *tree = GTK_TREE_VIEW (lookup_widget (w, "pref_pluginlist"));
    int active = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (menuitem));
    if (active) {
        gtk_tree_view_set_model (tree, GTK_TREE_MODEL (pluginliststore_filtered));
    } else {
        gtk_tree_view_set_model (tree, GTK_TREE_MODEL (pluginliststore));
    }
}

gboolean
on_pref_pluginlist_button_press_event  (GtkWidget       *widget,
                                        GdkEventButton  *event,
                                        gpointer         user_data)
{
    if (event->button == 3) {
        gtk_menu_popup (GTK_MENU (pluginlistmenu), NULL, NULL, NULL, NULL, event->button, gtk_get_current_event_time());
        return TRUE;
    }

    return FALSE;
}

static void
plugin_pref_prop_changed_cb(ddb_pluginprefs_dialog_t *make_dialog_conf) {
    apply_conf (GTK_WIDGET (make_dialog_conf->content), &make_dialog_conf->dialog_conf, 0);
}

static void
gtkui_conf_get_str (const char *key, char *value, int len, const char *def) {
    deadbeef->conf_get_str (key, def, value, len);
}

void
on_pref_pluginlist_cursor_changed      (GtkTreeView     *treeview,
                                        gpointer         user_data)
{
    GtkTreePath *path;
    GtkTreeViewColumn *col;
    gtk_tree_view_get_cursor (treeview, &path, &col);
    if (!path || !col) {
        // reset
        return;
    }
    GtkTreeIter iter;
    GtkTreeModel *model;
    int idx;
    model = gtk_tree_view_get_model (treeview);
    if (!gtk_tree_model_get_iter (model, &iter, path)) {
        return;
    }

    gtk_tree_model_get (model, &iter, PLUGIN_LIST_COL_IDX, &idx, -1);

    DB_plugin_t **plugins = deadbeef->plug_get_list ();
    GtkWidget *w = prefwin;
    assert (w);

    // Plugins that were blacklisted (or otherwise did not load) have no entry
    // in the loaded list, so there is nothing to describe for those rows.
    if (idx < 0 || !plugins[idx]) {
        gtk_entry_set_text (GTK_ENTRY (lookup_widget (w, "plug_version")), "");
        gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (lookup_widget (w, "plug_description"))), _("Not loaded"), -1);
        gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (lookup_widget (w, "plug_license"))), "", -1);
        gtk_link_button_set_uri (GTK_LINK_BUTTON (lookup_widget (w, "weblink")), "");
        gtk_widget_set_sensitive (lookup_widget (w, "weblink"), FALSE);

        GtkWidget *container = lookup_widget (w, "plug_conf_dlg_viewport");
        GtkWidget *child = gtk_bin_get_child (GTK_BIN (container));
        if (child) {
            gtk_widget_destroy (child);
        }
        gtk_widget_hide (lookup_widget (w, "plugin_actions_btnbox"));
        return;
    }

    DB_plugin_t *p = plugins[idx];

    assert (p);

    char s[20];
    snprintf (s, sizeof (s), "%d.%d", p->version_major, p->version_minor);
    gtk_entry_set_text (GTK_ENTRY (lookup_widget (w, "plug_version")), s);

    const char *descr = p->descr ?: "";
    const char *copyright = p->copyright ?: "";

    {
        GtkTextView *tv = GTK_TEXT_VIEW (lookup_widget (w, "plug_description"));

        GtkTextBuffer *buffer = gtk_text_buffer_new (NULL);

        gtk_text_buffer_set_text (buffer, descr, (gint)strlen(descr));
        gtk_text_view_set_buffer (GTK_TEXT_VIEW (tv), buffer);
        g_object_unref (buffer);
    }

    GtkWidget *link = lookup_widget (w, "weblink");
    if (p->website) {
        gtk_link_button_set_uri (GTK_LINK_BUTTON(link), p->website);
        gtk_widget_set_sensitive (link, TRUE);
    }
    else {
        gtk_link_button_set_uri (GTK_LINK_BUTTON(link), "");
        gtk_widget_set_sensitive (link, FALSE);
    }

    {
        GtkTextView *lictv = GTK_TEXT_VIEW (lookup_widget (w, "plug_license"));
        GtkTextBuffer *buffer = gtk_text_buffer_new (NULL);

        gtk_text_buffer_set_text (buffer, copyright, (gint)strlen(copyright));
        gtk_text_view_set_buffer (GTK_TEXT_VIEW (lictv), buffer);
        g_object_unref (buffer);
    }

    GtkWidget *plugin_actions_btnbox = lookup_widget (w, "plugin_actions_btnbox");

    GtkWidget *container = (lookup_widget (w, "plug_conf_dlg_viewport"));
    GtkWidget *child = gtk_bin_get_child (GTK_BIN (container));
    if (child)
        gtk_widget_destroy(child);

    if (p->configdialog) {
        ddb_dialog_t conf = {
            .title = p->name,
            .layout = p->configdialog,
            .set_param = deadbeef->conf_set_str,
            .get_param = gtkui_conf_get_str,
        };
        ddb_pluginprefs_dialog_t make_dialog_conf = {
            .dialog_conf = conf,
            .parent = prefwin,
            .prop_changed = plugin_pref_prop_changed_cb,
        };
        GtkWidget *box = gtk_vbox_new(FALSE, 0);
        gtk_widget_show (box);
        if (user_data == (void *)1) {
            apply_conf (box, &conf, 1);
        }
        make_dialog_conf.containerbox = box;
        gtk_container_add (GTK_CONTAINER (container), box);
        gtkui_make_dialog (&make_dialog_conf);

        gtk_widget_show (plugin_actions_btnbox);
    } else {
        gtk_widget_hide (plugin_actions_btnbox);
    }
}

void
on_plugin_conf_reset_btn_clicked       (GtkButton       *button,
                                        gpointer         user_data)
{
    GtkWidget *w = prefwin;
    GtkTreeView *treeview = GTK_TREE_VIEW (lookup_widget (w, "pref_pluginlist"));
    on_pref_pluginlist_cursor_changed(treeview, (void *)1);
}

void
on_pref_pluginlist_row_activated       (GtkTreeView     *treeview,
                                        GtkTreePath     *path,
                                        GtkTreeViewColumn *column,
                                        gpointer         user_data)
{
    GtkWidget *w = prefwin;
    GtkNotebook *notebook = GTK_NOTEBOOK (lookup_widget (w, "plugin_notebook"));
    gtk_notebook_set_current_page (notebook, 0);
}

void
on_plugin_conf_tab_btn_clicked         (GtkRadioButton       *button,
                                        gpointer         user_data)
{
    GtkWidget *w = prefwin;
    GtkNotebook *notebook = GTK_NOTEBOOK (lookup_widget (w, "plugin_notebook"));
    gtk_notebook_set_current_page (notebook, 0);
}


void
on_plugin_info_tab_btn_clicked         (GtkRadioButton       *button,
                                        gpointer         user_data)
{
    GtkWidget *w = prefwin;
    GtkNotebook *notebook = GTK_NOTEBOOK (lookup_widget (w, "plugin_notebook"));
    gtk_notebook_set_current_page (notebook, 1);
}


void
on_plugin_license_tab_btn_clicked      (GtkRadioButton       *button,
                                        gpointer         user_data)
{
    GtkWidget *w = prefwin;
    GtkNotebook *notebook = GTK_NOTEBOOK (lookup_widget (w, "plugin_notebook"));
    gtk_notebook_set_current_page (notebook, 2);
}

//  This code is only here so you can programmatically switch page
//  of the notebook and have the radio buttons update accordingly.
void
on_plugin_notebook_switch_page         (GtkNotebook     *notebook,
                                        GtkWidget       *page,
                                        guint            page_num,
                                        gpointer         user_data)
{
    GtkWidget *w = prefwin;
    GtkToggleButton *plugin_conf_tab_btn = GTK_TOGGLE_BUTTON (lookup_widget (w, "plugin_conf_tab_btn"));
    GtkToggleButton *plugin_info_tab_btn = GTK_TOGGLE_BUTTON (lookup_widget (w, "plugin_info_tab_btn"));
    GtkToggleButton *plugin_license_tab_btn = GTK_TOGGLE_BUTTON (lookup_widget (w, "plugin_license_tab_btn"));

    GSignalMatchType mask = G_SIGNAL_MATCH_DETAIL | G_SIGNAL_MATCH_DATA;
    GQuark detail = g_quark_from_static_string ("switch_page");
    g_signal_handlers_block_matched ((gpointer)notebook, mask, detail, 0, NULL, NULL, NULL);

    switch (page_num) {
    case 0:
        gtk_toggle_button_set_active (plugin_conf_tab_btn, 1);
        break;
    case 1:
        gtk_toggle_button_set_active (plugin_info_tab_btn, 1);
        break;
    case 2:
        gtk_toggle_button_set_active (plugin_license_tab_btn, 1);
    }
    g_signal_handlers_unblock_matched ((gpointer)notebook, mask, detail, 0, NULL, NULL, NULL);
}

// The "Loaded" checkbox. Flipping it does not touch the running player: it only
// rewrites the blacklist that the plugin loader reads at startup.
static void
on_plugin_loaded_toggled      (GtkCellRendererToggle *cell,
                               gchar                 *path_str,
                               gpointer               user_data) {
    GtkTreeView *tree = GTK_TREE_VIEW (user_data);
    GtkTreeModel *model = gtk_tree_view_get_model (tree);
    GtkTreePath *path = gtk_tree_path_new_from_string (path_str);
    GtkTreeIter iter;

    if (!path || !gtk_tree_model_get_iter (model, &iter, path)) {
        gtk_tree_path_free (path);
        return;
    }
    gtk_tree_path_free (path);

    // The tree shows either the store or its filtered view, and the path above
    // is in whatever model is currently displayed.
    if (GTK_IS_TREE_MODEL_FILTER (model)) {
        GtkTreeIter child;
        gtk_tree_model_filter_convert_iter_to_child_iter (GTK_TREE_MODEL_FILTER (model), &child, &iter);
        iter = child;
        model = GTK_TREE_MODEL (pluginliststore);
    }

    gint idx;
    gchar *fname;
    gboolean loaded;
    gtk_tree_model_get (model, &iter,
                        PLUGIN_LIST_COL_FNAME, &fname,
                        PLUGIN_LIST_COL_LOADED, &loaded,
                        PLUGIN_LIST_COL_IDX, &idx, -1);

    if (!fname || !*fname) {
        g_free (fname);
        return;
    }

    DB_plugin_t **plugins = deadbeef->plug_get_list ();
    if (idx >= 0 && plugins[idx] && plugins[idx]->type == DB_PLUGIN_GUI) {
        set_plugin_state_label (_("The interface plugin cannot be disabled."));
        g_free (fname);
        return;
    }

    char name[256];
    blacklist_name_from_path (fname, name, sizeof (name));
    g_free (fname);

    // The new enabled state is !loaded, so its blacklist state is loaded.
    if (!plugin_set_blacklisted (name, loaded)) {
        set_plugin_state_label (_("Too many plugins are disabled to record the choice."));
        return;
    }

    gtk_list_store_set (pluginliststore, &iter, PLUGIN_LIST_COL_LOADED, !loaded, -1);
    set_plugin_state_label (_("Saved. The change applies when DeaDBeeF is next started."));
}

void
prefwin_init_plugins_tab (GtkWidget *_prefwin) {
    GtkWidget *w = prefwin = _prefwin;
    GtkTreeView *tree = GTK_TREE_VIEW (lookup_widget (w, "pref_pluginlist"));
    GtkCellRenderer *rend_text = gtk_cell_renderer_text_new ();
    GtkCellRenderer *rend_toggle = gtk_cell_renderer_toggle_new ();
    // Order is: title, index, builtin, hasconfig, loaded, file name, can toggle
    GtkListStore *store = gtk_list_store_new (PLUGIN_LIST_COL_COUNT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT,
                                              G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_BOOLEAN);
    pluginliststore = store;
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes (_("Plugin"), rend_text, "text", PLUGIN_LIST_COL_TITLE, "weight", PLUGIN_LIST_COL_BUILTIN, NULL);
    gtk_tree_view_append_column (tree, col);
    gtk_tree_view_column_set_expand (col, TRUE);
    gtk_tree_view_set_headers_visible (tree, TRUE);
    g_object_set (G_OBJECT (rend_text), "ellipsize", PANGO_ELLIPSIZE_END, NULL);

    DB_plugin_t **plugins = deadbeef->plug_get_list ();
    int i;
    const char *plugindir = deadbeef->get_system_dir (DDB_SYS_DIR_PLUGIN);

    // Which files are already accounted for by a loaded plugin.
    GHashTable *loaded_files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    for (i = 0; plugins[i]; i++) {
        GtkTreeIter it;
        gtk_list_store_append (store, &it);
        const char *pluginpath = deadbeef->plug_get_path_for_plugin_ptr (plugins[i]);
        const char *fname = NULL;
        if (pluginpath) {
            const char *base = strrchr (pluginpath, '/');
            fname = base ? base + 1 : pluginpath;
            g_hash_table_add (loaded_files, g_strdup (fname));
        }
        else {
            pluginpath = plugindir;
        }

        char name[256] = "";
        if (fname) {
            blacklist_name_from_path (fname, name, sizeof (name));
        }

        // The interface plugin gets no checkbox: disabling it would leave the
        // next start with no interface at all.
        gboolean can_toggle = plugins[i]->type != DB_PLUGIN_GUI;

        gtk_list_store_set (store, &it,
                            PLUGIN_LIST_COL_TITLE, plugins[i]->name,
                            PLUGIN_LIST_COL_IDX, i,
                            PLUGIN_LIST_COL_BUILTIN, strstr(pluginpath, plugindir) ? PANGO_WEIGHT_NORMAL : PANGO_WEIGHT_BOLD,
                            PLUGIN_LIST_COL_HASCONFIG, plugins[i]->configdialog ? 1 : 0,
                            PLUGIN_LIST_COL_LOADED, !plugin_is_blacklisted (name),
                            PLUGIN_LIST_COL_FNAME, fname,
                            PLUGIN_LIST_COL_CAN_TOGGLE, can_toggle,
                            -1);
    }

    // Plugins that are in the folder but did not load - blacklisted here, or
    // broken - still need to appear, otherwise the checkbox for putting them
    // back could never be reached again.
    GDir *dir = g_dir_open (plugindir, 0, NULL);
    if (dir) {
        const char *entry;
        while ((entry = g_dir_read_name (dir))) {
            size_t l = strlen (entry);
            if (l <= 3 || strcmp (entry + l - 3, ".so")) {
                continue;
            }
            // the loader ignores these, so showing them would be misleading
            if (strstr (entry, ".fallback.") || entry[0] == '.') {
                continue;
            }
            if (g_hash_table_contains (loaded_files, entry)) {
                continue;
            }
            char name[256];
            blacklist_name_from_path (entry, name, sizeof (name));

            char title[300];
            snprintf (title, sizeof (title), "%s (%s)", name, _("not loaded"));

            GtkTreeIter it;
            gtk_list_store_append (store, &it);
            gtk_list_store_set (store, &it,
                                PLUGIN_LIST_COL_TITLE, title,
                                PLUGIN_LIST_COL_IDX, -1,
                                PLUGIN_LIST_COL_BUILTIN, PANGO_WEIGHT_NORMAL,
                                PLUGIN_LIST_COL_HASCONFIG, FALSE,
                                PLUGIN_LIST_COL_LOADED, !plugin_is_blacklisted (name),
                                PLUGIN_LIST_COL_FNAME, entry,
                                PLUGIN_LIST_COL_CAN_TOGGLE, TRUE,
                                -1);
        }
        g_dir_close (dir);
    }
    g_hash_table_destroy (loaded_files);

    // Second column: the on/off switch for the next startup.
    GtkTreeViewColumn *tog = gtk_tree_view_column_new_with_attributes (_("Loaded"), rend_toggle,
                                                                      "active", PLUGIN_LIST_COL_LOADED,
                                                                      "sensitive", PLUGIN_LIST_COL_CAN_TOGGLE, NULL);
    gtk_tree_view_append_column (tree, tog);
    g_signal_connect (rend_toggle, "toggled", G_CALLBACK (on_plugin_loaded_toggled), tree);

    // Both columns already have a fixed meaning, and sorting by the checkbox
    // would only shuffle the list for no reason.
    gtk_tree_view_column_set_clickable (col, FALSE);
    gtk_tree_view_column_set_clickable (tog, FALSE);

    gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE(store), PLUGIN_LIST_COL_TITLE, GTK_SORT_ASCENDING);

    // Create a filtered model, then we switch between the two models to show/hide configurable plugins
    pluginliststore_filtered = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new (GTK_TREE_MODEL(store), NULL));
    gtk_tree_model_filter_set_visible_column (pluginliststore_filtered, PLUGIN_LIST_COL_HASCONFIG);

    gtk_tree_view_set_model (tree, GTK_TREE_MODEL (store));

    pluginlistmenu = GTK_MENU(create_plugin_list_popup_menu ());
    gtk_menu_attach_to_widget (GTK_MENU (pluginlistmenu), GTK_WIDGET (tree), NULL);

    // Says up front that the checkbox is about the next startup, because
    // nothing visibly happens when it is flipped while running.
    plugin_state_label = gtk_label_new (_("Unchecking a plugin stops it from loading when DeaDBeeF is next started."));
    gtk_misc_set_alignment (GTK_MISC (plugin_state_label), 0, 0.5);
    gtk_label_set_line_wrap (GTK_LABEL (plugin_state_label), TRUE);
    gtk_widget_show (plugin_state_label);
    GtkWidget *info_box = lookup_widget (w, "vbox54");
    if (info_box) {
        gtk_box_pack_start (GTK_BOX (info_box), plugin_state_label, FALSE, FALSE, 0);
        gtk_box_reorder_child (GTK_BOX (info_box), plugin_state_label, 0);
    }

    // We do this here so one can leave the tabs visible in the Glade designer for convenience.
    GtkNotebook *notebook = GTK_NOTEBOOK (lookup_widget (w, "plugin_notebook"));
    gtk_notebook_set_show_tabs(notebook, FALSE);
    gtk_notebook_set_current_page(notebook, 0);

    // Some styling changes since Glade doesn't support setting it
#if GTK_CHECK_VERSION(3,12,0)
    GtkButtonBox *bbox = GTK_BUTTON_BOX (lookup_widget (w, "plugin_tabbtn_hbtnbox"));
    gtk_button_box_set_layout (bbox, GTK_BUTTONBOX_EXPAND);
#endif
}

void
prefwin_free_plugins (void) {
    prefwin = NULL;
    copyright_window = NULL;
    pluginliststore = NULL;
    pluginliststore_filtered = NULL;
    plugin_state_label = NULL;
}

static void
show_copyright_window (const char *text, const char *title, GtkWidget **pwindow) {
    if (*pwindow) {
        return;
    }
    GtkWidget *widget = *pwindow = create_helpwindow ();
    g_object_set_data (G_OBJECT (widget), "pointer", pwindow);
    g_signal_connect (widget, "delete_event", G_CALLBACK (on_gtkui_info_window_delete), pwindow);
    gtk_window_set_title (GTK_WINDOW (widget), title);
    gtk_window_set_transient_for (GTK_WINDOW (widget), GTK_WINDOW (prefwin));
    GtkWidget *txt = lookup_widget (widget, "helptext");
    GtkTextBuffer *buffer = gtk_text_buffer_new (NULL);

    gtk_text_buffer_set_text (buffer, text, (gint)strlen(text));
    gtk_text_view_set_buffer (GTK_TEXT_VIEW (txt), buffer);
    g_object_unref (buffer);
    gtk_widget_show (widget);
}

void
on_plug_copyright_clicked              (GtkButton       *button,
                                        gpointer         user_data)
{
    GtkTreeView *treeview = GTK_TREE_VIEW(lookup_widget (prefwin, "pref_pluginlist"));
    GtkTreePath *path;
    GtkTreeViewColumn *col;
    gtk_tree_view_get_cursor (treeview, &path, &col);
    if (!path || !col) {
        // reset
        return;
    }
    // The visible row is not necessarily the row's position in the plugin
    // array: the list is sorted, can be filtered, and now also carries rows for
    // plugins that did not load.
    GtkTreeModel *model = gtk_tree_view_get_model (treeview);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter (model, &iter, path)) {
        gtk_tree_path_free (path);
        return;
    }
    gtk_tree_path_free (path);

    int idx;
    gtk_tree_model_get (model, &iter, PLUGIN_LIST_COL_IDX, &idx, -1);

    DB_plugin_t **plugins = deadbeef->plug_get_list ();
    if (idx < 0 || !plugins[idx]) {
        return;
    }
    DB_plugin_t *p = plugins[idx];

    if (p->copyright) {
        show_copyright_window (p->copyright, "Copyright", &copyright_window);
    }
}

