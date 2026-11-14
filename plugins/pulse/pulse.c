/*
    PulseAudio output plugin for DeaDBeeF Player
    Copyright (C) 2011 Jan D. Behrens <zykure@web.de>
    Copyright (C) 2010-2012 Oleksiy Yakovenko <waker@users.sourceforge.net>
    Copyright (C) 2010 Anton Novikov <tonn.post@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#  include "../../config.h"
#endif

#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

#include <stdint.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
    #include <sys/prctl.h>
#endif

// POSIX regex, used for the output device filter. mingw has no <regex.h>, there
// the filter degrades to a case-insensitive substring match (see
// pulse_str_contains).
#ifndef _WIN32
    #include <regex.h>
#endif

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
    #include <ctype.h>
#endif
#include <deadbeef/deadbeef.h>

#define trace(...) { deadbeef->log_detailed (&plugin.plugin, 0, __VA_ARGS__); }

static DB_output_t plugin;
DB_functions_t * deadbeef;

// serveraddr2 is a version bump, since the handling has changed, and "default"
// value has different meaning.
#define CONFSTR_PULSE_SERVERADDR "pulse.serveraddr2"
#define CONFSTR_PULSE_BUFFERSIZE "pulse.buffersize"
// Output device (sink) name. The key is <output_plugin_id>_soundcard, which is
// what gtkui's Preferences -> Sound device dropdown reads and writes.
#define CONFSTR_PULSE_SINK "pulseaudio_soundcard"
// Playback latency in milliseconds. 0 (or unset) keeps PulseAudio's own
// adaptive default buffer sizing; a positive value is passed to PulseAudio as
// the target buffer length (tlength), which is what actually controls latency.
#define CONFSTR_PULSE_LATENCY_MS "pulse.latency_ms"
// Regex filtering the sinks offered to the UI (Preferences -> Sound, the "Output
// device" widget, "Next Output Device"). A sink is listed when the regex matches
// either its name or its description, case-insensitively. Unset or empty means
// "list every sink". This affects the device lists only, never the device used
// for playback, so a device hidden by the filter keeps playing if it was
// selected before the filter was set.
#define CONFSTR_PULSE_DEVICE_FILTER "pulse.device_filter"
#define PULSE_DEVICE_FILTER_MAXLEN 1000
#define PULSE_DEFAULT_BUFFERSIZE 4096

static intptr_t pulse_tid;
static int pulse_terminate;

static pa_simple *s;
static pa_sample_spec ss;
static int _setformat_requested;
static ddb_waveformat_t requested_fmt;
static ddb_playback_state_t state = DDB_PLAYBACK_STATE_STOPPED;
static uintptr_t mutex;
static int in_callback;

static int buffer_size;

// the sink / buffer size / latency that the currently open stream was created with
static char current_sink[1000] = "default";
static int current_buffer_size = PULSE_DEFAULT_BUFFERSIZE;
static int current_latency_ms = 0;

static void pulse_thread(void *context);

static int64_t
pulse_now_ms (void) {
    struct timespec ts;
    clock_gettime (CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
pulse_get_server (char *server, size_t size) {
    // migrate from 0.7.x
    deadbeef->conf_lock ();
    int has_server2 = deadbeef->conf_get_str_fast (CONFSTR_PULSE_SERVERADDR, NULL) != NULL;
    deadbeef->conf_unlock ();

    if (!has_server2) {
        deadbeef->conf_get_str ("pulse.serveraddr", "", server, size);
        // convert default
        if (!strcasecmp (server, "default")) {
            *server = 0;
        }
    }
    else {
        deadbeef->conf_get_str (CONFSTR_PULSE_SERVERADDR, "", server, size);
    }
}

typedef struct {
    void (*callback)(const char *name, const char *desc, void *);
    void *userdata;
    char filter[PULSE_DEVICE_FILTER_MAXLEN];
    // the sink used for playback: it is always reported, even when the filter
    // hides it, so the device controls never lose the current selection
    char selected[1000];
    int regex_valid;
#ifndef _WIN32
    regex_t regex;
#endif
} pulse_enum_ctx_t;

#ifdef _WIN32
// mingw has no POSIX regex -- treat the configured filter as a plain
// case-insensitive substring instead of a pattern.
static int
pulse_str_contains (const char *haystack, const char *needle) {
    if (haystack == NULL || needle == NULL) {
        return 0;
    }
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*n && tolower ((unsigned char)*h) == tolower ((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n) {
            return 1;
        }
    }
    return 0;
}
#endif

static int
pulse_sink_matches_filter (const pulse_enum_ctx_t *ctx, const char *name, const char *desc) {
    if (name != NULL && *ctx->selected && !strcmp (name, ctx->selected)) {
        // never hide the device that is in use
        return 1;
    }
    if (!ctx->filter[0]) {
        // no filter configured -- list every sink
        return 1;
    }
#ifdef _WIN32
    return pulse_str_contains (name, ctx->filter) || pulse_str_contains (desc, ctx->filter);
#else
    if (ctx->regex_valid) {
        if (regexec (&ctx->regex, name ? name : "", 0, NULL, 0) == 0) {
            return 1;
        }
        if (regexec (&ctx->regex, desc ? desc : "", 0, NULL, 0) == 0) {
            return 1;
        }
    }
    return 0;
#endif
}

static void
pulse_sink_info_cb (pa_context *c, const pa_sink_info *i, int eol, void *userdata) {
    pulse_enum_ctx_t *ctx = userdata;
    if (i != NULL && ctx->callback != NULL && pulse_sink_matches_filter (ctx, i->name, i->description)) {
        ctx->callback (i->name, i->description, ctx->userdata);
    }
}

static int
pulse_wait_context_ready (pa_mainloop *ml, pa_context *ctx, int timeout_ms) {
    int64_t deadline = pulse_now_ms () + timeout_ms;
    for (;;) {
        pa_context_state_t st = pa_context_get_state (ctx);
        if (st == PA_CONTEXT_READY) {
            return 0;
        }
        if (!PA_CONTEXT_IS_GOOD (st)) {
            return -1;
        }
        if (pulse_now_ms () > deadline) {
            return -1;
        }
        if (pa_mainloop_iterate (ml, 0, NULL) < 0) {
            return -1;
        }
        usleep (10000);
    }
}

// Enumerate PulseAudio sinks. Called by gtkui to fill the output device
// dropdown in Preferences -> Sound. The sink name reported here is what gets
// stored in the "pulseaudio_soundcard" conf key. Sinks not matching the
// "pulse.device_filter" regex are skipped.
static void
pulse_enum_soundcards (void (*callback)(const char *name, const char *desc, void *), void *userdata) {
    if (callback == NULL) {
        return;
    }

    char server[1000];
    pulse_get_server (server, sizeof (server));

    pa_mainloop *ml = pa_mainloop_new ();
    if (ml == NULL) {
        return;
    }
    pa_mainloop_api *api = pa_mainloop_get_api (ml);
    pa_context *ctx = pa_context_new (api, "Deadbeef");
    if (ctx == NULL) {
        pa_mainloop_free (ml);
        return;
    }

    if (pa_context_connect (ctx, *server ? server : NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        pa_context_unref (ctx);
        pa_mainloop_free (ml);
        return;
    }

    // compile the device filter; an invalid regex falls back to listing everything
    pulse_enum_ctx_t ectx;
    memset (&ectx, 0, sizeof (ectx));
    ectx.callback = callback;
    ectx.userdata = userdata;
    deadbeef->conf_get_str (CONFSTR_PULSE_DEVICE_FILTER, "", ectx.filter, sizeof (ectx.filter));
    deadbeef->conf_get_str (CONFSTR_PULSE_SINK, "default", ectx.selected, sizeof (ectx.selected));
    if (!strcasecmp (ectx.selected, "default")) {
        // the server default sink is not a specific device, so nothing to force into the list
        *ectx.selected = 0;
    }
#ifndef _WIN32
    if (ectx.filter[0]) {
        int rc = regcomp (&ectx.regex, ectx.filter, REG_EXTENDED | REG_ICASE);
        if (rc == 0) {
            ectx.regex_valid = 1;
        }
        else {
            // invalid expression -- don't hide any sink
            trace ("pulse: invalid device filter regex \"%s\", listing all sinks\n", ectx.filter);
            ectx.filter[0] = 0;
        }
    }
#endif

    if (pulse_wait_context_ready (ml, ctx, 3000) == 0) {
        pa_operation *op = pa_context_get_sink_info_list (ctx, pulse_sink_info_cb, &ectx);
        if (op != NULL) {
            int64_t deadline = pulse_now_ms () + 5000;
            while (pa_operation_get_state (op) == PA_OPERATION_RUNNING) {
                if (pulse_now_ms () > deadline) {
                    break;
                }
                if (pa_mainloop_iterate (ml, 0, NULL) < 0) {
                    break;
                }
                usleep (5000);
            }
            pa_operation_unref (op);
        }
    }

    pa_context_disconnect (ctx);
    pa_context_unref (ctx);
    pa_mainloop_free (ml);
#ifndef _WIN32
    if (ectx.regex_valid) {
        regfree (&ectx.regex);
    }
#endif
}

static int pulse_set_spec(ddb_waveformat_t *fmt)
{
    memcpy (&plugin.fmt, fmt, sizeof (ddb_waveformat_t));
    if (!plugin.fmt.channels) {
        // generic format
        plugin.fmt.bps = 16;
        plugin.fmt.is_float = 0;
        plugin.fmt.channels = 2;
        plugin.fmt.samplerate = 44100;
        plugin.fmt.channelmask = 3;
    }

    trace ("format %dbit %s %dch %dHz channelmask=%X\n", plugin.fmt.bps, plugin.fmt.is_float ? "float" : "int", plugin.fmt.channels, plugin.fmt.samplerate, plugin.fmt.channelmask);

    ss.channels = plugin.fmt.channels;
    // Try to auto-configure the channel map, see <pulse/channelmap.h> for details
    pa_channel_map channel_map;
    pa_channel_map_init_extend(&channel_map, ss.channels, PA_CHANNEL_MAP_WAVEEX);
    trace ("pulse: channels: %d\n", ss.channels);

    // Read samplerate from config
    ss.rate = plugin.fmt.samplerate;
    trace ("pulse: samplerate: %d\n", ss.rate);

    switch (plugin.fmt.bps) {
    case 8:
        ss.format = PA_SAMPLE_U8;
        break;
    case 16:
        ss.format = PA_SAMPLE_S16LE;
        break;
    case 24:
        ss.format = PA_SAMPLE_S24LE;
        break;
    case 32:
        if (plugin.fmt.is_float) {
            ss.format = PA_SAMPLE_FLOAT32LE;
        }
        else {
            ss.format = PA_SAMPLE_S32LE;
        }
        break;
    default:
        return -1;
    };

    if (s) {
        pa_simple_free(s);
    }

    // PulseAudio buffer attributes. When no explicit latency is configured we
    // pass NULL so PulseAudio keeps its own (adaptive) default sizing. When
    // "pulse.latency_ms" is set we request that much playback latency by
    // setting tlength (target length of the buffer, in bytes). The other
    // fields are left at -1 so PulseAudio derives them from tlength.
    pa_buffer_attr attr_storage;
    pa_buffer_attr *attr = NULL;
    int latency_ms = deadbeef->conf_get_int (CONFSTR_PULSE_LATENCY_MS, 0);
    if (latency_ms > 0 && ss.rate > 0) {
        size_t frame_size = pa_frame_size (&ss);
        uint32_t tlength = (uint32_t)((int64_t)latency_ms * ss.rate / 1000 * frame_size);
        attr_storage.maxlength = (uint32_t)-1;
        attr_storage.tlength = tlength;
        attr_storage.prebuf = (uint32_t)-1;
        attr_storage.minreq = (uint32_t)-1;
        attr_storage.fragsize = (uint32_t)-1;
        attr = &attr_storage;
        trace ("pulse: requested latency %d ms (%u bytes)\n", latency_ms, tlength);
    }

    buffer_size = deadbeef->conf_get_int(CONFSTR_PULSE_BUFFERSIZE, PULSE_DEFAULT_BUFFERSIZE);

    int error;

    // Read serveraddr from config
    char server[1000];
    pulse_get_server (server, sizeof (server));

    // Read output device (sink) from config. Empty or "default" means the
    // server default sink. This value is set by the device dropdown in
    // Preferences -> Sound.
    char sink[1000];
    deadbeef->conf_get_str (CONFSTR_PULSE_SINK, "default", sink, sizeof (sink));
    const char *dev = (*sink && strcasecmp (sink, "default")) ? sink : NULL;

    for (;;) {
        s = pa_simple_new(*server ? server : NULL, "Deadbeef", PA_STREAM_PLAYBACK, dev, "Music", &ss, &channel_map, attr, &error);

        if (s != NULL) {
            break;
        }

        if (dev != NULL) {
            // configured sink is unavailable -- fall back to the default sink
            fprintf (stderr, "pulse: failed to open sink \"%s\": %s, falling back to default\n", dev, pa_strerror (error));
            dev = NULL;
            continue;
        }

        if (ss.rate <= 192000) {
            break;
        }

        // Older pulseaudio versions couldn't handle more than 192KHz,
        // so try to lower it down
        ss.rate = plugin.fmt.samplerate = 192000;
    }

    if (s == NULL)
    {
        const char *strerr = pa_strerror (error);
        fprintf (stderr, "pa_simple_new failed: %s\n", strerr);
        return -1;
    }

    strncpy (current_sink, dev ? dev : "default", sizeof (current_sink) - 1);
    current_sink[sizeof (current_sink) - 1] = 0;
    current_buffer_size = buffer_size;
    current_latency_ms = latency_ms;

    return 0;
}

static int
pulse_init(void) {
    trace ("pulse_init\n");
    deadbeef->mutex_lock (mutex);
    state = DDB_PLAYBACK_STATE_STOPPED;
    trace ("pulse_terminate=%d\n", pulse_terminate);
    if (pulse_terminate) {
        deadbeef->mutex_unlock (mutex);
        return -1;
    }

    if (requested_fmt.samplerate != 0) {
        memcpy (&plugin.fmt, &requested_fmt, sizeof (ddb_waveformat_t));
    }

    if (0 != pulse_set_spec(&plugin.fmt)) {
        deadbeef->mutex_unlock (mutex);
        return -1;
    }

    pulse_tid = deadbeef->thread_start(pulse_thread, NULL);
    deadbeef->mutex_unlock (mutex);

    return 0;
}

static int pulse_free(void);
static int pulse_play(void);
static int pulse_pause(void);

static int
_setformat_apply (void) {
    _setformat_requested = 0;
    if (!memcmp (&requested_fmt, &plugin.fmt, sizeof (ddb_waveformat_t))) {
        return 0;
    }

    return pulse_set_spec(&requested_fmt);
}

static int
pulse_setformat (ddb_waveformat_t *fmt)
{
    deadbeef->mutex_lock(mutex);
    _setformat_requested = 1;
    memcpy (&requested_fmt, fmt, sizeof (ddb_waveformat_t));
    deadbeef->mutex_unlock(mutex);
    return 0;
}

static int pulse_free(void)
{
    trace("pulse_free\n");

    state = DDB_PLAYBACK_STATE_STOPPED;
    deadbeef->mutex_lock(mutex);

    if (!pulse_tid || pulse_terminate == 1) {
        deadbeef->mutex_unlock(mutex);
        return 0;
    }

    if (in_callback) {
        pulse_terminate = 1;
        deadbeef->mutex_unlock(mutex);
        return 0;
    }

    pulse_terminate = 1;
    deadbeef->mutex_unlock(mutex);

    deadbeef->thread_join(pulse_tid);
    pulse_terminate = 0;

    return 0;
}

static int pulse_play(void)
{
    trace ("pulse_play\n");
    deadbeef->mutex_lock (mutex);
    if (!pulse_tid)
    {
        if (pulse_init () < 0)
        {
            deadbeef->mutex_unlock (mutex);
            return -1;
        }
    }

    pa_simple_flush (s, NULL);
    state = DDB_PLAYBACK_STATE_PLAYING;
    deadbeef->mutex_unlock (mutex);

    return 0;
}

static int pulse_stop(void)
{
    trace ("pulse_stop\n");
    pulse_free();
    return 0;
}

static int pulse_pause(void)
{
    trace ("pulse_pause\n");
    pulse_free();
    state = DDB_PLAYBACK_STATE_PAUSED;
    return 0;
}

static int pulse_unpause(void)
{
    trace ("pulse_unpause\n");
    deadbeef->mutex_lock (mutex);
    if (state == DDB_PLAYBACK_STATE_PAUSED)
    {
        if (pulse_init () < 0)
        {
            deadbeef->mutex_unlock (mutex);
            return -1;
        }
        state = DDB_PLAYBACK_STATE_PLAYING;
    }

    deadbeef->mutex_unlock (mutex);

    return 0;
}

static void pulse_thread(void *context)
{
#ifdef __linux__
    prctl(PR_SET_NAME, "deadbeef-pulse", 0, 0, 0, 0);
#endif

    trace ("pulse thread started \n");
    while (!pulse_terminate)
    {
        if (state != DDB_PLAYBACK_STATE_PLAYING || !deadbeef->streamer_ok_to_read (-1))
        {
            usleep(10000);
            continue;
        }

        // setformat
        deadbeef->mutex_lock (mutex);
        int res = 0;
        if (_setformat_requested) {
            res = _setformat_apply ();
        }
        if (res != 0) {
            deadbeef->thread_detach (pulse_tid);
            pulse_terminate = 1;
            deadbeef->mutex_unlock(mutex);
            break;
        }
        deadbeef->mutex_unlock(mutex);

        int sample_size = plugin.fmt.channels * (plugin.fmt.bps / 8);
        char buf[buffer_size];

        in_callback = 1;
        int bytesread = deadbeef->streamer_read(buf, buffer_size);
        in_callback = 0;
        if (pulse_terminate) {
            break;
        }
        if (bytesread < 0) {
            bytesread = 0;
        }

        int error;

        res = 0;
        if (bytesread > 0) {
            deadbeef->mutex_lock (mutex);
            res = pa_simple_write(s, buf, bytesread, &error);
            deadbeef->mutex_unlock(mutex);
        }

        if (pulse_terminate) {
            break;
        }

        if (res < 0)
        {
            usleep(10000);
        }
    }

    deadbeef->mutex_lock (mutex);
    state = DDB_PLAYBACK_STATE_STOPPED;
    if (s)
    {
        pa_simple_drain (s, NULL);
        pa_simple_free(s);
        s = NULL;
    }
    pulse_terminate = 0;
    pulse_tid = 0;
    deadbeef->mutex_unlock (mutex);
    trace ("pulse_thread finished\n");
}

static ddb_playback_state_t pulse_get_state(void)
{
    return state;
}

static int pulse_plugin_start(void)
{
    mutex = deadbeef->mutex_create();

    return 0;
}

static int pulse_plugin_stop(void)
{
    deadbeef->mutex_free(mutex);

    return 0;
}

// React to config changes (e.g. the device chosen in Preferences -> Sound) by
// reinitializing the output when the sink or buffer size actually changed.
static int
pulse_configchanged (void) {
    char sink[1000];
    deadbeef->conf_get_str (CONFSTR_PULSE_SINK, "default", sink, sizeof (sink));
    const char *want = *sink ? sink : "default";
    int buffer = deadbeef->conf_get_int (CONFSTR_PULSE_BUFFERSIZE, PULSE_DEFAULT_BUFFERSIZE);
    int latency = deadbeef->conf_get_int (CONFSTR_PULSE_LATENCY_MS, 0);

    if (strcmp (want, current_sink) != 0 || buffer != current_buffer_size || latency != current_latency_ms) {
        deadbeef->sendmessage (DB_EV_REINIT_SOUND, 0, 0, 0);
    }
    return 0;
}

static int
pulse_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    switch (id) {
    case DB_EV_CONFIGCHANGED:
        pulse_configchanged ();
        break;
    }
    return 0;
}

DB_plugin_t * pulse_load(DB_functions_t *api)
{
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

static const char settings_dlg[] =
    "property \"PulseAudio server (leave empty for default)\" entry " CONFSTR_PULSE_SERVERADDR " \"\";\n"
    "property \"Preferred buffer size\" entry " CONFSTR_PULSE_BUFFERSIZE " " STR(PULSE_DEFAULT_BUFFERSIZE) ";\n";

static DB_output_t plugin =
{
    DDB_PLUGIN_SET_API_VERSION
    .plugin.version_major = 0,
    .plugin.version_minor = 1,
    .plugin.type = DB_PLUGIN_OUTPUT,
//    .plugin.flags = DDB_PLUGIN_FLAG_LOGGING,
    .plugin.id = "pulseaudio",
    .plugin.name = "PulseAudio output plugin",
    .plugin.descr = "At the moment of this writing, PulseAudio seems to be very unstable in many (or most) GNU/Linux distributions.\nIf you experience problems - please try switching to ALSA or OSS output.\nIf that doesn't help - please uninstall PulseAudio from your system, and try ALSA or OSS again.\nThanks for understanding",
    .plugin.copyright =
        "PulseAudio output plugin for DeaDBeeF Player\n"
        "Copyright (C) 2011 Jan D. Behrens <zykure@web.de>\n"
        "Copyright (C) 2010-2012 Oleksiy Yakovenko <waker@users.sourceforge.net>\n"
        "Copyright (C) 2010 Anton Novikov <tonn.post@gmail.com>\n"
        "\n"
        "This program is free software; you can redistribute it and/or\n"
        "modify it under the terms of the GNU General Public License\n"
        "as published by the Free Software Foundation; either version 2\n"
        "of the License, or (at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "GNU General Public License for more details.\n"
        "\n"
        "You should have received a copy of the GNU General Public License\n"
        "along with this program; if not, write to the Free Software\n"
        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n",
    .plugin.website = "http://deadbeef.sf.net",
    .plugin.start = pulse_plugin_start,
    .plugin.stop = pulse_plugin_stop,
    .plugin.message = pulse_message,
    .plugin.configdialog = settings_dlg,
    .init = pulse_init,
    .free = pulse_free,
    .setformat = pulse_setformat,
    .play = pulse_play,
    .stop = pulse_stop,
    .pause = pulse_pause,
    .unpause = pulse_unpause,
    .state = pulse_get_state,
    .enum_soundcards = pulse_enum_soundcards,
};
