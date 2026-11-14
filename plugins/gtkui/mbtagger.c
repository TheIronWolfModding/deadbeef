/*
    DeaDBeeF -- the music player
    Copyright (C) 2009-2026 Oleksiy Yakovenko and other contributors

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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <gtk/gtk.h>

#include <deadbeef/deadbeef.h>

#include "gtkui.h"
#include "support.h"
#include "mbtagger.h"
#include "musicbrainz.h"
#include "trkproperties_shared.h"

//#define trace(...) { fprintf(stderr, __VA_ARGS__); }
#define trace(fmt,...)

extern DB_functions_t *deadbeef;

enum {
    MB_LOOKUP_DISCID,
    MB_LOOKUP_SEARCH,
    MB_LOOKUP_RELEASE_ID,
    MB_LOOKUP_RELEASE_GROUP_ID,
};

// a modal window with a message, a progress bar and a cancel button, used both
// while querying the server and while writing the tags
typedef struct mb_progress_s {
    GtkWidget *window;
    GtkWidget *label;
    GtkWidget *bar;
    volatile int *abort;
} mb_progress_t;

typedef struct mb_job_s {
    int type;
    ddb_playlist_t *plt;
    DB_playItem_t **tracks;
    int numtracks;
    char *artist;
    char *album;
    char *mbid;

    mb_result_t *result;
    volatile int abort;

    mb_progress_t progress;
    guint pulse_source;
    intptr_t tid;
} mb_job_t;

typedef struct mb_write_s {
    mb_job_t *job;
    volatile int abort;
    volatile int current;
    int written;

    mb_progress_t progress;
    guint poll_source;
    intptr_t tid;
} mb_write_t;

// the query running right now, only one is supported at a time
static mb_job_t *_job = NULL;

static void _job_free (mb_job_t *job);
static void _show_release_picker (mb_job_t *job);
static gboolean _query_finished_cb (gpointer ctx);
static gboolean _write_tags_finished_cb (gpointer ctx);

#pragma mark - helpers

static void
_show_error (const char *message) {
    GtkWidget *dlg = gtk_message_dialog_new (mainwin ? GTK_WINDOW (mainwin) : NULL,
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "%s", message);
    gtk_dialog_run (GTK_DIALOG (dlg));
    gtk_widget_destroy (dlg);
}

static void
_open_url (const char *url) {
#ifdef __APPLE__
    const char *opener = "open";
#else
    const char *opener = "xdg-open";
#endif
    char *quoted = g_shell_quote (url);
    char *command = g_strdup_printf ("%s %s >/dev/null 2>&1 &", opener, quoted);
    if (system (command) != 0) {
        _show_error (_("Failed to open the URL in the web browser."));
    }
    g_free (command);
    g_free (quoted);
}

// joins two values into one column value, like foo_musicbrainz does
static void
_slash (char *out, int size, const char *one, const char *two) {
    const char *a = (one && *one) ? one : "-";
    const char *b = (two && *two) ? two : "-";
    snprintf (out, size, "%s/%s", a, b);
}

#pragma mark - progress window

static void
_progress_cancel_clicked (GtkButton *button, void *ctx) {
    mb_progress_t *p = ctx;
    if (p->abort) {
        *p->abort = 1;
    }
    gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
}

static gboolean
_progress_delete_event (GtkWidget *widget, GdkEvent *event, void *ctx) {
    mb_progress_t *p = ctx;
    if (p->abort) {
        *p->abort = 1;
    }
    return TRUE; // the worker closes the window when it's done
}

static void
_progress_create (mb_progress_t *p, const char *title, const char *text, volatile int *abort) {
    p->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    p->abort = abort;

    gtk_container_set_border_width (GTK_CONTAINER (p->window), 12);
    gtk_window_set_title (GTK_WINDOW (p->window), title);
    gtk_window_set_position (GTK_WINDOW (p->window), GTK_WIN_POS_CENTER_ON_PARENT);
    gtk_window_set_modal (GTK_WINDOW (p->window), TRUE);
    gtk_window_set_skip_taskbar_hint (GTK_WINDOW (p->window), TRUE);
    gtk_window_set_type_hint (GTK_WINDOW (p->window), GDK_WINDOW_TYPE_HINT_DIALOG);
    if (mainwin) {
        gtk_window_set_transient_for (GTK_WINDOW (p->window), GTK_WINDOW (mainwin));
    }

    GtkWidget *vbox = gtk_vbox_new (FALSE, 8);
    gtk_container_add (GTK_CONTAINER (p->window), vbox);

    p->label = gtk_label_new (text);
    gtk_box_pack_start (GTK_BOX (vbox), p->label, FALSE, FALSE, 0);

    p->bar = gtk_progress_bar_new ();
    gtk_box_pack_start (GTK_BOX (vbox), p->bar, FALSE, FALSE, 0);

    GtkWidget *hbox = gtk_hbox_new (FALSE, 0);
    gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, TRUE, 2);

    GtkWidget *spacer = gtk_label_new ("");
    gtk_box_pack_start (GTK_BOX (hbox), spacer, TRUE, TRUE, 0);

    GtkWidget *cancel = gtk_button_new_with_mnemonic (_("_Cancel"));
    gtk_box_pack_start (GTK_BOX (hbox), cancel, FALSE, FALSE, 0);

    g_signal_connect (p->window, "delete-event", G_CALLBACK (_progress_delete_event), p);
    g_signal_connect (cancel, "clicked", G_CALLBACK (_progress_cancel_clicked), p);

    gtk_widget_show_all (p->window);
    gtk_window_present (GTK_WINDOW (p->window));
}

static void
_progress_destroy (mb_progress_t *p) {
    if (p->window) {
        gtk_widget_destroy (p->window);
        p->window = NULL;
    }
    p->label = NULL;
    p->bar = NULL;
}

#pragma mark - query

static void
_job_unref_tracks (mb_job_t *job) {
    if (!job->tracks) {
        return;
    }
    int numtracks = job->numtracks;
    trkproperties_free_track_list (&job->tracks, &numtracks);
    job->tracks = NULL;
    job->numtracks = 0;
}

static void
_job_free (mb_job_t *job) {
    if (!job) {
        return;
    }
    if (job->pulse_source) {
        g_source_remove (job->pulse_source);
        job->pulse_source = 0;
    }
    mb_result_free (job->result);
    job->result = NULL;
    _job_unref_tracks (job);
    free (job->artist);
    free (job->album);
    free (job->mbid);
    if (job->plt) {
        deadbeef->plt_unref (job->plt);
    }
    free (job);
}

static mb_job_t *
_job_new (int type, ddb_action_context_t ctx) {
    mb_job_t *job = calloc (1, sizeof (mb_job_t));
    if (!job) {
        return NULL;
    }
    job->type = type;
    job->plt = deadbeef->action_get_playlist ();
    if (!job->plt) {
        free (job);
        return NULL;
    }
    trkproperties_build_track_list_for_ctx (job->plt, ctx, &job->tracks, &job->numtracks);
    if (!job->tracks || !job->numtracks) {
        deadbeef->plt_unref (job->plt);
        job->plt = NULL;
        free (job);
        return NULL;
    }
    return job;
}

// runs the query, blocks; called from a worker thread
static void
_query_worker (void *ctx) {
    mb_job_t *job = ctx;

    switch (job->type) {
    case MB_LOOKUP_DISCID:
        job->result = mb_lookup_discid (job->tracks, job->numtracks, &job->abort);
        break;

    case MB_LOOKUP_SEARCH:
        job->result = mb_search (job->artist, job->album, job->numtracks, &job->abort);
        if ((!job->result || !job->result->num_releases) && !job->abort) {
            // retry with the edition info stripped from the album name
            char *stripped = mb_strip_album_edition (job->album);
            if (stripped && strcmp (stripped, job->album ? job->album : "")) {
                mb_result_free (job->result);
                job->result = mb_search (job->artist, stripped, job->numtracks, &job->abort);
            }
            free (stripped);
        }
        break;

    case MB_LOOKUP_RELEASE_ID:
        job->result = mb_lookup_mbid ("release", job->mbid, job->numtracks, &job->abort);
        break;

    case MB_LOOKUP_RELEASE_GROUP_ID:
        job->result = mb_lookup_mbid ("release-group", job->mbid, job->numtracks, &job->abort);
        break;
    }

    g_idle_add (_query_finished_cb, job);
}

static gboolean
_query_finished_cb (gpointer ctx) {
    mb_job_t *job = ctx;
    _job = NULL;

    deadbeef->thread_join (job->tid);
    job->tid = 0;
    if (job->pulse_source) {
        g_source_remove (job->pulse_source);
        job->pulse_source = 0;
    }
    _progress_destroy (&job->progress);

    if (job->abort) {
        trace ("musicbrainz: query aborted by the user\n");
    }
    else if (job->result && job->result->failed) {
        _show_error (_("MusicBrainz returned an error, please try again later."));
    }
    else if (!job->result || !job->result->num_releases) {
        _show_error (_("No matching releases were found on MusicBrainz."));
    }
    else {
        // the picker takes ownership of the job
        _show_release_picker (job);
        return FALSE;
    }

    _job_free (job);
    return FALSE;
}

static gboolean
_query_pulse_cb (gpointer ctx) {
    mb_job_t *job = ctx;
    if (job->progress.window && GTK_IS_PROGRESS_BAR (job->progress.bar)) {
        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (job->progress.bar));
    }
    return TRUE;
}

// starts the query of an already prepared job, and takes ownership of it
static void
_start_query (mb_job_t *job, const char *text) {
    if (_job) {
        _show_error (_("Another MusicBrainz query is already running."));
        _job_free (job);
        return;
    }

    _progress_create (&job->progress, _("MusicBrainz"), text, &job->abort);
    job->pulse_source = g_timeout_add (200, _query_pulse_cb, job);

    job->tid = deadbeef->thread_start (_query_worker, job);
    if (!job->tid) {
        _progress_destroy (&job->progress);
        _job_free (job);
        _show_error (_("Failed to start the MusicBrainz query."));
        return;
    }
    _job = job;
}

#pragma mark - writing tags

static void
_write_tags_worker (void *ctx) {
    mb_write_t *w = ctx;
    DB_decoder_t **decoders = deadbeef->plug_get_decoder_list ();
    DB_playItem_t **tracks = w->job->tracks;
    int numtracks = w->job->numtracks;

    for (int i = 0; i < numtracks; i++) {
        if (w->abort) {
            break;
        }
        DB_playItem_t *it = tracks[i];

        deadbeef->pl_lock ();
        char decoder_id[100] = "";
        const char *dec = deadbeef->pl_find_meta (it, ":DECODER");
        if (dec) {
            snprintf (decoder_id, sizeof (decoder_id), "%s", dec);
        }
        const char *uri = deadbeef->pl_find_meta (it, ":URI");
        int is_local = uri && deadbeef->is_local_file (uri);
        int is_subtrack = deadbeef->pl_get_item_flags (it) & DDB_IS_SUBTRACK;
        deadbeef->pl_unlock ();

        if (is_local && decoder_id[0] && !is_subtrack) {
            for (int j = 0; decoders[j]; j++) {
                if (!strcmp (decoders[j]->plugin.id, decoder_id)) {
                    if (decoders[j]->write_metadata && decoders[j]->write_metadata (it) == 0) {
                        w->written++;
                    }
                    break;
                }
            }
        }
        w->current = i + 1;
    }

    g_idle_add (_write_tags_finished_cb, w);
}

static gboolean
_write_tags_poll_cb (gpointer ctx) {
    mb_write_t *w = ctx;
    int total = w->job->numtracks;
    if (!w->progress.window || total <= 0) {
        return TRUE;
    }
    if (GTK_IS_PROGRESS_BAR (w->progress.bar)) {
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (w->progress.bar), (double)w->current / total);
    }
    char text[64];
    snprintf (text, sizeof (text), _("Writing tags: %d of %d files"), w->current, total);
    gtk_label_set_text (GTK_LABEL (w->progress.label), text);
    return TRUE;
}

static gboolean
_write_tags_finished_cb (gpointer ctx) {
    mb_write_t *w = ctx;

    deadbeef->thread_join (w->tid);
    w->tid = 0;
    if (w->poll_source) {
        g_source_remove (w->poll_source);
        w->poll_source = 0;
    }
    _progress_destroy (&w->progress);

    mb_job_t *job = w->job;

    // let the UI know that the tags of the tracks changed
    if (job->numtracks > 25) {
        deadbeef->sendmessage (DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_CONTENT, 0);
    }
    else {
        for (int i = 0; i < job->numtracks; i++) {
            ddb_event_track_t *ev = (ddb_event_track_t *)deadbeef->event_alloc (DB_EV_TRACKINFOCHANGED);
            if (ev) {
                ev->track = job->tracks[i];
                deadbeef->pl_item_ref (ev->track);
                deadbeef->event_send ((ddb_event_t *)ev, 0, 0);
            }
        }
    }
    if (job->plt) {
        deadbeef->plt_modified (job->plt);
    }

    free (w);
    _job_free (job);
    return FALSE;
}

// applies the tags to the files in the background; takes ownership of the job
static void
_write_tags (mb_job_t *job) {
    mb_write_t *w = calloc (1, sizeof (mb_write_t));
    if (!w) {
        _job_free (job);
        return;
    }
    w->job = job;

    _progress_create (&w->progress, _("MusicBrainz"), _("Writing tags..."), &w->abort);
    w->poll_source = g_timeout_add (100, _write_tags_poll_cb, w);

    w->tid = deadbeef->thread_start (_write_tags_worker, w);
    if (!w->tid) {
        g_source_remove (w->poll_source);
        _progress_destroy (&w->progress);
        free (w);
        _job_free (job);
        _show_error (_("Failed to write the tags."));
    }
}

#pragma mark - release picker

enum {
    REL_COL_ARTIST,
    REL_COL_ALBUM,
    REL_COL_DATE,
    REL_COL_LABEL,
    REL_COL_MEDIA,
    REL_COL_DISCS,
    REL_COL_TYPE,
    REL_NUM_COLS
};

enum {
    TRK_COL_DISC,
    TRK_COL_NUMBER,
    TRK_COL_TITLE,
    TRK_COL_ARTIST,
    TRK_NUM_COLS
};

enum {
    DISC_COL_NUMBER,
    DISC_COL_TITLE,
    DISC_NUM_COLS
};

typedef struct {
    mb_job_t *job;
    GtkWidget *dialog;
    GtkListStore *relstore;
    GtkListStore *trkstore;
    GtkListStore *discstore;
    GtkWidget *disccombo;
    int release_index;
    int discnumber; // 0 means the whole release
} mb_picker_t;

static void
_picker_update_tracks (mb_picker_t *p) {
    gtk_list_store_clear (p->trkstore);

    if (p->release_index < 0 || p->release_index >= p->job->result->num_releases) {
        return;
    }
    mb_release_t *rel = &p->job->result->releases[p->release_index];

    // show the tracks of the selected disc, or of the whole release,
    // sorted by disc and track number
    const mb_track_t **ordered = calloc (rel->num_tracks, sizeof (mb_track_t *));
    if (!ordered) {
        return;
    }
    int n = 0;
    for (int i = 0; i < rel->num_tracks; i++) {
        if (p->discnumber <= 0 || rel->tracks[i].discnumber == p->discnumber) {
            ordered[n++] = &rel->tracks[i];
        }
    }
    for (int i = 1; i < n; i++) {
        const mb_track_t *tmp = ordered[i];
        int j = i - 1;
        while (j >= 0 && (ordered[j]->discnumber > tmp->discnumber
                || (ordered[j]->discnumber == tmp->discnumber && ordered[j]->tracknumber > tmp->tracknumber))) {
            ordered[j+1] = ordered[j];
            j--;
        }
        ordered[j+1] = tmp;
    }

    for (int i = 0; i < n; i++) {
        const mb_track_t *t = ordered[i];
        char num[16];
        snprintf (num, sizeof (num), "%d", t->tracknumber);
        char disc[16] = "";
        if (p->discnumber <= 0 && t->totaldiscs > 1) {
            snprintf (disc, sizeof (disc), "%d", t->discnumber);
        }
        GtkTreeIter iter;
        gtk_list_store_append (p->trkstore, &iter);
        gtk_list_store_set (p->trkstore, &iter,
                TRK_COL_DISC, disc,
                TRK_COL_NUMBER, num,
                TRK_COL_TITLE, t->title ? t->title : "",
                TRK_COL_ARTIST, t->artist ? t->artist : "",
                -1);
    }
    free (ordered);
}

static void
_picker_update_discs (mb_picker_t *p) {
    gtk_list_store_clear (p->discstore);
    if (p->release_index < 0 || p->release_index >= p->job->result->num_releases) {
        return;
    }
    mb_release_t *rel = &p->job->result->releases[p->release_index];

    GtkTreeIter iter;
    if (rel->complete || rel->num_candidate_discs == 0) {
        // all the tracks of the release were selected
        gtk_list_store_append (p->discstore, &iter);
        gtk_list_store_set (p->discstore, &iter, DISC_COL_NUMBER, 0, DISC_COL_TITLE, _("Complete release"), -1);
        gtk_combo_box_set_active (GTK_COMBO_BOX (p->disccombo), 0);
        gtk_widget_set_sensitive (p->disccombo, FALSE);
        p->discnumber = 0;
    }
    else {
        for (int i = 0; i < rel->num_candidate_discs; i++) {
            char title[64];
            snprintf (title, sizeof (title), _("Disc %d"), rel->candidate_discs[i]);
            gtk_list_store_append (p->discstore, &iter);
            gtk_list_store_set (p->discstore, &iter, DISC_COL_NUMBER, rel->candidate_discs[i], DISC_COL_TITLE, title, -1);
        }
        gtk_combo_box_set_active (GTK_COMBO_BOX (p->disccombo), 0);
        gtk_widget_set_sensitive (p->disccombo, rel->num_candidate_discs > 1);
        p->discnumber = rel->candidate_discs[0];
    }
}

static void
_picker_release_selected (GtkTreeSelection *selection, gpointer user_data) {
    mb_picker_t *p = user_data;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected (selection, NULL, &iter)) {
        p->release_index = -1;
        return;
    }
    GtkTreePath *path = gtk_tree_model_get_path (GTK_TREE_MODEL (p->relstore), &iter);
    p->release_index = gtk_tree_path_get_indices (path)[0];
    gtk_tree_path_free (path);

    _picker_update_discs (p);
    _picker_update_tracks (p);
}

static void
_picker_disc_changed (GtkComboBox *combo, gpointer user_data) {
    mb_picker_t *p = user_data;
    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter (combo, &iter)) {
        return;
    }
    int discnumber = 0;
    gtk_tree_model_get (GTK_TREE_MODEL (p->discstore), &iter, DISC_COL_NUMBER, &discnumber, -1);
    p->discnumber = discnumber;
    _picker_update_tracks (p);
}

static void
_add_text_column (GtkTreeView *view, const char *title, int col, gboolean expands) {
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes (title, renderer, "text", col, NULL);
    gtk_tree_view_column_set_resizable (column, TRUE);
    gtk_tree_view_column_set_sizing (column, expands ? GTK_TREE_VIEW_COLUMN_AUTOSIZE : GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_min_width (column, 40);
    gtk_tree_view_append_column (view, column);
}

// shows the list of found releases, and the tracks of the selected one; takes
// ownership of the job
static void
_show_release_picker (mb_job_t *job) {
    mb_picker_t p;
    memset (&p, 0, sizeof (p));
    p.job = job;
    p.release_index = -1;

    p.dialog = gtk_dialog_new_with_buttons (_("Get tags from MusicBrainz"),
            mainwin ? GTK_WINDOW (mainwin) : NULL,
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Tag Files"), GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response (GTK_DIALOG (p.dialog), GTK_RESPONSE_OK);
    gtk_window_set_default_size (GTK_WINDOW (p.dialog), 760, 520);

    GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (p.dialog));
    gtk_container_set_border_width (GTK_CONTAINER (content), 6);

    GtkWidget *vbox = gtk_vbox_new (FALSE, 6);
    gtk_box_pack_start (GTK_BOX (content), vbox, TRUE, TRUE, 0);

    // releases
    p.relstore = gtk_list_store_new (REL_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
            G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *relscroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (relscroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (relscroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (vbox), relscroll, TRUE, TRUE, 0);

    GtkWidget *relview = gtk_tree_view_new_with_model (GTK_TREE_MODEL (p.relstore));
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (relview), TRUE);
    gtk_tree_selection_set_mode (gtk_tree_view_get_selection (GTK_TREE_VIEW (relview)), GTK_SELECTION_BROWSE);
    gtk_container_add (GTK_CONTAINER (relscroll), relview);

    _add_text_column (GTK_TREE_VIEW (relview), _("Artist"), REL_COL_ARTIST, FALSE);
    _add_text_column (GTK_TREE_VIEW (relview), _("Album"), REL_COL_ALBUM, FALSE);
    _add_text_column (GTK_TREE_VIEW (relview), _("Date"), REL_COL_DATE, FALSE);
    _add_text_column (GTK_TREE_VIEW (relview), _("Label / Catalog"), REL_COL_LABEL, FALSE);
    _add_text_column (GTK_TREE_VIEW (relview), _("Media"), REL_COL_MEDIA, FALSE);
    _add_text_column (GTK_TREE_VIEW (relview), _("Discs"), REL_COL_DISCS, FALSE);
    _add_text_column (GTK_TREE_VIEW (relview), _("Type / Status"), REL_COL_TYPE, FALSE);

    // disc selector
    GtkWidget *hbox = gtk_hbox_new (FALSE, 6);
    gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
    GtkWidget *disclabel = gtk_label_new (_("Apply:"));
    gtk_box_pack_start (GTK_BOX (hbox), disclabel, FALSE, FALSE, 0);
    p.discstore = gtk_list_store_new (DISC_NUM_COLS, G_TYPE_INT, G_TYPE_STRING);
    p.disccombo = gtk_combo_box_new_with_model (GTK_TREE_MODEL (p.discstore));
    GtkCellRenderer *discrenderer = gtk_cell_renderer_text_new ();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (p.disccombo), discrenderer, TRUE);
    gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (p.disccombo), discrenderer, "text", DISC_COL_TITLE);
    gtk_box_pack_start (GTK_BOX (hbox), p.disccombo, FALSE, FALSE, 0);
    g_signal_connect (p.disccombo, "changed", G_CALLBACK (_picker_disc_changed), &p);

    // tracks of the selected release
    p.trkstore = gtk_list_store_new (TRK_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *trkscroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (trkscroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (trkscroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (vbox), trkscroll, TRUE, TRUE, 0);

    GtkWidget *trkview = gtk_tree_view_new_with_model (GTK_TREE_MODEL (p.trkstore));
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (trkview), TRUE);
    gtk_container_add (GTK_CONTAINER (trkscroll), trkview);
    _add_text_column (GTK_TREE_VIEW (trkview), _("Disc"), TRK_COL_DISC, FALSE);
    _add_text_column (GTK_TREE_VIEW (trkview), _("#"), TRK_COL_NUMBER, FALSE);
    _add_text_column (GTK_TREE_VIEW (trkview), _("Title"), TRK_COL_TITLE, TRUE);
    _add_text_column (GTK_TREE_VIEW (trkview), _("Artist"), TRK_COL_ARTIST, TRUE);

    for (int i = 0; i < job->result->num_releases; i++) {
        mb_release_t *rel = &job->result->releases[i];
        char date[128];
        _slash (date, sizeof (date), rel->date ? rel->date : rel->original_date, rel->country);
        char label[256];
        _slash (label, sizeof (label), rel->labels, rel->catalog);
        char type[128];
        _slash (type, sizeof (type), rel->type, rel->status);
        char discs[16];
        snprintf (discs, sizeof (discs), "%d", rel->totaldiscs);

        GtkTreeIter iter;
        gtk_list_store_append (p.relstore, &iter);
        gtk_list_store_set (p.relstore, &iter,
                REL_COL_ARTIST, rel->artist ? rel->artist : "",
                REL_COL_ALBUM, rel->title ? rel->title : "",
                REL_COL_DATE, date,
                REL_COL_LABEL, label,
                REL_COL_MEDIA, rel->media ? rel->media : "",
                REL_COL_DISCS, discs,
                REL_COL_TYPE, type,
                -1);
    }

    g_signal_connect (gtk_tree_view_get_selection (GTK_TREE_VIEW (relview)), "changed",
            G_CALLBACK (_picker_release_selected), &p);

    gtk_widget_show_all (p.dialog);

    // the first release is the best match
    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (p.relstore), &first)) {
        gtk_tree_selection_select_iter (gtk_tree_view_get_selection (GTK_TREE_VIEW (relview)), &first);
    }

    int response = gtk_dialog_run (GTK_DIALOG (p.dialog));
    gtk_widget_destroy (p.dialog);

    if (response != GTK_RESPONSE_OK || p.release_index < 0 || p.release_index >= job->result->num_releases) {
        _job_free (job);
        return;
    }

    mb_release_t *rel = &job->result->releases[p.release_index];
    if (mb_apply_release (rel, p.discnumber, job->tracks, job->numtracks) == 0) {
        _show_error (_("The selected release does not match the number of selected files."));
        _job_free (job);
        return;
    }

    _write_tags (job);
}

#pragma mark - lookup dialogs

static char *
_ask_for_text (const char *title, const char *label_text, const char *initial) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons (title,
            mainwin ? GTK_WINDOW (mainwin) : NULL,
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Search"), GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response (GTK_DIALOG (dlg), GTK_RESPONSE_OK);

    GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dlg));
    gtk_container_set_border_width (GTK_CONTAINER (content), 6);

    GtkWidget *table = gtk_table_new (1, 2, FALSE);
    gtk_table_set_col_spacings (GTK_TABLE (table), 6);
    gtk_box_pack_start (GTK_BOX (content), table, TRUE, TRUE, 0);

    GtkWidget *label = gtk_label_new (label_text);
    gtk_table_attach (GTK_TABLE (table), label, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);

    GtkWidget *entry = gtk_entry_new ();
    if (initial && *initial) {
        gtk_entry_set_text (GTK_ENTRY (entry), initial);
    }
    gtk_table_attach (GTK_TABLE (table), entry, 1, 2, 0, 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);

    gtk_widget_show_all (dlg);

    char *text = NULL;
    if (gtk_dialog_run (GTK_DIALOG (dlg)) == GTK_RESPONSE_OK) {
        const char *t = gtk_entry_get_text (GTK_ENTRY (entry));
        if (t && *t) {
            text = strdup (t);
        }
    }
    gtk_widget_destroy (dlg);
    return text;
}

// asks for the artist and album, prefilled from the tags of the tracks, and
// starts the search; takes ownership of the job
static void
_ask_for_artist_album (mb_job_t *job) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons (_("Get tags from MusicBrainz"),
            mainwin ? GTK_WINDOW (mainwin) : NULL,
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Search"), GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response (GTK_DIALOG (dlg), GTK_RESPONSE_OK);
    gtk_window_set_default_size (GTK_WINDOW (dlg), 460, -1);

    GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dlg));
    gtk_container_set_border_width (GTK_CONTAINER (content), 6);

    GtkWidget *table = gtk_table_new (2, 2, FALSE);
    gtk_table_set_col_spacings (GTK_TABLE (table), 6);
    gtk_table_set_row_spacings (GTK_TABLE (table), 6);
    gtk_box_pack_start (GTK_BOX (content), table, TRUE, TRUE, 0);

    GtkWidget *artistlabel = gtk_label_new (_("Artist:"));
    gtk_table_attach (GTK_TABLE (table), artistlabel, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
    GtkWidget *artist = gtk_entry_new ();
    if (job->artist) {
        gtk_entry_set_text (GTK_ENTRY (artist), job->artist);
    }
    gtk_table_attach (GTK_TABLE (table), artist, 1, 2, 0, 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);

    GtkWidget *albumlabel = gtk_label_new (_("Album:"));
    gtk_table_attach (GTK_TABLE (table), albumlabel, 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
    GtkWidget *album = gtk_entry_new ();
    if (job->album) {
        gtk_entry_set_text (GTK_ENTRY (album), job->album);
    }
    gtk_table_attach (GTK_TABLE (table), album, 1, 2, 1, 2, GTK_EXPAND | GTK_FILL, 0, 0, 0);

    gtk_widget_show_all (dlg);

    int response = gtk_dialog_run (GTK_DIALOG (dlg));
    if (response == GTK_RESPONSE_OK) {
        const char *artist_text = gtk_entry_get_text (GTK_ENTRY (artist));
        const char *album_text = gtk_entry_get_text (GTK_ENTRY (album));
        if (artist_text && *artist_text && album_text && *album_text) {
            free (job->artist);
            free (job->album);
            job->artist = strdup (artist_text);
            job->album = strdup (album_text);
            gtk_widget_destroy (dlg);
            _start_query (job, _("Querying MusicBrainz for releases..."));
            return;
        }
    }
    gtk_widget_destroy (dlg);
    _job_free (job);
}

#pragma mark - actions

// the actions are chained in reverse order, so that the menu shows them in the
// order of the MusicBrainz entries in foobar
static DB_plugin_action_t action_mb_by_toc;
static DB_plugin_action_t action_mb_by_artist_album;
static DB_plugin_action_t action_mb_by_release_id;
static DB_plugin_action_t action_mb_by_release_group_id;
static DB_plugin_action_t action_mb_add_toc;

static int
_action_mb_by_toc (struct DB_plugin_action_s *action, ddb_action_context_t ctx) {
    mb_job_t *job = _job_new (MB_LOOKUP_DISCID, ctx);
    if (!job) {
        return 0;
    }
    if (job->numtracks > 99) {
        _show_error (_("Please select no more than 99 tracks."));
        _job_free (job);
        return 0;
    }
    if (!mb_check_toc (job->tracks, job->numtracks)) {
        _show_error (_("The selected files don't look like a CD. The sample rate of every file "
                "must be 44100 Hz or 48000 Hz, and the length of every file must be a whole "
                "number of CD sectors."));
        _job_free (job);
        return 0;
    }
    _start_query (job, _("Querying MusicBrainz for the disc ID..."));
    return 0;
}

static int
_action_mb_by_artist_album (struct DB_plugin_action_s *action, ddb_action_context_t ctx) {
    mb_job_t *job = _job_new (MB_LOOKUP_SEARCH, ctx);
    if (!job) {
        return 0;
    }
    mb_find_common_artist_album (job->tracks, job->numtracks, &job->artist, &job->album);
    _ask_for_artist_album (job);
    return 0;
}

static int
_action_mb_by_release_id (struct DB_plugin_action_s *action, ddb_action_context_t ctx) {
    mb_job_t *job = _job_new (MB_LOOKUP_RELEASE_ID, ctx);
    if (!job) {
        return 0;
    }
    job->mbid = mb_find_common_mbid (job->tracks, job->numtracks, "MUSICBRAINZ_ALBUMID");
    char *mbid = _ask_for_text (_("MusicBrainz Release ID"), _("MusicBrainz Release ID:"), job->mbid);
    if (!mbid) {
        _job_free (job);
        return 0;
    }
    free (job->mbid);
    job->mbid = mbid;
    if (!mb_is_uuid (mbid)) {
        _show_error (_("This is not a valid MusicBrainz ID."));
        _job_free (job);
        return 0;
    }
    _start_query (job, _("Querying MusicBrainz for the release..."));
    return 0;
}

static int
_action_mb_by_release_group_id (struct DB_plugin_action_s *action, ddb_action_context_t ctx) {
    mb_job_t *job = _job_new (MB_LOOKUP_RELEASE_GROUP_ID, ctx);
    if (!job) {
        return 0;
    }
    job->mbid = mb_find_common_mbid (job->tracks, job->numtracks, "MUSICBRAINZ_RELEASEGROUPID");
    char *mbid = _ask_for_text (_("MusicBrainz Release Group ID"), _("MusicBrainz Release Group ID:"), job->mbid);
    if (!mbid) {
        _job_free (job);
        return 0;
    }
    free (job->mbid);
    job->mbid = mbid;
    if (!mb_is_uuid (mbid)) {
        _show_error (_("This is not a valid MusicBrainz ID."));
        _job_free (job);
        return 0;
    }
    _start_query (job, _("Querying MusicBrainz for the release group..."));
    return 0;
}

static int
_action_mb_add_toc (struct DB_plugin_action_s *action, ddb_action_context_t ctx) {
    ddb_playlist_t *plt = deadbeef->action_get_playlist ();
    if (!plt) {
        return 0;
    }
    DB_playItem_t **tracks = NULL;
    int numtracks = 0;
    trkproperties_build_track_list_for_ctx (plt, ctx, &tracks, &numtracks);
    deadbeef->plt_unref (plt);
    if (!tracks || !numtracks) {
        return 0;
    }

    if (numtracks > 99 || !mb_check_toc (tracks, numtracks)) {
        _show_error (_("Only files with a sample rate of 44100 Hz can be used to submit a TOC "
                "to MusicBrainz. The length of every file must be a whole number of CD sectors, "
                "and no more than 99 files may be selected."));
    }
    else {
        char *url = mb_make_toc_url (tracks, numtracks);
        if (url) {
            _open_url (url);
            free (url);
        }
        else {
            _show_error (_("Failed to calculate the TOC of the selected files."));
        }
    }

    int n = numtracks;
    trkproperties_free_track_list (&tracks, &n);
    return 0;
}

// the definitions; the next pointers are wired up in mbtagger_link_actions
static DB_plugin_action_t action_mb_add_toc = {
    .title = "Tagging/MusicBrainz/Add TOC to MusicBrainz",
    .name = "mb_add_toc",
    .flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = _action_mb_add_toc,
};

static DB_plugin_action_t action_mb_by_release_group_id = {
    .title = "Tagging/MusicBrainz/Get Tags by MusicBrainz Release Group ID",
    .name = "mb_get_tags_by_release_group_id",
    .flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = _action_mb_by_release_group_id,
};

static DB_plugin_action_t action_mb_by_release_id = {
    .title = "Tagging/MusicBrainz/Get Tags by MusicBrainz Release ID",
    .name = "mb_get_tags_by_release_id",
    .flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = _action_mb_by_release_id,
};

static DB_plugin_action_t action_mb_by_artist_album = {
    .title = "Tagging/MusicBrainz/Get Tags by Artist and Album",
    .name = "mb_get_tags_by_artist_album",
    .flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = _action_mb_by_artist_album,
};

static DB_plugin_action_t action_mb_by_toc = {
    .title = "Tagging/MusicBrainz/Get Tags by CD TOC",
    .name = "mb_get_tags_by_toc",
    .flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = _action_mb_by_toc,
};

DB_plugin_action_t *
mbtagger_get_actions (DB_playItem_t *it) {
    return &action_mb_by_toc;
}

void
mbtagger_link_actions (DB_plugin_action_t *gtkui_actions) {
    action_mb_by_toc.next = &action_mb_by_artist_album;
    action_mb_by_artist_album.next = &action_mb_by_release_id;
    action_mb_by_release_id.next = &action_mb_by_release_group_id;
    action_mb_by_release_group_id.next = &action_mb_add_toc;
    action_mb_add_toc.next = gtkui_actions;
}
