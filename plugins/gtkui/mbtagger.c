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
#include <ctype.h>
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
static gboolean _cover_finished_cb (gpointer ctx);

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
    if (_job == job) {
        _job = NULL;
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
        if ((!job->result || !job->result->num_releases) && !job->abort
                && !(job->result && job->result->failed)) {
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
    // _job stays set until the job is really over, which includes the release
    // picker and writing the tags -- both run nested main loops, and the user
    // must not be able to start a second query on top of them
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
    else if (!job->result || !job->result->num_releases) {
        // a server which could not be answered at all is not an empty result,
        // and neither is a server which is too busy to answer right now
        if (job->result && job->result->busy) {
            _show_error (_("MusicBrainz is busy right now, please try again in a moment."));
        }
        else if (job->result && job->result->failed) {
            _show_error (_("MusicBrainz could not be reached, please try again later."));
        }
        else {
            _show_error (_("No matching releases were found on MusicBrainz."));
        }
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

    if (!w->abort && w->written == 0) {
        // the metadata of the items was updated either way, and the user has to
        // know that it did not reach the files
        _show_error (_("The tags could not be written to any of the files. They were only updated in the playlist."));
    }

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

#pragma mark - fetching cover art

// one folder of the selection, with the metadata needed to find its art
typedef struct {
    char *dir;
    char *mbid; // musicbrainz_albumid, may be NULL
    char *release_group_id; // musicbrainz_releasegroupid, may be NULL
    char *artist;
    char *album;
    int numtracks;
} mb_cover_album_t;

typedef struct {
    mb_cover_album_t *albums;
    int numalbums;
    volatile int abort;
    volatile int current;

    int saved;
    int skipped; // a cover file was already there
    int notfound;
    int failed;

    mb_progress_t progress;
    guint poll_source;
    intptr_t tid;
} mb_cover_task_t;

// only one cover art download at a time, so that two of them don't write the
// same file at once
static mb_cover_task_t *_cover_task;

static void
_show_info (const char *message) {
    GtkWidget *dlg = gtk_message_dialog_new (mainwin ? GTK_WINDOW (mainwin) : NULL,
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", message);
    gtk_dialog_run (GTK_DIALOG (dlg));
    gtk_widget_destroy (dlg);
}

// the cover art is always saved the way the artwork plugin looks for it, so
// that the rest of the player picks it up without rescanning anything
static char *
_cover_target_path (const char *dir, const char *image_url) {
    char name[256];
    deadbeef->conf_get_str ("musicbrainz.cover_filename", "cover.jpg", name, sizeof (name));
    if (!name[0]) {
        snprintf (name, sizeof (name), "cover.jpg");
    }

    // the extension decides how the rest of the player decodes the file, so it
    // is taken from the image rather than from the configured name
    char ext[16] = ".jpg";
    if (image_url) {
        const char *query = strchr (image_url, '?');
        const char *slash = strrchr (image_url, '/');
        const char *dot = slash ? strrchr (slash, '.') : NULL;
        if (dot && (!query || dot < query) && strlen (dot) < sizeof (ext)) {
            snprintf (ext, sizeof (ext), "%s", dot);
        }
    }

    char *base = g_strdup (name);
    char *bdot = strrchr (base, '.');
    if (bdot) {
        *bdot = 0;
    }
    char *file = g_strconcat (base, ext, NULL);
    char *path = g_build_filename (dir, file, NULL);
    g_free (file);
    g_free (base);
    return path;
}

static void
_cover_task_add_album (mb_cover_task_t *t, const char *dir,
        const char *mbid, const char *release_group_id, const char *artist, const char *album) {
    mb_cover_album_t *albums = realloc (t->albums, sizeof (mb_cover_album_t) * (t->numalbums + 1));
    if (!albums) {
        return;
    }
    t->albums = albums;
    mb_cover_album_t *a = &t->albums[t->numalbums++];
    memset (a, 0, sizeof (*a));
    a->dir = g_strdup (dir);
    a->mbid = g_strdup (mbid);
    a->release_group_id = g_strdup (release_group_id);
    a->artist = g_strdup (artist ? artist : "");
    a->album = g_strdup (album ? album : "");
}

// the playlist keeps local files as plain paths on unix, but file uris are
// possible too, and both have to work here
static char *
_cover_uri_to_path (const char *uri) {
    if (!uri || !*uri) {
        return NULL;
    }
    if (!strncmp (uri, "file://", 7)) {
        return g_filename_from_uri (uri, NULL, NULL);
    }
    if (strstr (uri, "://")) {
        return NULL;
    }
    return g_strdup (uri);
}

// groups the files by the folder they are in, and takes the MusicBrainz ids
// and the artist / album from the first file of each folder
static void
_cover_collect_albums (mb_cover_task_t *t, DB_playItem_t **tracks, int numtracks) {
    deadbeef->pl_lock ();
    for (int i = 0; i < numtracks; i++) {
        const char *uri = deadbeef->pl_find_meta (tracks[i], ":URI");
        if (!uri || !deadbeef->is_local_file (uri)) {
            continue;
        }
        char *path = _cover_uri_to_path (uri);
        if (!path) {
            continue;
        }
        char *dir = g_path_get_dirname (path);
        g_free (path);

        mb_cover_album_t *album = NULL;
        for (int j = 0; j < t->numalbums; j++) {
            if (!strcmp (t->albums[j].dir, dir)) {
                album = &t->albums[j];
                break;
            }
        }
        if (!album) {
            _cover_task_add_album (t, dir,
                    deadbeef->pl_find_meta (tracks[i], "musicbrainz_albumid"),
                    deadbeef->pl_find_meta (tracks[i], "musicbrainz_releasegroupid"),
                    deadbeef->pl_find_meta (tracks[i], "artist"),
                    deadbeef->pl_find_meta (tracks[i], "album"));
            if (t->numalbums == 0) {
                g_free (dir);
                break;
            }
            album = &t->albums[t->numalbums - 1];
        }
        album->numtracks++;
        g_free (dir);
    }
    deadbeef->pl_unlock ();
}

static void
_cover_task_free (mb_cover_task_t *t) {
    if (!t) {
        return;
    }
    if (_cover_task == t) {
        _cover_task = NULL;
    }
    for (int i = 0; i < t->numalbums; i++) {
        g_free (t->albums[i].dir);
        g_free (t->albums[i].mbid);
        g_free (t->albums[i].release_group_id);
        g_free (t->albums[i].artist);
        g_free (t->albums[i].album);
    }
    free (t->albums);
    free (t);
}

// the front image is what a music player shows as "the" cover, so it wins
// over the other types
static const mb_cover_t *
_cover_pick (const mb_cover_list_t *list) {
    const mb_cover_t *fallback = NULL;
    for (int i = 0; i < list->num_covers; i++) {
        const mb_cover_t *c = &list->covers[i];
        if (!c->image) {
            continue;
        }
        if (c->front && c->approved) {
            return c;
        }
        if (!fallback) {
            fallback = c;
        }
    }
    return fallback;
}

static void
_cover_worker (void *ctx) {
    mb_cover_task_t *t = ctx;

    for (int i = 0; i < t->numalbums && !t->abort; i++) {
        mb_cover_album_t *album = &t->albums[i];

        char *mbid = g_strdup (album->mbid);
        char *release_group_id = g_strdup (album->release_group_id);

        if ((!mbid || !*mbid) && album->album[0]) {
            // the files were not tagged by the tagger, so look the release up
            // by artist and album instead; the number of files in the folder is
            // only a hint, as a partial album would otherwise match nothing
            mb_result_t *result = mb_search (album->artist, album->album, album->numtracks, &t->abort);
            if (result && result->num_releases == 0 && album->numtracks > 0 && !t->abort) {
                mb_result_free (result);
                result = mb_search (album->artist, album->album, -1, &t->abort);
            }
            if (result && result->num_releases > 0) {
                g_free (mbid);
                mbid = g_strdup (result->releases[0].release_id);
                if (!release_group_id || !*release_group_id) {
                    g_free (release_group_id);
                    release_group_id = g_strdup (result->releases[0].release_group_id);
                }
            }
            mb_result_free (result);
        }

        if (!mbid || !*mbid) {
            // nothing could be matched, which is a missing cover, not a failure
            g_free (mbid);
            g_free (release_group_id);
            t->notfound++;
            t->current++;
            continue;
        }

        mb_cover_list_t *covers = mb_cover_fetch (mbid, release_group_id, &t->abort);
        g_free (mbid);
        g_free (release_group_id);

        const mb_cover_t *cover = covers ? _cover_pick (covers) : NULL;
        if (!cover) {
            if (covers && covers->failed) {
                t->failed++;
            }
            else {
                t->notfound++;
            }
            mb_cover_list_free (covers);
            t->current++;
            continue;
        }

        char *path = _cover_target_path (album->dir, cover->image);
        if (g_file_test (path, G_FILE_TEST_EXISTS)) {
            t->skipped++;
        }
        else {
            void *data = NULL;
            size_t size = 0;
            if (mb_cover_download (cover->image, &data, &size, &t->abort) && data) {
                GError *err = NULL;
                if (g_file_set_contents (path, data, size, &err)) {
                    t->saved++;
                }
                else {
                    t->failed++;
                }
                if (err) {
                    g_error_free (err);
                }
                free (data);
            }
            else if (!t->abort) {
                t->failed++;
            }
        }
        g_free (path);
        mb_cover_list_free (covers);
        t->current++;
    }

    g_idle_add (_cover_finished_cb, t);
}

static gboolean
_cover_finished_cb (gpointer ctx) {
    mb_cover_task_t *t = ctx;

    deadbeef->thread_join (t->tid);
    t->tid = 0;
    if (t->poll_source) {
        g_source_remove (t->poll_source);
        t->poll_source = 0;
    }
    _progress_destroy (&t->progress);

    if (!t->abort) {
        GString *msg = g_string_new (NULL);
        if (t->saved > 0) {
            g_string_append_printf (msg, _("Saved the cover art of %d album(s)."), t->saved);
        }
        if (t->skipped > 0) {
            if (msg->len) {
                g_string_append_c (msg, ' ');
            }
            g_string_append_printf (msg, _("%d album(s) already had a cover file."), t->skipped);
        }
        if (t->notfound > 0) {
            if (msg->len) {
                g_string_append_c (msg, ' ');
            }
            g_string_append_printf (msg, _("No cover art was found for %d album(s)."), t->notfound);
        }
        if (t->failed > 0) {
            if (msg->len) {
                g_string_append_c (msg, ' ');
            }
            g_string_append_printf (msg, _("The cover art of %d album(s) could not be fetched."), t->failed);
        }
        if (!msg->len) {
            g_string_append (msg, _("No cover art was found."));
        }
        _show_info (msg->str);
        g_string_free (msg, TRUE);
    }

    _cover_task_free (t);
    return FALSE;
}

static gboolean
_cover_poll_cb (gpointer ctx) {
    mb_cover_task_t *t = ctx;
    if (t->progress.window && GTK_IS_PROGRESS_BAR (t->progress.bar) && t->numalbums > 0) {
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (t->progress.bar), (double)t->current / t->numalbums);
    }
    return TRUE;
}

static int
_action_mb_fetch_cover (struct DB_plugin_action_s *action, ddb_action_context_t ctx) {
    if (_cover_task) {
        _show_error (_("A cover art download is already running."));
        return 0;
    }
    ddb_playlist_t *plt = deadbeef->action_get_playlist ();
    if (!plt) {
        return 0;
    }
    DB_playItem_t **tracks = NULL;
    int numtracks = 0;
    trkproperties_build_track_list_for_ctx (plt, ctx, &tracks, &numtracks);
    deadbeef->plt_unref (plt);

    if (!tracks || !numtracks) {
        trkproperties_free_track_list (&tracks, &numtracks);
        _show_error (_("Please select the files of a local album first."));
        return 0;
    }

    mb_cover_task_t *t = calloc (1, sizeof (mb_cover_task_t));
    if (!t) {
        trkproperties_free_track_list (&tracks, &numtracks);
        return 0;
    }
    _cover_collect_albums (t, tracks, numtracks);
    trkproperties_free_track_list (&tracks, &numtracks);

    if (t->numalbums == 0) {
        _cover_task_free (t);
        _show_error (_("Cover art can only be saved next to local files."));
        return 0;
    }

    _cover_task = t;
    _progress_create (&t->progress, _("Cover Art"), _("Fetching cover art..."), &t->abort);
    t->poll_source = g_timeout_add (100, _cover_poll_cb, t);

    t->tid = deadbeef->thread_start (_cover_worker, t);
    if (!t->tid) {
        g_source_remove (t->poll_source);
        _progress_destroy (&t->progress);
        _cover_task_free (t);
        _show_error (_("Failed to start the cover art download."));
    }
    return 0;
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
    TRK_COL_FILE,
    TRK_COL_NUMBER,
    TRK_COL_SUBTITLE,
    TRK_COL_TITLE,
    TRK_COL_ARTIST,
    TRK_COL_INDEX, // the index of the track in the release, not displayed
    TRK_NUM_COLS
};

enum {
    DISC_COL_NUMBER,
    DISC_COL_TITLE,
    DISC_NUM_COLS
};

// the release fields which can be edited in the dialog
enum {
    REL_FIELD_ARTIST,
    REL_FIELD_ALBUM,
    REL_FIELD_DATE,
    REL_FIELD_ORIGINAL_DATE,
    REL_FIELD_LABEL,
    REL_FIELD_CATALOG,
    REL_FIELD_BARCODE,
    REL_FIELD_TYPE,
    REL_FIELD_STATUS,
    REL_NUM_FIELDS
};

// the values MusicBrainz accepts for these two fields, so that they can't be
// mistyped; the empty string means the field stays unset
static const char *release_group_types[] = {
    "", "Album", "Single", "EP", "Compilation", "Soundtrack", "Spokenword",
    "Interview", "Audiobook", "Live", "Remix", "Other", NULL
};

static const char *release_statuses[] = {
    "", "Official", "Promotion", "Bootleg", "Pseudo-Release", NULL
};

typedef struct {
    mb_job_t *job;
    GtkWidget *dialog;
    GtkListStore *relstore;
    GtkListStore *trkstore;
    GtkListStore *discstore;
    GtkWidget *disccombo;
    GtkWidget *link;
    GtkWidget *fields[REL_NUM_FIELDS];
    int release_index;
    int discnumber; // 0 means the whole release
    int updating; // set while the fields are filled back from the release
} mb_picker_t;

static void
_set_string (char **dst, const char *value) {
    char *dup = value ? strdup (value) : NULL;
    free (*dst);
    *dst = dup;
}

static mb_release_t *
_picker_release (mb_picker_t *p) {
    if (p->release_index < 0 || p->release_index >= p->job->result->num_releases) {
        return NULL;
    }
    return &p->job->result->releases[p->release_index];
}

// the file name of the selected track which the release track is applied to
static void
_picker_filename (char *out, int size, DB_playItem_t *it) {
    out[0] = 0;
    if (!it) {
        return;
    }
    deadbeef->pl_lock ();
    const char *uri = deadbeef->pl_find_meta (it, ":URI");
    if (uri) {
        const char *base = uri;
        for (const char *p = uri; *p; p++) {
            if (*p == '/' || *p == '\\') {
                base = p + 1;
            }
        }
        snprintf (out, size, "%s", base);
    }
    deadbeef->pl_unlock ();
}

static void
_picker_update_tracks (mb_picker_t *p) {
    gtk_list_store_clear (p->trkstore);

    mb_release_t *rel = _picker_release (p);
    if (!rel) {
        return;
    }

    // show the tracks of the selected disc, or of the whole release,
    // sorted by disc and track number
    int *ordered = calloc (rel->num_tracks, sizeof (int));
    if (!ordered) {
        return;
    }
    int n = 0;
    for (int i = 0; i < rel->num_tracks; i++) {
        if (p->discnumber <= 0 || rel->tracks[i].discnumber == p->discnumber) {
            ordered[n++] = i;
        }
    }
    for (int i = 1; i < n; i++) {
        int tmp = ordered[i];
        int j = i - 1;
        while (j >= 0 && (rel->tracks[ordered[j]].discnumber > rel->tracks[tmp].discnumber
                || (rel->tracks[ordered[j]].discnumber == rel->tracks[tmp].discnumber
                    && rel->tracks[ordered[j]].tracknumber > rel->tracks[tmp].tracknumber))) {
            ordered[j+1] = ordered[j];
            j--;
        }
        ordered[j+1] = tmp;
    }

    for (int i = 0; i < n; i++) {
        const mb_track_t *t = &rel->tracks[ordered[i]];
        char num[32];
        snprintf (num, sizeof (num), "%d.%02d", t->discnumber, t->tracknumber);
        char file[512];
        _picker_filename (file, sizeof (file), i < p->job->numtracks ? p->job->tracks[i] : NULL);

        GtkTreeIter iter;
        gtk_list_store_append (p->trkstore, &iter);
        gtk_list_store_set (p->trkstore, &iter,
                TRK_COL_FILE, file,
                TRK_COL_NUMBER, num,
                TRK_COL_SUBTITLE, t->disc_title ? t->disc_title : "",
                TRK_COL_TITLE, t->title ? t->title : "",
                TRK_COL_ARTIST, t->artist ? t->artist : "",
                TRK_COL_INDEX, ordered[i],
                -1);
    }
    free (ordered);
}

static void
_picker_update_discs (mb_picker_t *p) {
    gtk_list_store_clear (p->discstore);
    mb_release_t *rel = _picker_release (p);
    if (!rel) {
        return;
    }

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

// writes the current values of a release back into its row of the release list
static void
_picker_refresh_release_row (mb_picker_t *p) {
    mb_release_t *rel = _picker_release (p);
    if (!rel) {
        return;
    }
    GtkTreeIter iter;
    if (!gtk_tree_model_iter_nth_child (GTK_TREE_MODEL (p->relstore), &iter, NULL, p->release_index)) {
        return;
    }

    char date[128];
    _slash (date, sizeof (date), rel->date, rel->country);
    char label[256];
    _slash (label, sizeof (label), rel->labels, rel->catalog);
    char type[128];
    _slash (type, sizeof (type), rel->type, rel->status);
    char discs[16];
    snprintf (discs, sizeof (discs), "%d", rel->totaldiscs);

    gtk_list_store_set (p->relstore, &iter,
            REL_COL_ARTIST, rel->artist ? rel->artist : "",
            REL_COL_ALBUM, rel->title ? rel->title : "",
            REL_COL_DATE, date,
            REL_COL_LABEL, label,
            REL_COL_MEDIA, rel->media ? rel->media : "",
            REL_COL_DISCS, discs,
            REL_COL_TYPE, type,
            -1);
}

// selects the value in one of the fixed combos; a value which the server sent
// but which isn't in the list is added to it, rather than dropped silently
static void
_picker_set_combo (GtkWidget *combo, const char *value) {
    GtkTreeModel *model = gtk_combo_box_get_model (GTK_COMBO_BOX (combo));
    int n = gtk_tree_model_iter_n_children (model, NULL);
    int active = 0;
    for (int i = 0; i < n; i++) {
        GtkTreeIter iter;
        char *text = NULL;
        gtk_tree_model_iter_nth_child (model, &iter, NULL, i);
        gtk_tree_model_get (model, &iter, 0, &text, -1);
        int match = value && text && !strcmp (text, value);
        g_free (text);
        if (match) {
            active = i;
            break;
        }
    }
    if (value && *value && active == 0) {
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), value);
        active = n;
    }
    gtk_combo_box_set_active (GTK_COMBO_BOX (combo), active);
}

static void
_picker_update_release_fields (mb_picker_t *p) {
    mb_release_t *rel = _picker_release (p);
    if (!rel) {
        return;
    }

    p->updating = 1;
    gtk_entry_set_text (GTK_ENTRY (p->fields[REL_FIELD_ARTIST]), rel->artist ? rel->artist : "");
    gtk_entry_set_text (GTK_ENTRY (p->fields[REL_FIELD_ALBUM]), rel->title ? rel->title : "");
    gtk_entry_set_text (GTK_ENTRY (p->fields[REL_FIELD_DATE]), rel->date ? rel->date : "");
    gtk_entry_set_text (GTK_ENTRY (p->fields[REL_FIELD_ORIGINAL_DATE]), rel->original_date ? rel->original_date : "");
    gtk_entry_set_text (GTK_ENTRY (p->fields[REL_FIELD_LABEL]), rel->labels ? rel->labels : "");
    gtk_entry_set_text (GTK_ENTRY (p->fields[REL_FIELD_CATALOG]), rel->catalog ? rel->catalog : "");
    gtk_entry_set_text (GTK_ENTRY (p->fields[REL_FIELD_BARCODE]), rel->barcode ? rel->barcode : "");
    _picker_set_combo (p->fields[REL_FIELD_TYPE], rel->type);
    _picker_set_combo (p->fields[REL_FIELD_STATUS], rel->status);
    p->updating = 0;

    char url[512];
    if (rel->release_id && *rel->release_id) {
        snprintf (url, sizeof (url), "%s/release/%s", mb_get_server (), rel->release_id);
    }
    else {
        url[0] = 0;
    }
    gtk_link_button_set_uri (GTK_LINK_BUTTON (p->link), url);
    gtk_widget_set_sensitive (p->link, url[0] != 0);
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
    _picker_update_release_fields (p);
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

// removes the spaces around the values of a ';' separated list, so that they
// can be stored the way musicbrainz.c expects them
static char *
_normalize_labels (const char *text) {
    GString *out = g_string_new (NULL);
    const char *p = text;
    while (p && *p) {
        const char *end = strchr (p, ';');
        const char *v = p;
        const char *ve = end ? end : p + strlen (p);
        while (v < ve && isspace ((unsigned char)*v)) {
            v++;
        }
        while (ve > v && isspace ((unsigned char)ve[-1])) {
            ve--;
        }
        if (ve > v) {
            if (out->len) {
                g_string_append_c (out, ';');
            }
            g_string_append_len (out, v, ve - v);
        }
        p = end ? end + 1 : NULL;
    }
    return g_string_free (out, FALSE);
}

static void
_picker_release_field_changed (GtkEditable *editable, gpointer user_data) {
    mb_picker_t *p = user_data;
    if (p->updating) {
        return;
    }
    mb_release_t *rel = _picker_release (p);
    if (!rel) {
        return;
    }

    int field = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (editable), "mbfield"));
    const char *text = gtk_entry_get_text (GTK_ENTRY (editable));
    switch (field) {
    case REL_FIELD_ARTIST:
        _set_string (&rel->artist, text);
        break;
    case REL_FIELD_ALBUM:
        _set_string (&rel->title, text);
        break;
    case REL_FIELD_DATE:
        _set_string (&rel->date, text);
        break;
    case REL_FIELD_ORIGINAL_DATE:
        _set_string (&rel->original_date, text);
        break;
    case REL_FIELD_LABEL:
        {
            char *labels = _normalize_labels (text);
            _set_string (&rel->labels, labels);
            free (labels);
        }
        break;
    case REL_FIELD_CATALOG:
        _set_string (&rel->catalog, text);
        break;
    case REL_FIELD_BARCODE:
        _set_string (&rel->barcode, text);
        break;
    }
    _picker_refresh_release_row (p);
}

static void
_picker_release_combo_changed (GtkComboBox *combo, gpointer user_data) {
    mb_picker_t *p = user_data;
    if (p->updating) {
        return;
    }
    mb_release_t *rel = _picker_release (p);
    if (!rel) {
        return;
    }

    int field = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (combo), "mbfield"));
    char *text = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (combo));
    switch (field) {
    case REL_FIELD_TYPE:
        _set_string (&rel->type, text);
        break;
    case REL_FIELD_STATUS:
        _set_string (&rel->status, text);
        break;
    }
    g_free (text);
    _picker_refresh_release_row (p);
}

