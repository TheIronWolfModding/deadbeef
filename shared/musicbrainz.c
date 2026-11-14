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
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#  include <windows.h>
#endif

#include <jansson.h>

#include "musicbrainz.h"

//#define trace(...) { fprintf(stderr, __VA_ARGS__); }
#define trace(fmt,...)

#define MB_DEFAULT_SERVER "https://musicbrainz.org"
#define MB_LOOKUP_INC "artists+labels+recordings+release-groups+artist-credits+isrcs"
#define MB_MAX_TRACKS 99
#define MB_CD_FRAMES_PER_SEC 75
#define MB_DEFAULT_PREGAP_SECTORS 150
#define MB_MIN_REQUEST_INTERVAL_MS 1000
#define MB_CACHE_SIZE 64
#define MB_MAX_CANDIDATES 30
#define MB_MAX_ATTEMPTS 3
#define MB_RETRY_DELAY_MS 1500

#define MB_ID_LEN 36

extern DB_functions_t *deadbeef;

#pragma mark - string helpers

static char *
mb_strdup (const char *s) {
    if (!s) {
        return NULL;
    }
    return strdup (s);
}

static char *
mb_strdup_append (char *base, const char *sep, const char *s) {
    if (!s || !*s) {
        return base;
    }
    size_t baselen = base ? strlen (base) : 0;
    size_t seplen = sep ? strlen (sep) : 0;
    size_t len = strlen (s);
    char *out = malloc (baselen + seplen + len + 1);
    if (!out) {
        return base;
    }
    if (base) {
        memcpy (out, base, baselen);
        free (base);
    }
    if (seplen) {
        memcpy (out + baselen, sep, seplen);
    }
    memcpy (out + baselen + seplen, s, len + 1);
    return out;
}

// replaces the value, or appends it with a ';' separator for multivalue tags
static char *
mb_append_value (char *base, const char *value) {
    if (!value || !*value) {
        return base;
    }
    return mb_strdup_append (base, base ? ";" : "", value);
}

// 1 if a MusicBrainz ID starts at the beginning of the string, which may be
// longer than the ID itself
static int
mb_is_uuid_at (const char *id) {
    if (!id || strlen (id) < MB_ID_LEN) {
        return 0;
    }
    for (int i = 0; i < MB_ID_LEN; i++) {
        switch (i) {
        case 8:
        case 13:
        case 18:
        case 23:
            if (id[i] != '-') {
                return 0;
            }
            break;
        default:
            if (!isxdigit ((unsigned char)id[i])) {
                return 0;
            }
            break;
        }
    }
    return 1;
}

int
mb_is_uuid (const char *id) {
    return mb_is_uuid_at (id) && id[MB_ID_LEN] == 0;
}

char *
mb_extract_mbid (const char *text) {
    if (!text) {
        return NULL;
    }
    // pasting the whole release page URL from a browser is common, so pick the
    // first MusicBrainz ID out of the text instead of rejecting it
    for (const char *p = text; *p; p++) {
        if (mb_is_uuid_at (p)) {
            char *id = malloc (MB_ID_LEN + 1);
            if (id) {
                memcpy (id, p, MB_ID_LEN);
                id[MB_ID_LEN] = 0;
            }
            return id;
        }
    }
    return NULL;
}

#pragma mark - url

static char *
mb_url_encode (const char *s) {
    if (!s) {
        return NULL;
    }
    static const char hex[] = "0123456789ABCDEF";
    char *out = malloc (strlen (s) * 3 + 1);
    if (!out) {
        return NULL;
    }
    char *p = out;
    for (const char *c = s; *c; c++) {
        if (isalnum ((unsigned char)*c) || *c == '-' || *c == '_' || *c == '.' || *c == '~') {
            *p++ = *c;
        }
        else {
            *p++ = '%';
            *p++ = hex[((unsigned char)*c) >> 4];
            *p++ = hex[((unsigned char)*c) & 0xf];
        }
    }
    *p = 0;
    return out;
}

#pragma mark - sha1

// FIPS 180-1 sha1, only used to calculate MusicBrainz disc IDs
typedef struct {
    uint32_t state[5];
    uint64_t bitcount;
    uint8_t buffer[64];
    size_t buffered;
} mb_sha1_t;

static inline uint32_t
sha1_rol (uint32_t v, int bits) {
    return (v << bits) | (v >> (32 - bits));
}