static void
_picker_track_edited (GtkCellRendererText *cell, gchar *path_str, gchar *new_text, gpointer user_data) {
    mb_picker_t *p = user_data;
    mb_release_t *rel = _picker_release (p);
    int column = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (cell), "mbcolumn"));

    GtkTreePath *path = gtk_tree_path_new_from_string (path_str);
    GtkTreeIter iter;
    if (!rel || !path || !gtk_tree_model_get_iter (GTK_TREE_MODEL (p->trkstore), &iter, path)) {
        gtk_tree_path_free (path);
        return;
    }
    int index = -1;
    gtk_tree_model_get (GTK_TREE_MODEL (p->trkstore), &iter, TRK_COL_INDEX, &index, -1);
    gtk_tree_path_free (path);

    if (index < 0 || index >= rel->num_tracks) {
        return;
    }

    if (column == TRK_COL_TITLE) {
        _set_string (&rel->tracks[index].title, new_text);
    }
    else if (column == TRK_COL_ARTIST) {
        _set_string (&rel->tracks[index].artist, new_text);
    }
    else if (column == TRK_COL_SUBTITLE) {
        // the disc subtitle belongs to the whole disc
        int discnumber = rel->tracks[index].discnumber;
        for (int i = 0; i < rel->num_tracks; i++) {
            if (rel->tracks[i].discnumber == discnumber) {
                _set_string (&rel->tracks[i].disc_title, new_text);
            }
        }
        // the other rows of the same disc display the same subtitle
        GtkTreeModel *model = GTK_TREE_MODEL (p->trkstore);
        GtkTreeIter row;
        if (gtk_tree_model_get_iter_first (model, &row)) {
            do {
                int idx = -1;
                gtk_tree_model_get (model, &row, TRK_COL_INDEX, &idx, -1);
                if (idx >= 0 && idx < rel->num_tracks && rel->tracks[idx].discnumber == discnumber) {
                    gtk_list_store_set (p->trkstore, &row, column, new_text, -1);
                }
            } while (gtk_tree_model_iter_next (model, &row));
        }
    }

    gtk_list_store_set (p->trkstore, &iter, column, new_text, -1);
}

static gboolean
_picker_link_activate (GtkLinkButton *button, gpointer user_data) {
    const char *uri = gtk_link_button_get_uri (button);
    if (uri && *uri) {
        _open_url (uri);
    }
    return TRUE; // we opened it ourselves
}

static void
_add_text_column (GtkTreeView *view, const char *title, int col, gboolean expands, gboolean editable,
        GCallback edited, gpointer user_data) {
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
    g_object_set (renderer, "editable", editable, NULL);
    g_object_set_data (G_OBJECT (renderer), "mbcolumn", GINT_TO_POINTER (col));
    if (editable) {
        g_signal_connect (renderer, "edited", edited, user_data);
    }

    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes (title, renderer, "text", col, NULL);
    gtk_tree_view_column_set_resizable (column, TRUE);
    gtk_tree_view_column_set_min_width (column, 40);
    gtk_tree_view_column_set_expand (column, expands);
    gtk_tree_view_append_column (view, column);
}

static GtkWidget *
_add_field (GtkWidget *table, int left, int top, const char *title, GtkWidget *field, int id) {
    GtkWidget *label = gtk_label_new (title);
    gtk_misc_set_alignment (GTK_MISC (label), 1.0, 0.5);
    gtk_table_attach (GTK_TABLE (table), label, left, left + 1, top, top + 1, GTK_FILL, 0, 0, 0);
    gtk_table_attach (GTK_TABLE (table), field, left + 1, left + 2, top, top + 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);
    g_object_set_data (G_OBJECT (field), "mbfield", GINT_TO_POINTER (id));
    return field;
}