static void
sha1_block (uint32_t state[5], const uint8_t buf[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)buf[i * 4] << 24) | ((uint32_t)buf[i * 4 + 1] << 16) | ((uint32_t)buf[i * 4 + 2] << 8) | buf[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = sha1_rol (w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999;
        }
        else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        }
        else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        }
        else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t tmp = sha1_rol (a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rol (b, 30);
        b = a;
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void
sha1_init (mb_sha1_t *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xc3d2e1f0;
    ctx->bitcount = 0;
    ctx->buffered = 0;
}

static void
sha1_update (mb_sha1_t *ctx, const void *data, size_t size) {
    const uint8_t *p = data;
    ctx->bitcount += (uint64_t)size * 8;
    if (ctx->buffered) {
        size_t need = 64 - ctx->buffered;
        if (size < need) {
            memcpy (ctx->buffer + ctx->buffered, p, size);
            ctx->buffered += size;
            return;
        }
        memcpy (ctx->buffer + ctx->buffered, p, need);
        sha1_block (ctx->state, ctx->buffer);
        ctx->buffered = 0;
        p += need;
        size -= need;
    }
    while (size >= 64) {
        sha1_block (ctx->state, p);
        p += 64;
        size -= 64;
    }
    if (size) {
        memcpy (ctx->buffer, p, size);
        ctx->buffered = size;
    }
}

static void
sha1_final (mb_sha1_t *ctx, uint8_t digest[20]) {
    uint64_t bitcount = ctx->bitcount;
    uint8_t pad = 0x80;
    sha1_update (ctx, &pad, 1);
    uint8_t zero = 0;
    while (ctx->buffered != 56) {
        sha1_update (ctx, &zero, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bitcount >> (56 - i * 8));
    }
    // the length must not be counted twice, so append it without going through sha1_update
    memcpy (ctx->buffer + 56, len, 8);
    sha1_block (ctx->state, ctx->buffer);

    for (int i = 0; i < 5; i++) {
        digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
}

// the base64 variant used by MusicBrainz: standard alphabet with / replaced
// by . and = replaced by -
static void
mb_discid_base64 (const uint8_t *digest, int size, char *out) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._";
    char *p = out;
    for (int i = 0; i < size; i += 3) {
        int remaining = size - i;
        uint32_t v = digest[i] << 16;
        if (remaining > 1) {
            v |= digest[i+1] << 8;
        }
        if (remaining > 2) {
            v |= digest[i+2];
        }
        *p++ = alphabet[(v >> 18) & 0x3f];
        *p++ = alphabet[(v >> 12) & 0x3f];
        *p++ = remaining > 1 ? alphabet[(v >> 6) & 0x3f] : '-';
        *p++ = remaining > 2 ? alphabet[v & 0x3f] : '-';
    }
    *p = 0;
}

#pragma mark - http

typedef struct {
    char *url;
    char *data;
} mb_cache_entry_t;

static mb_cache_entry_t _cache[MB_CACHE_SIZE];
static int _cache_next = 0;
static int64_t _last_request_time = 0;
static uintptr_t _mutex = 0;
static int _mutex_failed = 0;

static void
mb_lock (void) {
    if (_mutex_failed) {
        return;
    }
    if (!_mutex) {
        _mutex = deadbeef->mutex_create ();
        if (!_mutex) {
            _mutex_failed = 1;
            return;
        }
    }
    deadbeef->mutex_lock (_mutex);
}

static void
mb_unlock (void) {
    if (_mutex && !_mutex_failed) {
        deadbeef->mutex_unlock (_mutex);
    }
}

static int64_t
mb_time_ms (void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64 ();
#else
    struct timespec ts;
#  ifdef CLOCK_MONOTONIC
    clock_gettime (CLOCK_MONOTONIC, &ts);
#  else
    clock_gettime (CLOCK_REALTIME, &ts);
#  endif
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void
mb_sleep_ms (int ms) {
#ifdef _WIN32
    Sleep (ms);
#else
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    nanosleep (&ts, NULL);
#endif
}

static char *
mb_cache_get (const char *url) {
    char *data = NULL;
    mb_lock ();
    for (int i = 0; i < MB_CACHE_SIZE; i++) {
        if (_cache[i].url && !strcmp (_cache[i].url, url)) {
            data = mb_strdup (_cache[i].data);
            break;
        }
    }
    mb_unlock ();
    return data;
}

static void
mb_cache_set (const char *url, const char *data) {
    mb_lock ();
    int slot = _cache_next;
    _cache_next = (_cache_next + 1) % MB_CACHE_SIZE;
    free (_cache[slot].url);
    free (_cache[slot].data);
    _cache[slot].url = mb_strdup (url);
    _cache[slot].data = mb_strdup (data);
    mb_unlock ();
}

// waits until it's allowed to send the next request, respecting the 1 request
// per second limit of the MusicBrainz web service
static int
mb_wait_rate_limit (const volatile int *abort) {
    mb_lock ();
    int64_t now = mb_time_ms ();
    int64_t wait = _last_request_time + MB_MIN_REQUEST_INTERVAL_MS - now;
    if (wait < 0) {
        wait = 0;
    }
    _last_request_time = now + wait;
    mb_unlock ();

    while (wait > 0) {
        if (abort && *abort) {
            return 0;
        }
        int ms = wait > 100 ? 100 : (int)wait;
        mb_sleep_ms (ms);
        wait -= ms;
    }
    return !(abort && *abort);
}

static char *
mb_try_fetch (const char *url, const volatile int *abort);

// downloads the url, returns a null terminated buffer with the response, or
// NULL and sets *failed. A failed request is not the same as a valid but empty
// result -- the musicbrainz search server answers 503 every now and then
static char *
mb_fetch (const char *url, const volatile int *abort, int *failed) {
    if (failed) {
        *failed = 0;
    }

    char *cached = mb_cache_get (url);
    if (cached) {
        trace ("musicbrainz: cache hit %s\n", url);
        return cached;
    }

    for (int attempt = 0; attempt < MB_MAX_ATTEMPTS; attempt++) {
        char *data = mb_try_fetch (url, abort);
        if (data) {
            return data;
        }
        if (abort && *abort) {
            return NULL;
        }
        if (attempt + 1 < MB_MAX_ATTEMPTS) {
            // the server answers "currently busy" every now and then, and
            // gets happier the slower we come back at it
            trace ("musicbrainz: retrying %s\n", url);
            mb_sleep_ms (MB_RETRY_DELAY_MS * (attempt + 1));
        }
    }

    if (failed) {
        *failed = 1;
    }
    return NULL;
}

static char *
mb_try_fetch (const char *url, const volatile int *abort) {
    if (!mb_wait_rate_limit (abort)) {
        return NULL;
    }

    trace ("musicbrainz: GET %s\n", url);
    DB_FILE *f = deadbeef->fopen (url);
    if (!f) {
        return NULL;
    }

    size_t size = 0;
    size_t alloc = 65536;
    char *data = malloc (alloc);
    if (!data) {
        deadbeef->fclose (f);
        return NULL;
    }

    int failed = 0;
    for (;;) {
        if (abort && *abort) {
            failed = 1;
            break;
        }
        if (size + 65536 > alloc) {
            char *newdata = realloc (data, alloc * 2);
            if (!newdata) {
                failed = 1;
                break;
            }
            data = newdata;
            alloc *= 2;
        }
        size_t read = deadbeef->fread (data + size, 1, 65536, f);
        if (!read) {
            break;
        }
        size += read;
    }
    deadbeef->fclose (f);

    if (failed || !size) {
        free (data);
        return NULL;
    }
    data[size] = 0;
    mb_cache_set (url, data);
    return data;
}

#pragma mark - json helpers

static char *
mb_json_str (json_t *obj, const char *key) {
    json_t *val = json_object_get (obj, key);
    if (!json_is_string (val)) {
        return NULL;
    }
    return mb_strdup (json_string_value (val));
}

static int
mb_json_int (json_t *obj, const char *key, int def) {
    json_t *val = json_object_get (obj, key);
    if (!json_is_integer (val)) {
        return def;
    }
    return (int)json_integer_value (val);
}

// joins the artist credit of a release or a recording into a single name,
// and collects the artist ids into a ';' separated string
static void
mb_parse_artist_credit (json_t *obj, char **name, char **ids) {
    json_t *credit = json_object_get (obj, "artist-credit");
    if (!json_is_array (credit)) {
        return;
    }
    size_t index;
    json_t *part;
    json_array_foreach (credit, index, part) {
        const char *partname = json_string_value (json_object_get (part, "name"));
        if (partname) {
            *name = mb_strdup_append (*name, "", partname);
        }
        const char *join = json_string_value (json_object_get (part, "joinphrase"));
        if (join) {
            *name = mb_strdup_append (*name, "", join);
        }
        json_t *artist = json_object_get (part, "artist");
        if (json_is_object (artist)) {
            *ids = mb_append_value (*ids, json_string_value (json_object_get (artist, "id")));
        }
    }
}

static void
mb_free_track (mb_track_t *track) {
    if (!track) {
        return;
    }
    free (track->title);
    free (track->artist);
    free (track->artist_ids);
    free (track->recording_id);
    free (track->release_track_id);
    free (track->isrcs);
    free (track->disc_format);
    free (track->disc_title);
    memset (track, 0, sizeof (*track));
}

static void
mb_free_release (mb_release_t *release) {
    if (!release) {
        return;
    }
    free (release->title);
    free (release->artist);
    free (release->artist_ids);
    free (release->date);
    free (release->original_date);
    free (release->status);
    free (release->type);
    free (release->country);
    free (release->labels);
    free (release->catalog);
    free (release->barcode);
    free (release->asin);
    free (release->release_id);
    free (release->release_group_id);
    free (release->disc_id);
    free (release->media);
    for (int i = 0; i < release->num_tracks; i++) {
        mb_free_track (&release->tracks[i]);
    }
    free (release->tracks);
    free (release->candidate_discs);
    memset (release, 0, sizeof (*release));
}

void
mb_result_free (mb_result_t *result) {
    if (!result) {
        return;
    }
    for (int i = 0; i < result->num_releases; i++) {
        mb_free_release (&result->releases[i]);
    }
    free (result->releases);
    free (result);
}

#pragma mark - release parsing

static void
mb_add_candidate_disc (mb_release_t *release, int discnumber) {
    for (int i = 0; i < release->num_candidate_discs; i++) {
        if (release->candidate_discs[i] == discnumber) {
            return;
        }
    }
    int *discs = realloc (release->candidate_discs, sizeof (int) * (release->num_candidate_discs + 1));
    if (!discs) {
        return;
    }
    discs[release->num_candidate_discs++] = discnumber;
    release->candidate_discs = discs;
}

// parses a single release json object; numtracks is the number of selected
// files, used to find the matching disc, and may be 0 to accept everything
static int
mb_parse_release (json_t *json_release, int numtracks, const char *discid, mb_release_t *r) {
    memset (r, 0, sizeof (*r));

    mb_parse_artist_credit (json_release, &r->artist, &r->artist_ids);

    json_t *medias = json_object_get (json_release, "media");
    if (!json_is_array (medias)) {
        medias = json_array ();
    }

    int release_tracks = 0;
    size_t index;
    json_t *media;
    json_array_foreach (medias, index, media) {
        json_t *tracks = json_object_get (media, "tracks");
        if (json_is_array (tracks)) {
            release_tracks += (int)json_array_size (tracks);
        }
    }
    r->totaldiscs = (int)json_array_size (medias);

    json_array_foreach (medias, index, media) {
        json_t *tracks = json_object_get (media, "tracks");
        if (!json_is_array (tracks)) {
            continue;
        }
        int media_tracks = (int)json_array_size (tracks);

        int matches = numtracks <= 0 || release_tracks == numtracks || media_tracks == numtracks;
        if (!matches) {
            continue;
        }

        // when looking up by disc id, the disc must actually carry it
        if (discid && *discid) {
            json_t *discs = json_object_get (media, "discs");
            int found = 0;
            if (json_is_array (discs)) {
                size_t di;
                json_t *disc;
                json_array_foreach (discs, di, disc) {
                    const char *id = json_string_value (json_object_get (disc, "id"));
                    if (id && !strcmp (id, discid)) {
                        found = 1;
                        break;
                    }
                }
            }
            if (!found) {
                continue;
            }
        }

        if (media_tracks == numtracks && release_tracks != numtracks) {
            mb_add_candidate_disc (r, mb_json_int (media, "position", 1));
        }
        else if (release_tracks == numtracks) {
            r->complete = 1;
        }

        char *disc_format = mb_json_str (media, "format");
        char *disc_title = mb_json_str (media, "title");
        int discnumber = mb_json_int (media, "position", 1);

        // NOTE: this loop needs its own index variable -- reusing the index of
        // the enclosing media loop would skip every media after the first one
        json_t *track;
        size_t track_index;
        json_array_foreach (tracks, track_index, track) {
            mb_track_t t;
            memset (&t, 0, sizeof (t));
            mb_parse_artist_credit (track, &t.artist, &t.artist_ids);
            t.title = mb_json_str (track, "title");
            t.release_track_id = mb_json_str (track, "id");
            t.tracknumber = mb_json_int (track, "position", 0);
            t.discnumber = discnumber;
            t.totaldiscs = r->totaldiscs;
            t.totaltracks = media_tracks;
            t.disc_format = mb_strdup (disc_format);
            t.disc_title = mb_strdup (disc_title);

            json_t *recording = json_object_get (track, "recording");
            if (json_is_object (recording)) {
                t.recording_id = mb_json_str (recording, "id");
                json_t *isrcs = json_object_get (recording, "isrcs");
                if (json_is_array (isrcs)) {
                    size_t ii;
                    json_t *isrc;
                    json_array_foreach (isrcs, ii, isrc) {
                        t.isrcs = mb_append_value (t.isrcs, json_string_value (isrc));
                    }
                }
            }

            mb_track_t *grown = realloc (r->tracks, sizeof (mb_track_t) * (r->num_tracks + 1));
            if (!grown) {
                mb_free_track (&t);
                break;
            }
            r->tracks = grown;
            r->tracks[r->num_tracks++] = t;
        }

        free (disc_format);
        free (disc_title);
    }

    if (!r->num_tracks) {
        mb_free_release (r);
        return 0;
    }

    if (r->complete) {
        free (r->candidate_discs);
        r->candidate_discs = NULL;
        r->num_candidate_discs = 0;
    }

    r->title = mb_json_str (json_release, "title");
    r->release_id = mb_json_str (json_release, "id");
    r->asin = mb_json_str (json_release, "asin");
    r->barcode = mb_json_str (json_release, "barcode");
    r->country = mb_json_str (json_release, "country");
    r->date = mb_json_str (json_release, "date");
    r->status = mb_json_str (json_release, "status");
    r->disc_id = mb_strdup (discid);

    json_t *label_infos = json_object_get (json_release, "label-info");
    if (json_is_array (label_infos)) {
        size_t li;
        json_t *label_info;
        json_array_foreach (label_infos, li, label_info) {
            json_t *label = json_object_get (label_info, "label");
            if (json_is_object (label)) {
                const char *name = json_string_value (json_object_get (label, "name"));
                if (name && (!r->labels || !strstr (r->labels, name))) {
                    r->labels = mb_append_value (r->labels, name);
                }
            }
            if (!r->catalog) {
                r->catalog = mb_json_str (label_info, "catalog-number");
            }
        }
    }

    json_t *group = json_object_get (json_release, "release-group");
    if (json_is_object (group)) {
        r->original_date = mb_json_str (group, "first-release-date");
        r->type = mb_json_str (group, "primary-type");
        r->release_group_id = mb_json_str (group, "id");
    }

    // a release is a various artists release when any of its tracks has an
    // artist credit different from the album artist credit
    for (int i = 0; i < r->num_tracks; i++) {
        if (r->tracks[i].artist && r->artist && strcmp (r->tracks[i].artist, r->artist)) {
            r->is_various = 1;
            break;
        }
    }

    // collect the media formats for the picker
    for (int i = 0; i < r->num_tracks; i++) {
        const char *format = r->tracks[i].disc_format;
        if (!format || !*format) {
            continue;
        }
        if (!r->media || !strstr (r->media, format)) {
            r->media = mb_strdup_append (r->media, r->media ? ", " : "", format);
        }
    }

    return 1;
}

static int
mb_compare_releases (const void *a, const void *b) {
    const mb_release_t *ra = a;
    const mb_release_t *rb = b;
    // complete releases first, then the oldest ones, like foo_musicbrainz
    if (ra->complete != rb->complete) {
        return rb->complete - ra->complete;
    }
    const char *da = ra->date ? ra->date : "";
    const char *db = rb->date ? rb->date : "";
    int cmp = strcmp (da, db);
    if (cmp) {
        return cmp;
    }
    const char *ta = ra->title ? ra->title : "";
    const char *tb = rb->title ? rb->title : "";
    return strcasecmp (ta, tb);
}

static mb_result_t *
mb_result_new (void) {
    mb_result_t *result = calloc (1, sizeof (mb_result_t));
    if (!result) {
        deadbeef->log ("musicbrainz: out of memory\n");
    }
    return result;
}

// appends a parsed release to the result
static void
mb_result_add (mb_result_t *result, mb_release_t *release) {
    if (!result || result->num_releases >= MB_MAX_CANDIDATES) {
        mb_free_release (release);
        return;
    }
    mb_release_t *grown = realloc (result->releases, sizeof (mb_release_t) * (result->num_releases + 1));
    if (!grown) {
        mb_free_release (release);
        return;
    }
    result->releases = grown;
    result->releases[result->num_releases++] = *release;
}

#pragma mark - queries

const char *
mb_get_server (void) {
    // NOTE: only one query runs at a time, and the gui reads the server outside
    // of the worker thread, so the static buffer below is never used concurrently
    static char server[256];
    deadbeef->conf_get_str ("musicbrainz.server", MB_DEFAULT_SERVER, server, sizeof (server));
    size_t len = strlen (server);
    while (len > 0 && server[len-1] == '/') {
        server[--len] = 0;
    }
    if (!len) {
        strcpy (server, MB_DEFAULT_SERVER);
    }
    return server;
}

static int
mb_max_candidates (void) {
    int max = deadbeef->conf_get_int ("musicbrainz.max_releases", MB_MAX_CANDIDATES);
    if (max < 1) {
        max = 1;
    }
    else if (max > MB_MAX_CANDIDATES) {
        max = MB_MAX_CANDIDATES;
    }
    return max;
}

// MusicBrainz answers {"error" : "..."} for a bad query, and also with a 200
// status when the server is overloaded -- jansson parses that just fine, so it
// has to be detected separately from an empty result
static int
mb_check_server_error (json_t *root, mb_result_t *result) {
    json_t *error = json_object_get (root, "error");
    if (error && json_is_string (error)) {
        deadbeef->log ("musicbrainz: server error: %s\n", json_string_value (error));
        if (result) {
            result->failed = 1;
        }
        return 0;
    }
    return 1;
}

// performs a lookup by entity/id, and adds the resulting release to the result
static void
mb_lookup_release_ids (mb_result_t *result, json_t *ids, int numtracks, const volatile int *abort) {
    if (!result) {
        return;
    }
    size_t count = json_array_size (ids);
    int max = mb_max_candidates ();
    for (size_t i = 0; i < count && result->num_releases < max; i++) {
        if (abort && *abort) {
            result->aborted = 1;
            return;
        }
        const char *id = json_string_value (json_array_get (ids, i));
        if (!id) {
            continue;
        }

        char url[512];
        snprintf (url, sizeof (url), "%s/ws/2/release/%s?fmt=json&inc=%s", mb_get_server (), id, MB_LOOKUP_INC);

        int req_failed = 0;
        char *data = mb_fetch (url, abort, &req_failed);
        if (!data) {
            if (req_failed) {
                result->failed = 1;
            }
            continue;
        }
        json_error_t error;
        json_t *json_release = json_loads (data, JSON_DISABLE_EOF_CHECK, &error);
        free (data);
        if (!json_is_object (json_release)) {
            continue;
        }
        if (!mb_check_server_error (json_release, result)) {
            json_decref (json_release);
            continue;
        }

        mb_release_t release;
        if (mb_parse_release (json_release, numtracks, NULL, &release)) {
            mb_result_add (result, &release);
        }
        json_decref (json_release);
    }
}

// collects the ids of the releases which have as many tracks as the selection
static json_t *
mb_filtered_release_ids (json_t *releases, int numtracks) {
    json_t *ids = json_array ();
    size_t index;
    json_t *release;
    json_array_foreach (releases, index, release) {
        int total = 0;
        int match = 0;
        json_t *medias = json_object_get (release, "media");
        if (json_is_array (medias)) {
            json_t *media;
            size_t mi;
            json_array_foreach (medias, mi, media) {
                int tracks = mb_json_int (media, "track-count", 0);
                total += tracks;
                if (tracks == numtracks) {
                    match = 1;
                }
            }
        }
        if (mb_json_int (release, "track-count", 0) == numtracks || total == numtracks || match) {
            const char *id = json_string_value (json_object_get (release, "id"));
            if (id) {
                json_array_append_new (ids, json_string (id));
            }
        }
    }
    return ids;
}

mb_result_t *
mb_lookup_discid (DB_playItem_t **tracks, int numtracks, const volatile int *abort) {
    mb_result_t *result = mb_result_new ();
    if (!result) {
        return NULL;
    }
    char *discid = mb_make_discid (tracks, numtracks, NULL);
    if (!discid) {
        return result;
    }

    char url[512];
    snprintf (url, sizeof (url), "%s/ws/2/discid/%s?fmt=json&cdstubs=no&inc=%s", mb_get_server (), discid, MB_LOOKUP_INC);

    int req_failed = 0;
    char *data = mb_fetch (url, abort, &req_failed);
    if (!data) {
        result->failed = req_failed;
        free (discid);
        return result;
    }
    json_error_t error;
    json_t *json_root = json_loads (data, JSON_DISABLE_EOF_CHECK, &error);
    free (data);
    if (!json_is_object (json_root) || !mb_check_server_error (json_root, result)) {
        json_decref (json_root);
        free (discid);
        return result;
    }

    char *json_discid = mb_json_str (json_root, "id");
    json_t *releases = json_object_get (json_root, "releases");
    if (json_is_array (releases)) {
        size_t index;
        json_t *json_release;
        json_array_foreach (releases, index, json_release) {
            mb_release_t release;
            if (mb_parse_release (json_release, numtracks, json_discid, &release)) {
                mb_result_add (result, &release);
            }
        }
    }

    json_decref (json_root);
    free (json_discid);
    free (discid);

    if (result->num_releases > 1) {
        qsort (result->releases, result->num_releases, sizeof (mb_release_t), mb_compare_releases);
    }
    return result;
}

mb_result_t *
mb_search (const char *artist, const char *album, int numtracks, const volatile int *abort) {
    mb_result_t *result = mb_result_new ();
    if (!result) {
        return NULL;
    }

    // the query is built dynamically, because artist and album names come from
    // the tags and can be arbitrarily long
    int qlen = snprintf (NULL, 0, album && *album ? "artist:\"%s\" AND release:\"%s\"" : "artist:\"%s\"",
            artist ? artist : "", album ? album : "");
    char *query = malloc (qlen + 1);
    if (!query) {
        return result;
    }
    if (album && *album) {
        snprintf (query, qlen + 1, "artist:\"%s\" AND release:\"%s\"", artist ? artist : "", album);
    }
    else {
        snprintf (query, qlen + 1, "artist:\"%s\"", artist ? artist : "");
    }
    char *enc_query = mb_url_encode (query);
    free (query);
    if (!enc_query) {
        return result;
    }

    char *url = malloc (strlen (mb_get_server ()) + strlen (enc_query) + 64);
    if (!url) {
        free (enc_query);
        return result;
    }
    sprintf (url, "%s/ws/2/release?fmt=json&limit=100&query=%s", mb_get_server (), enc_query);
    free (enc_query);

    int req_failed = 0;
    char *data = mb_fetch (url, abort, &req_failed);
    free (url);
    if (!data) {
        result->failed = req_failed;
        return result;
    }
    json_error_t error;
    json_t *json_root = json_loads (data, JSON_DISABLE_EOF_CHECK, &error);
    free (data);
    if (!json_is_object (json_root) || !mb_check_server_error (json_root, result)) {
        json_decref (json_root);
        return result;
    }

    json_t *ids = mb_filtered_release_ids (json_object_get (json_root, "releases"), numtracks);
    json_decref (json_root);

    mb_lookup_release_ids (result, ids, numtracks, abort);
    json_decref (ids);

    if (result->num_releases > 1) {
        qsort (result->releases, result->num_releases, sizeof (mb_release_t), mb_compare_releases);
    }
    return result;
}

mb_result_t *
mb_lookup_mbid (const char *entity, const char *mbid, int numtracks, const volatile int *abort) {
    mb_result_t *result = mb_result_new ();
    if (!result) {
        return NULL;
    }
    if (!mb_is_uuid (mbid)) {
        return result;
    }

    char url[512];
    if (!strcmp (entity, "release-group")) {
        snprintf (url, sizeof (url), "%s/ws/2/release-group/%s?fmt=json&inc=releases+media", mb_get_server (), mbid);
    }
    else {
        snprintf (url, sizeof (url), "%s/ws/2/release/%s?fmt=json&inc=%s", mb_get_server (), mbid, MB_LOOKUP_INC);
    }

    int req_failed = 0;
    char *data = mb_fetch (url, abort, &req_failed);
    if (!data) {
        result->failed = req_failed;
        return result;
    }
    json_error_t error;
    json_t *json_root = json_loads (data, JSON_DISABLE_EOF_CHECK, &error);
    free (data);
    if (!json_is_object (json_root) || !mb_check_server_error (json_root, result)) {
        json_decref (json_root);
        return result;
    }

    if (!strcmp (entity, "release-group")) {
        // a release group can hold dozens of releases, and every one of them
        // costs a request of its own at one request per second, so only the
        // releases with a matching number of tracks are looked up, like in
        // foo_musicbrainz
        json_t *ids = mb_filtered_release_ids (json_object_get (json_root, "releases"), numtracks);
        json_decref (json_root);
        mb_lookup_release_ids (result, ids, numtracks, abort);
        json_decref (ids);
    }
    else {
        mb_release_t release;
        if (mb_parse_release (json_root, numtracks, NULL, &release)) {
            mb_result_add (result, &release);
        }
        json_decref (json_root);
    }
    return result;
}

#pragma mark - toc / disc id

static int64_t
mb_parse_int64 (const char *s, int64_t def) {
    if (!s || !*s) {
        return def;
    }
    char *end = NULL;
    int64_t val = strtoll (s, &end, 10);
    return end == s ? def : val;
}

static int
mb_track_samples (DB_playItem_t *it, int64_t *samples) {
    deadbeef->pl_lock ();
    int64_t start = mb_parse_int64 (deadbeef->pl_find_meta (it, ":STARTSAMPLE"), -1);
    int64_t end = mb_parse_int64 (deadbeef->pl_find_meta (it, ":ENDSAMPLE"), -1);
    int samplerate = deadbeef->pl_find_meta_int (it, ":SAMPLERATE", 0);
    deadbeef->pl_unlock ();

    if (start >= 0 && end > start) {
        *samples = end - start;
        return 1;
    }

    float duration = deadbeef->pl_get_item_duration (it);
    if (duration <= 0 || samplerate <= 0) {
        return 0;
    }
    *samples = (int64_t)(duration * samplerate + 0.5);
    return 1;
}

// returns the number of samples in a CD sector for the tracks, or 0 when the
// selection doesn't look like a CD
static int
mb_samples_per_sector (DB_playItem_t **tracks, int numtracks) {
    if (numtracks < 1 || numtracks > MB_MAX_TRACKS) {
        return 0;
    }
    int samplerate = 0;
    for (int i = 0; i < numtracks; i++) {
        deadbeef->pl_lock ();
        int sr = deadbeef->pl_find_meta_int (tracks[i], ":SAMPLERATE", 0);
        deadbeef->pl_unlock ();
        if (sr != 44100 && sr != 48000) {
            return 0;
        }
        if (!samplerate) {
            samplerate = sr;
        }
        else if (sr != samplerate) {
            return 0;
        }

        int64_t samples;
        if (!mb_track_samples (tracks[i], &samples)) {
            return 0;
        }
        int per_sector = samplerate == 48000 ? 640 : 588;
        int64_t rest = samples % per_sector;
        // ripped files are often a few samples away from the exact sector size
        if (rest != 0 && rest < per_sector - 1) {
            if (rest > 1 && per_sector - rest > 1) {
                return 0;
            }
        }
    }
    return samplerate == 48000 ? 640 : 588;
}

// 1 if the decoder of the track writes lossless audio; unknown decoders are
// assumed to be lossless, so that a new plugin does not block the feature
static int
mb_is_lossless (DB_playItem_t *it) {
    // the values of the :DECODER metadata are the ids of the decoder plugins
    static const char *const lossy_decoders[] = {
        // lossy compression
        "stdmpg", "stdogg", "ffmpeg", "ffap", "aac", "opus", "musepack", "wma", "dts", "wv",
        // synthesized / emulated audio, which never comes from a cd
        "adplug", "stdgme", "in_sc68", "sidplay2", "psf", "stddumb", "wmidi", "vtx", NULL
    };

    deadbeef->pl_lock ();
    const char *dec = deadbeef->pl_find_meta (it, ":DECODER");
    char decoder[100] = "";
    if (dec) {
        snprintf (decoder, sizeof (decoder), "%s", dec);
    }
    deadbeef->pl_unlock ();

    for (int i = 0; lossy_decoders[i]; i++) {
        if (!strcmp (lossy_decoders[i], decoder)) {
            return 0;
        }
    }
    return 1;
}

int
mb_check_toc (DB_playItem_t **tracks, int numtracks) {
    return mb_samples_per_sector (tracks, numtracks) != 0;
}

// like mb_check_toc, but with the extra conditions of a TOC submission: only
// 44100 Hz lossless files are accepted by MusicBrainz
int
mb_check_toc_submission (DB_playItem_t **tracks, int numtracks) {
    if (numtracks < 1 || numtracks > MB_MAX_TRACKS) {
        return 0;
    }
    for (int i = 0; i < numtracks; i++) {
        deadbeef->pl_lock ();
        int sr = deadbeef->pl_find_meta_int (tracks[i], ":SAMPLERATE", 0);
        deadbeef->pl_unlock ();
        if (sr != 44100 || !mb_is_lossless (tracks[i])) {
            return 0;
        }
    }
    return mb_samples_per_sector (tracks, numtracks) != 0;
}

static int
mb_pregap_sectors (const char *pregap_msf) {
    int pregap = deadbeef->conf_get_int ("musicbrainz.pregap_sectors", MB_DEFAULT_PREGAP_SECTORS);
    if (pregap < 0) {
        pregap = 0;
    }
    if (pregap_msf && strlen (pregap_msf) == 8
            && isdigit (pregap_msf[0]) && isdigit (pregap_msf[1]) && pregap_msf[2] == ':'
            && isdigit (pregap_msf[3]) && isdigit (pregap_msf[4]) && pregap_msf[5] == ':'
            && isdigit (pregap_msf[6]) && isdigit (pregap_msf[7])) {
        int minutes = (pregap_msf[0] - '0') * 10 + (pregap_msf[1] - '0');
        int seconds = (pregap_msf[3] - '0') * 10 + (pregap_msf[4] - '0');
        int frames = (pregap_msf[6] - '0') * 10 + (pregap_msf[7] - '0');
        pregap = (minutes * 60 + seconds) * MB_CD_FRAMES_PER_SEC + frames;
    }
    return pregap;
}

// fills the 100 sector offsets of a CD, in the order the disc id is hashed
// from: lead-out, then the offsets of the tracks, zero padded
static int
mb_make_sector_offsets (DB_playItem_t **tracks, int numtracks, const char *pregap_msf, uint32_t offsets[100]) {
    int per_sector = mb_samples_per_sector (tracks, numtracks);
    if (!per_sector) {
        return 0;
    }
    memset (offsets, 0, sizeof (uint32_t) * 100);

    uint32_t pos = (uint32_t)mb_pregap_sectors (pregap_msf);
    for (int i = 0; i < numtracks; i++) {
        offsets[i + 1] = pos;
        int64_t samples;
        if (!mb_track_samples (tracks[i], &samples)) {
            return 0;
        }
        pos += (uint32_t)((samples + per_sector / 2) / per_sector);
    }
    offsets[0] = pos; // lead-out
    return 1;
}

char *
mb_make_discid (DB_playItem_t **tracks, int numtracks, const char *pregap_msf) {
    uint32_t offsets[100];
    if (!mb_make_sector_offsets (tracks, numtracks, pregap_msf, offsets)) {
        return NULL;
    }

    mb_sha1_t sha;
    sha1_init (&sha);
    char buf[16];

    snprintf (buf, sizeof (buf), "%02X", 1);
    sha1_update (&sha, buf, 2);
    snprintf (buf, sizeof (buf), "%02X", numtracks);
    sha1_update (&sha, buf, 2);
    for (int i = 0; i < 100; i++) {
        snprintf (buf, sizeof (buf), "%08X", offsets[i]);
        sha1_update (&sha, buf, 8);
    }

    uint8_t digest[20];
    sha1_final (&sha, digest);

    char *discid = malloc (32);
    if (!discid) {
        return NULL;
    }
    mb_discid_base64 (digest, sizeof (digest), discid);
    return discid;
}

char *
mb_make_toc_url (DB_playItem_t **tracks, int numtracks) {
    uint32_t offsets[100];
    if (!mb_make_sector_offsets (tracks, numtracks, NULL, offsets)) {
        return NULL;
    }

    // the musicbrainz toc submission form expects the cdindex, the number of
    // tracks, then the lead out offset, then the offset of every track,
    // separated with '+', and optionally the disc id it calculates from them
    char *discid = mb_make_discid (tracks, numtracks, NULL);
    const char *server = mb_get_server ();
    int size = (int)strlen (server) + (numtracks + 2) * 11 + (discid ? (int)strlen (discid) : 0) + 64;
    char *url = malloc (size);
    if (!url) {
        free (discid);
        return NULL;
    }

    int len = snprintf (url, size, "%s/cdtoc/attach?toc=1+%d+%u", server, numtracks, offsets[0]);
    for (int i = 1; i <= numtracks && len < size - 1; i++) {
        len += snprintf (url + len, size - len, "+%u", offsets[i]);
    }
    if (discid && len < size - 1) {
        len += snprintf (url + len, size - len, "&tracks=%d&id=%s", numtracks, discid);
    }
    free (discid);
    return url;
}

#pragma mark - applying tags

static void
mb_set_meta (DB_playItem_t *it, const char *key, const char *value) {
    if (value && *value) {
        deadbeef->pl_replace_meta (it, key, value);
    }
    else {
        deadbeef->pl_delete_meta (it, key);
    }
}

// writes every value of a ';' separated list as a separate multivalue tag
static void
mb_set_meta_list (DB_playItem_t *it, const char *key, const char *values) {
    deadbeef->pl_delete_meta (it, key);
    if (!values || !*values) {
        return;
    }
    const char *p = values;
    while (p) {
        const char *end = strchr (p, ';');
        size_t len = end ? (size_t)(end - p) : strlen (p);
        if (len > 0) {
            char *val = malloc (len + 1);
            if (val) {
                memcpy (val, p, len);
                val[len] = 0;
                deadbeef->pl_append_meta (it, key, val);
                free (val);
            }
        }
        p = end ? end + 1 : NULL;
    }
}

static void
mb_cut_to_year (char *date) {
    if (!date) {
        return;
    }
    char *p = date;
    while (*p && *p != '-') {
        p++;
    }
    *p = 0;
}

// a release is a various artists release when any of its tracks has an artist
// which is different from the album artist; this is already decided when the
// release is parsed, but the values can be corrected in the dialog afterwards,
// so the tracks which are really applied are checked here as well
static int
mb_release_is_various (mb_release_t *release, const mb_track_t **tracks, int numtracks) {
    if (release->is_various) {
        return 1;
    }
    const char *album_artist = release->artist ? release->artist : "";
    for (int i = 0; i < numtracks; i++) {
        const mb_track_t *t = tracks[i];
        if (t->artist && t->artist[0] && strcmp (t->artist, album_artist)) {
            return 1;
        }
    }
    return 0;
}

int
mb_apply_release (mb_release_t *release, int discnumber, DB_playItem_t **tracks, int numtracks) {
    if (!release || !numtracks) {
        return 0;
    }

    // collect the tracks to apply, in disc and track number order
    const mb_track_t **ordered = calloc (release->num_tracks, sizeof (mb_track_t *));
    if (!ordered) {
        return 0;
    }
    int on_disc = 0;
    for (int i = 0; i < release->num_tracks; i++) {
        if (discnumber <= 0 || release->tracks[i].discnumber == discnumber) {
            ordered[on_disc++] = &release->tracks[i];
        }
    }
    for (int i = 1; i < on_disc; i++) {
        const mb_track_t *tmp = ordered[i];
        int j = i - 1;
        while (j >= 0 && (ordered[j]->discnumber > tmp->discnumber
                || (ordered[j]->discnumber == tmp->discnumber && ordered[j]->tracknumber > tmp->tracknumber))) {
            ordered[j+1] = ordered[j];
            j--;
        }
        ordered[j+1] = tmp;
    }
    if (on_disc != numtracks) {
        // the release doesn't fit the selection
        free (ordered);
        return 0;
    }

    const int write_mbids = deadbeef->conf_get_int ("musicbrainz.write_mbids", 1);
    const int write_track_mbids = deadbeef->conf_get_int ("musicbrainz.write_track_mbids", 1);
    const int write_album_artist = deadbeef->conf_get_int ("musicbrainz.write_album_artist", 0);
    const int write_label_info = deadbeef->conf_get_int ("musicbrainz.write_label_info", 1);
    const int write_album_type = deadbeef->conf_get_int ("musicbrainz.write_album_type", 1);
    const int write_album_status = deadbeef->conf_get_int ("musicbrainz.write_album_status", 1);
    const int write_country = deadbeef->conf_get_int ("musicbrainz.write_country", 1);
    const int write_media = deadbeef->conf_get_int ("musicbrainz.write_media", 0);
    const int write_isrc = deadbeef->conf_get_int ("musicbrainz.write_isrc", 1);
    const int write_asin = deadbeef->conf_get_int ("musicbrainz.write_asin", 0);
    const int short_date = deadbeef->conf_get_int ("musicbrainz.short_date", 1);
    const int use_original_date = deadbeef->conf_get_int ("musicbrainz.use_original_date", 0);

    const int album_artist = write_album_artist || mb_release_is_various (release, ordered, on_disc);
    const char *date = use_original_date && release->original_date ? release->original_date : release->date;

    char discnum_str[16];
    char tracknum_str[16];
    char totaltracks_str[16];
    char totaldiscs_str[16];
    char year[32];

    int updated = 0;
    for (int i = 0; i < numtracks; i++) {
        DB_playItem_t *it = tracks[i];
        if (!it) {
            continue;
        }
        const mb_track_t *t = ordered[i];
        updated++;

        if (album_artist) {
            mb_set_meta (it, "album artist", release->artist);
        }
        mb_set_meta (it, "album", release->title);
        mb_set_meta (it, "artist", t->artist);
        mb_set_meta (it, "title", t->title);

        snprintf (tracknum_str, sizeof (tracknum_str), "%d", t->tracknumber);
        mb_set_meta (it, "track", tracknum_str);
        snprintf (totaltracks_str, sizeof (totaltracks_str), "%d", t->totaltracks);
        mb_set_meta (it, "numtracks", totaltracks_str);

        if (t->totaldiscs > 1) {
            snprintf (discnum_str, sizeof (discnum_str), "%d", t->discnumber);
            mb_set_meta (it, "disc", discnum_str);
            snprintf (totaldiscs_str, sizeof (totaldiscs_str), "%d", t->totaldiscs);
            mb_set_meta (it, "numdiscs", totaldiscs_str);
            mb_set_meta (it, "discsubtitle", t->disc_title);
        }
        else {
            // a single disc release has nothing to clear these to
            deadbeef->pl_delete_meta (it, "disc");
            deadbeef->pl_delete_meta (it, "numdiscs");
            deadbeef->pl_delete_meta (it, "discsubtitle");
        }

        // a release without a date must not leave the year of the previous tag
        year[0] = 0;
        if (date && *date) {
            snprintf (year, sizeof (year), "%s", date);
            if (short_date) {
                mb_cut_to_year (year);
            }
        }
        mb_set_meta (it, "year", year);

        // the original date is only interesting when it differs from the date,
        // like in foo_musicbrainz; a stale value from a previous tag must go
        char orig[32] = "";
        if (!use_original_date && release->original_date && release->date
                && strcmp (release->original_date, release->date)) {
            snprintf (orig, sizeof (orig), "%s", release->original_date);
            if (short_date) {
                mb_cut_to_year (orig);
            }
        }
        mb_set_meta (it, "ORIGINAL_RELEASE_TIME", orig);

        if (write_album_type) {
            mb_set_meta (it, "MUSICBRAINZ_ALBUMTYPE", release->type);
        }
        if (write_album_status) {
            mb_set_meta (it, "MUSICBRAINZ_ALBUMSTATUS", release->status);
        }
        if (write_country) {
            mb_set_meta (it, "RELEASECOUNTRY", release->country);
        }
        if (write_media) {
            // musicbrainz calls this field MEDIA in vorbis comments, and that
            // is what foo_musicbrainz writes too, so the name is kept even
            // though deadbeef maps the id3v2 TMED frame from "MEDIA TYPE"
            mb_set_meta (it, "MEDIA", t->disc_format);
        }
        if (write_label_info) {
            mb_set_meta_list (it, "LABEL", release->labels);
            mb_set_meta (it, "CATALOGNUMBER", release->catalog);
            mb_set_meta (it, "BARCODE", release->barcode);
        }
        if (write_asin) {
            mb_set_meta (it, "ASIN", release->asin);
        }
        if (write_isrc) {
            mb_set_meta_list (it, "ISRC", t->isrcs);
        }

        if (write_mbids) {
            mb_set_meta (it, "MUSICBRAINZ_ALBUMID", release->release_id);
            mb_set_meta (it, "MUSICBRAINZ_RELEASEGROUPID", release->release_group_id);
            mb_set_meta_list (it, "MUSICBRAINZ_ARTISTID", t->artist_ids);
            if (album_artist) {
                mb_set_meta_list (it, "MUSICBRAINZ_ALBUMARTISTID", release->artist_ids);
            }
            if (release->disc_id) {
                mb_set_meta (it, "MUSICBRAINZ_DISCID", release->disc_id);
            }
            if (write_track_mbids) {
                mb_set_meta (it, "MUSICBRAINZ_TRACKID", t->recording_id);
                // the key deadbeef itself uses for the recording id; junklib
                // writes it as an id3v2 UFID frame
                mb_set_meta (it, "musicbrainz_trackid", t->recording_id);
                mb_set_meta (it, "MUSICBRAINZ_RELEASETRACKID", t->release_track_id);
            }
        }
    }

    free (ordered);
    return updated;
}

#pragma mark - playlist helpers

static char *
mb_get_meta (DB_playItem_t *it, const char *key) {
    deadbeef->pl_lock ();
    const char *val = deadbeef->pl_find_meta (it, key);
    char *out = mb_strdup (val);
    deadbeef->pl_unlock ();
    return out;
}

char *
mb_find_common_mbid (DB_playItem_t **tracks, int numtracks, const char *key) {
    char *mbid = NULL;
    for (int i = 0; i < numtracks; i++) {
        char *val = mb_get_meta (tracks[i], key);
        if (!mbid) {
            if (!mb_is_uuid (val)) {
                free (val);
                return NULL;
            }
            mbid = val;
        }
        else {
            int same = val && !strcasecmp (val, mbid);
            free (val);
            if (!same) {
                free (mbid);
                return NULL;
            }
        }
    }
    return mbid;
}

void
mb_find_common_artist_album (DB_playItem_t **tracks, int numtracks, char **artist, char **album) {
    *artist = NULL;
    *album = NULL;
    if (!numtracks) {
        return;
    }

    // a value which any of the tracks does not have, or which they don't agree
    // on, is left empty for the user to fill in, without throwing away the
    // other value
    int have_artist = 1;
    int have_album = 1;

    for (int i = 0; i < numtracks; i++) {
        char *cur_artist = mb_get_meta (tracks[i], "album artist");
        if (!cur_artist || !*cur_artist) {
            free (cur_artist);
            cur_artist = mb_get_meta (tracks[i], "artist");
        }
        char *cur_album = mb_get_meta (tracks[i], "album");
        if ((!cur_album || !*cur_album) && numtracks == 1) {
            free (cur_album);
            cur_album = mb_get_meta (tracks[i], "title");
        }

        if (have_artist) {
            if (!cur_artist || !*cur_artist || (*artist && strcasecmp (cur_artist, *artist))) {
                have_artist = 0;
                free (*artist);
                *artist = NULL;
            }
            else if (!*artist) {
                *artist = cur_artist;
                cur_artist = NULL;
            }
        }

        if (have_album) {
            if (!cur_album || !*cur_album || (*album && strcasecmp (cur_album, *album))) {
                have_album = 0;
                free (*album);
                *album = NULL;
            }
            else if (!*album) {
                *album = cur_album;
                cur_album = NULL;
            }
        }

        free (cur_artist);
        free (cur_album);
    }
}

static const char *album_descriptors[] = {
    "10th Anniversary",
    "20th Anniversary",
    "25th Anniversary",
    "30th Anniversary",
    "7\" Vinyl",
    "12\" Vinyl",
    "Bonus Disc",
    "Bonus Track",
    "CD1",
    "CD2",
    "CD3",
    "CD4",
    "CD#1",
    "CD#2",
    "CD#3",
    "CD#4",
    "CD #1",
    "CD #2",
    "CD #3",
    "CD #4",
    "Deluxe Edition",
    "Digipack",
    "Disc One",
    "Disc Two",
    "Disc Three",
    "Disc Four",
    "Disc 1",
    "Disc 2",
    "Disc 3",
    "Disc 4",
    "Expanded Edition",
    "Japan Edition",
    "Japanese Edition",
    "Limited Edition",
    "Ltd. Edition",
    "Promo",
    "Reissue",
    "Remastered Edition",
    "Remastered",
    "SACD",
    "SHM-CD",
    "Special Edition",
    "Target Exclusive",
    "UK Edition",
    "Vinyl Edition",
    "Vinyl Reissue",
    NULL
};

// finds the last case insensitive occurrence of needle in haystack
static const char *
mb_strrcasestr (const char *haystack, const char *needle) {
    size_t needlelen = strlen (needle);
    if (!needlelen || strlen (haystack) < needlelen) {
        return NULL;
    }
    const char *found = NULL;
    for (const char *p = haystack; *p; p++) {
        if (!strncasecmp (p, needle, needlelen)) {
            found = p;
        }
    }
    return found;
}

char *
mb_strip_album_edition (const char *album) {
    if (!album) {
        return NULL;
    }
    char *out = mb_strdup (album);
    if (!out) {
        return NULL;
    }
    for (int i = 0; album_descriptors[i]; i++) {
        const char *pos = mb_strrcasestr (out, album_descriptors[i]);
        if (!pos || pos == out) {
            // an album which is nothing but a descriptor keeps its name
            continue;
        }
        if (pos[-1] == '[' || pos[-1] == '{' || pos[-1] == '(') {
            pos--;
        }
        if (pos > out && pos[-1] == ' ') {
            pos--;
        }
        if (pos > out) {
            out[pos - out] = 0;
        }
    }
    return out;
}