static GtkWidget *
_add_combo (GtkWidget *table, int left, int top, const char *title, const char **values, int id,
        GCallback changed, gpointer user_data) {
    GtkWidget *combo = gtk_combo_box_text_new ();
    for (int i = 0; values[i]; i++) {
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), values[i]);
    }
    _add_field (table, left, top, title, combo, id);
    g_signal_connect (combo, "changed", changed, user_data);
    return combo;
}

// shows the found releases, their tracks, and the tags which are going to be
// written; everything in it can be corrected before tagging, like in
// foo_musicbrainz; takes ownership of the job
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
    // the list of releases and the tag preview need a lot of room, so the
    // window always opens maximized, with the old default size as a fallback
    // for window managers which ignore the maximize request for dialogs
    gtk_window_set_default_size (GTK_WINDOW (p.dialog), 900, 640);
    gtk_window_maximize (GTK_WINDOW (p.dialog));

    GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (p.dialog));
    gtk_container_set_border_width (GTK_CONTAINER (content), 6);

    GtkWidget *vbox = gtk_vbox_new (FALSE, 6);
    gtk_box_pack_start (GTK_BOX (content), vbox, TRUE, TRUE, 0);

    // the list of the found releases
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

    for (int i = 0; i < job->result->num_releases; i++) {
        GtkTreeIter iter;
        gtk_list_store_append (p.relstore, &iter);
    }

    // the release info, which is written to the files
    GtkWidget *table = gtk_table_new (5, 4, FALSE);
    gtk_table_set_col_spacings (GTK_TABLE (table), 6);
    gtk_table_set_row_spacings (GTK_TABLE (table), 4);
    gtk_box_pack_start (GTK_BOX (vbox), table, FALSE, FALSE, 0);

    p.fields[REL_FIELD_ARTIST] = _add_field (table, 0, 0, _("Album artist:"), gtk_entry_new (), REL_FIELD_ARTIST);
    p.fields[REL_FIELD_ALBUM] = _add_field (table, 2, 0, _("Album:"), gtk_entry_new (), REL_FIELD_ALBUM);
    p.fields[REL_FIELD_DATE] = _add_field (table, 0, 1, _("Date:"), gtk_entry_new (), REL_FIELD_DATE);
    p.fields[REL_FIELD_ORIGINAL_DATE] = _add_field (table, 2, 1, _("Original date:"), gtk_entry_new (), REL_FIELD_ORIGINAL_DATE);
    p.fields[REL_FIELD_LABEL] = _add_field (table, 0, 2, _("Label(s):"), gtk_entry_new (), REL_FIELD_LABEL);
    p.fields[REL_FIELD_CATALOG] = _add_field (table, 2, 2, _("Catalog #:"), gtk_entry_new (), REL_FIELD_CATALOG);
    p.fields[REL_FIELD_BARCODE] = _add_field (table, 0, 3, _("Barcode:"), gtk_entry_new (), REL_FIELD_BARCODE);
    p.fields[REL_FIELD_TYPE] = _add_combo (table, 2, 3, _("Type:"), release_group_types, REL_FIELD_TYPE,
            G_CALLBACK (_picker_release_combo_changed), &p);
    p.fields[REL_FIELD_STATUS] = _add_combo (table, 0, 4, _("Status:"), release_statuses, REL_FIELD_STATUS,
            G_CALLBACK (_picker_release_combo_changed), &p);

    for (int i = 0; i < REL_FIELD_TYPE; i++) {
        g_signal_connect (p.fields[i], "changed", G_CALLBACK (_picker_release_field_changed), &p);
    }

    // the disc to apply, and the release page on the MusicBrainz server
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

    p.link = gtk_link_button_new_with_label ("", _("MusicBrainz release page"));
    gtk_box_pack_end (GTK_BOX (hbox), p.link, FALSE, FALSE, 0);
    g_signal_connect (p.link, "activate-link", G_CALLBACK (_picker_link_activate), &p);

    // the tracks of the selected release, next to the files they are applied to
    p.trkstore = gtk_list_store_new (TRK_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
            G_TYPE_STRING, G_TYPE_INT);
    GtkWidget *trkscroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (trkscroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (trkscroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (vbox), trkscroll, TRUE, TRUE, 0);

    GtkWidget *trkview = gtk_tree_view_new_with_model (GTK_TREE_MODEL (p.trkstore));
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (trkview), TRUE);
    gtk_container_add (GTK_CONTAINER (trkscroll), trkview);
    _add_text_column (GTK_TREE_VIEW (trkview), _("File"), TRK_COL_FILE, TRUE, FALSE, NULL, NULL);
    _add_text_column (GTK_TREE_VIEW (trkview), _("Track"), TRK_COL_NUMBER, FALSE, FALSE, NULL, NULL);
    _add_text_column (GTK_TREE_VIEW (trkview), _("Disc subtitle"), TRK_COL_SUBTITLE, FALSE, TRUE,
            G_CALLBACK (_picker_track_edited), &p);
    _add_text_column (GTK_TREE_VIEW (trkview), _("Title"), TRK_COL_TITLE, TRUE, TRUE,
            G_CALLBACK (_picker_track_edited), &p);
    _add_text_column (GTK_TREE_VIEW (trkview), _("Artist"), TRK_COL_ARTIST, TRUE, TRUE,
            G_CALLBACK (_picker_track_edited), &p);

    // the columns are filled through _picker_refresh_release_row, so that the
    // same code is used for the initial fill and for the edits
    for (int i = 0; i < job->result->num_releases; i++) {
        p.release_index = i;
        _picker_refresh_release_row (&p);
    }
    p.release_index = -1;

    g_signal_connect (gtk_tree_view_get_selection (GTK_TREE_VIEW (relview)), "changed",
            G_CALLBACK (_picker_release_selected), &p);

    _add_text_column (GTK_TREE_VIEW (relview), _("Artist"), REL_COL_ARTIST, FALSE, FALSE, NULL, NULL);
    _add_text_column (GTK_TREE_VIEW (relview), _("Album"), REL_COL_ALBUM, TRUE, FALSE, NULL, NULL);
    _add_text_column (GTK_TREE_VIEW (relview), _("Date / Country"), REL_COL_DATE, FALSE, FALSE, NULL, NULL);
    _add_text_column (GTK_TREE_VIEW (relview), _("Label(s) / Catalog #"), REL_COL_LABEL, TRUE, FALSE, NULL, NULL);
    _add_text_column (GTK_TREE_VIEW (relview), _("Media"), REL_COL_MEDIA, FALSE, FALSE, NULL, NULL);
    _add_text_column (GTK_TREE_VIEW (relview), _("Discs"), REL_COL_DISCS, FALSE, FALSE, NULL, NULL);
    _add_text_column (GTK_TREE_VIEW (relview), _("Type / Status"), REL_COL_TYPE, FALSE, FALSE, NULL, NULL);

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

// disables the accept button of a text dialog while the entry is empty
static void
_text_entry_changed (GtkEditable *editable, gpointer user_data) {
    GtkWidget **widgets = user_data; // entry, button
    const char *text = gtk_entry_get_text (GTK_ENTRY (widgets[0]));
    gtk_widget_set_sensitive (widgets[1], text && *text);
}

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

    GtkWidget *validation[2] = { entry, gtk_dialog_get_widget_for_response (GTK_DIALOG (dlg), GTK_RESPONSE_OK) };
    g_signal_connect (entry, "changed", G_CALLBACK (_text_entry_changed), validation);
    _text_entry_changed (NULL, validation);

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

typedef struct {
    GtkWidget *entry;
    char *raw; // the album name as it is tagged
    char *stripped; // with the edition info removed, which finds more matches
    int swapping;
} mb_edition_t;

static void
_toggle_edition (GtkToggleButton *button, gpointer user_data) {
    mb_edition_t *e = user_data;
    if (e->swapping) {
        return;
    }
    e->swapping = 1;
    gtk_entry_set_text (GTK_ENTRY (e->entry),
            gtk_toggle_button_get_active (button) ? e->raw : e->stripped);
    e->swapping = 0;
}

// disables the search button until both artist and album are filled in
static void
_artist_album_changed (GtkEditable *editable, gpointer user_data) {
    GtkWidget **widgets = user_data; // artist, album, button
    const char *artist = gtk_entry_get_text (GTK_ENTRY (widgets[0]));
    const char *album = gtk_entry_get_text (GTK_ENTRY (widgets[1]));
    gtk_widget_set_sensitive (widgets[2], artist && *artist && album && *album);
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

    // searching for "Album [Deluxe Edition]" usually finds nothing, so the
    // edition descriptors are stripped from the prefilled name, with a way to
    // put them back
    mb_edition_t edition;
    memset (&edition, 0, sizeof (edition));
    edition.entry = album;
    edition.raw = job->album ? strdup (job->album) : NULL;
    edition.stripped = mb_strip_album_edition (job->album);
    if (!edition.stripped) {
        edition.stripped = edition.raw ? strdup (edition.raw) : NULL;
    }
    int strips_edition = edition.raw && edition.stripped && strcmp (edition.raw, edition.stripped);
    gtk_entry_set_text (GTK_ENTRY (album), strips_edition ? edition.stripped : (edition.raw ? edition.raw : ""));
    gtk_table_attach (GTK_TABLE (table), album, 1, 2, 1, 2, GTK_EXPAND | GTK_FILL, 0, 0, 0);

    GtkWidget *validation[3] = {
        artist, album, gtk_dialog_get_widget_for_response (GTK_DIALOG (dlg), GTK_RESPONSE_OK)
    };
    g_signal_connect (artist, "changed", G_CALLBACK (_artist_album_changed), validation);
    g_signal_connect (album, "changed", G_CALLBACK (_artist_album_changed), validation);
    _artist_album_changed (NULL, validation);

    if (strips_edition) {
        GtkWidget *keep = gtk_check_button_new_with_mnemonic (_("_Keep the edition info in the album name"));
        gtk_table_attach (GTK_TABLE (table), keep, 1, 2, 2, 3, GTK_FILL, 0, 0, 0);
        g_signal_connect (keep, "toggled", G_CALLBACK (_toggle_edition), &edition);
    }

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
            free (edition.raw);
            free (edition.stripped);
            return;
        }
    }
    gtk_widget_destroy (dlg);
    free (edition.raw);
    free (edition.stripped);
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
    char *text = _ask_for_text (_("MusicBrainz Release ID"), _("MusicBrainz Release ID:"), job->mbid);
    if (!text) {
        _job_free (job);
        return 0;
    }
    free (job->mbid);
    // a release page URL copied from a browser is accepted too
    job->mbid = mb_extract_mbid (text);
    free (text);
    if (!job->mbid) {
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
    char *text = _ask_for_text (_("MusicBrainz Release Group ID"), _("MusicBrainz Release Group ID:"), job->mbid);
    if (!text) {
        _job_free (job);
        return 0;
    }
    free (job->mbid);
    job->mbid = mb_extract_mbid (text);
    free (text);
    if (!job->mbid) {
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

    if (numtracks > 99 || !mb_check_toc_submission (tracks, numtracks)) {
        _show_error (_("Only lossless files with a sample rate of 44100 Hz can be used to submit "
                "a TOC to MusicBrainz. The length of every file must be a whole number of CD "
                "sectors, and no more than 99 files may be selected."));
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

#pragma mark - preferences

// foo_musicbrainz keeps its settings in the foobar preferences; deadbeef has
// no way for a gui plugin to add a page to the preferences window, so they are
// edited from the Tagging/MusicBrainz submenu instead
static const char *const mb_setting_keys[] = {
    "musicbrainz.write_mbids",
    "musicbrainz.write_track_mbids",
    "musicbrainz.write_album_artist",
    "musicbrainz.write_label_info",
    "musicbrainz.write_album_type",
    "musicbrainz.write_album_status",
    "musicbrainz.write_country",
    "musicbrainz.write_media",
    "musicbrainz.write_isrc",
    "musicbrainz.write_asin",
    "musicbrainz.short_date",
    "musicbrainz.use_original_date",
    "musicbrainz.ascii_punctuation",
};

static const int mb_setting_defaults[] = {
    1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 0, 0,
};

#define MB_NUM_SETTINGS ((int)(sizeof (mb_setting_keys) / sizeof (mb_setting_keys[0])))

static void
_show_preferences (void) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons (_("MusicBrainz"),
            mainwin ? GTK_WINDOW (mainwin) : NULL,
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            _("_Cancel"), GTK_RESPONSE_CANCEL, _("_OK"), GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response (GTK_DIALOG (dlg), GTK_RESPONSE_OK);

    GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dlg));
    gtk_container_set_border_width (GTK_CONTAINER (content), 6);

    GtkWidget *vbox = gtk_vbox_new (FALSE, 6);
    gtk_box_pack_start (GTK_BOX (content), vbox, TRUE, TRUE, 0);

    GtkWidget *table = gtk_table_new (MB_NUM_SETTINGS / 2 + 2, 2, FALSE);
    gtk_table_set_col_spacings (GTK_TABLE (table), 12);
    gtk_table_set_row_spacings (GTK_TABLE (table), 4);
    gtk_box_pack_start (GTK_BOX (vbox), table, TRUE, TRUE, 0);

    GtkWidget *server_label = gtk_label_new (_("Server:"));
    gtk_misc_set_alignment (GTK_MISC (server_label), 0.0, 0.5);
    gtk_table_attach (GTK_TABLE (table), server_label, 0, 2, 0, 1, GTK_FILL, 0, 0, 0);

    GtkWidget *server = gtk_entry_new ();
    char value[512];
    deadbeef->conf_get_str ("musicbrainz.server", "", value, sizeof (value));
    gtk_entry_set_text (GTK_ENTRY (server), value);
    gtk_table_attach (GTK_TABLE (table), server, 0, 2, 1, 2, GTK_EXPAND | GTK_FILL, 0, 0, 0);

    GtkWidget *server_hint = gtk_label_new (_("Leave empty to use https://musicbrainz.org"));
    gtk_misc_set_alignment (GTK_MISC (server_hint), 0.0, 0.5);
    gtk_widget_set_sensitive (server_hint, FALSE);
    gtk_table_attach (GTK_TABLE (table), server_hint, 0, 2, 2, 3, GTK_FILL, 0, 0, 0);

    GtkWidget *max_label = gtk_label_new (_("Maximum releases to fetch:"));
    gtk_misc_set_alignment (GTK_MISC (max_label), 0.0, 0.5);
    gtk_table_attach (GTK_TABLE (table), max_label, 0, 1, 3, 4, GTK_FILL, 0, 0, 0);

    GtkWidget *max = gtk_spin_button_new_with_range (1, 30, 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (max), deadbeef->conf_get_int ("musicbrainz.max_releases", 30));
    gtk_table_attach (GTK_TABLE (table), max, 1, 2, 3, 4, GTK_FILL, 0, 0, 0);

    const char *titles[MB_NUM_SETTINGS] = {
        _("Write the _MusicBrainz IDs"),
        _("Write the _recording and release track IDs"),
        _("Always write the al_bum artist"),
        _("Write the lab_el, catalog number and barcode"),
        _("Write the rele_ase type"),
        _("Write the release _status"),
        _("Write the release co_untry"),
        _("Write the _media format"),
        _("Write the _ISRCs"),
        _("Write the A_SIN"),
        _("Write only the _year of the release date"),
        _("Use the _original release date instead"),
        _("Replace _typographic punctuation with ASCII"),
    };

    GtkWidget *checks[MB_NUM_SETTINGS];
    for (int i = 0; i < MB_NUM_SETTINGS; i++) {
        checks[i] = gtk_check_button_new_with_mnemonic (titles[i]);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (checks[i]),
                deadbeef->conf_get_int (mb_setting_keys[i], mb_setting_defaults[i]));
        gtk_table_attach (GTK_TABLE (table), checks[i], i % 2, i % 2 + 1, 4 + i / 2, 5 + i / 2,
                GTK_EXPAND | GTK_FILL, 0, 0, 0);
    }

    // used by "Tagging / MusicBrainz / Fetch Cover Art"
    int cover_row = 5 + MB_NUM_SETTINGS / 2;
    GtkWidget *cover_label = gtk_label_new (_("Cover art file name:"));
    gtk_misc_set_alignment (GTK_MISC (cover_label), 0.0, 0.5);
    gtk_table_attach (GTK_TABLE (table), cover_label, 0, 1, cover_row, cover_row + 1, GTK_FILL, 0, 0, 0);

    GtkWidget *cover_name = gtk_entry_new ();
    char cover_value[256];
    deadbeef->conf_get_str ("musicbrainz.cover_filename", "cover.jpg", cover_value, sizeof (cover_value));
    gtk_entry_set_text (GTK_ENTRY (cover_name), cover_value);
    gtk_table_attach (GTK_TABLE (table), cover_name, 1, 2, cover_row, cover_row + 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);

    GtkWidget *cover_hint = gtk_label_new (_("Saved next to the files by Fetch Cover Art"));
    gtk_misc_set_alignment (GTK_MISC (cover_hint), 0.0, 0.5);
    gtk_widget_set_sensitive (cover_hint, FALSE);
    gtk_table_attach (GTK_TABLE (table), cover_hint, 0, 2, cover_row + 1, cover_row + 2, GTK_FILL, 0, 0, 0);

    gtk_widget_show_all (dlg);

    if (gtk_dialog_run (GTK_DIALOG (dlg)) == GTK_RESPONSE_OK) {
        const char *text = gtk_entry_get_text (GTK_ENTRY (server));
        deadbeef->conf_set_str ("musicbrainz.server", text ? text : "");
        deadbeef->conf_set_int ("musicbrainz.max_releases", (int)gtk_spin_button_get_value (GTK_SPIN_BUTTON (max)));
        const char *cover_text = gtk_entry_get_text (GTK_ENTRY (cover_name));
        deadbeef->conf_set_str ("musicbrainz.cover_filename",
                cover_text && *cover_text ? cover_text : "cover.jpg");
        for (int i = 0; i < MB_NUM_SETTINGS; i++) {
            deadbeef->conf_set_int (mb_setting_keys[i], gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (checks[i])));
        }
        deadbeef->conf_save ();
    }
    gtk_widget_destroy (dlg);
}

static int
_action_mb_preferences (struct DB_plugin_action_s *action, ddb_action_context_t ctx) {
    _show_preferences ();
    return 0;
}

// the definitions; the next pointers are wired up in mbtagger_link_actions
static DB_plugin_action_t action_mb_preferences = {
    .title = "Tagging/MusicBrainz/Preferences",
    .name = "mb_preferences",
    .flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = _action_mb_preferences,
};

static DB_plugin_action_t action_mb_add_toc = {
    .title = "Tagging/MusicBrainz/Add TOC to MusicBrainz",
    .name = "mb_add_toc",
    .flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = _action_mb_add_toc,
};

static DB_plugin_action_t action_mb_fetch_cover = {
    .title = "Tagging/MusicBrainz/Fetch Cover Art",
    .name = "mb_fetch_cover",
    .flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = _action_mb_fetch_cover,
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
    // only one query at a time, so the busy ones are taken out of the menu
    int flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU | (_job ? DB_ACTION_DISABLED : 0);
    action_mb_by_toc.flags = flags;
    action_mb_by_artist_album.flags = flags;
    action_mb_by_release_id.flags = flags;
    action_mb_by_release_group_id.flags = flags;
    action_mb_add_toc.flags = flags;
    action_mb_fetch_cover.flags = flags;
    action_mb_preferences.flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU;
    return &action_mb_by_toc;
}

void
mbtagger_link_actions (DB_plugin_action_t *gtkui_actions) {
    action_mb_by_toc.next = &action_mb_by_artist_album;
    action_mb_by_artist_album.next = &action_mb_by_release_id;
    action_mb_by_release_id.next = &action_mb_by_release_group_id;
    action_mb_by_release_group_id.next = &action_mb_add_toc;
    action_mb_add_toc.next = &action_mb_fetch_cover;
    action_mb_fetch_cover.next = &action_mb_preferences;
    action_mb_preferences.next = gtkui_actions;
}
