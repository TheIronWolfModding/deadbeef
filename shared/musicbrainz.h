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

#ifndef ____musicbrainz__
#define ____musicbrainz__

#include <deadbeef/deadbeef.h>

// MusicBrainz release lookup and tagging engine.
//
// All the query functions are blocking and have to be called from a worker
// thread, never from the UI thread. The abort flag is checked before every
// http request, so the caller can cancel a running query at any time.
//
// None of the functions here write anything to disk -- mb_apply_release only
// updates the metadata of the playlist items, and it's up to the caller to
// save the tags with the track decoder, and to notify the UI.

/// A single track of a MusicBrainz release.
typedef struct mb_track_s {
    char *title;
    char *artist; // artist credit, as joined by MusicBrainz
    char *artist_ids; // ';' separated MusicBrainz artist IDs
    char *recording_id;
    char *release_track_id;
    char *isrcs; // ';' separated
    char *disc_format;
    char *disc_title;
    int tracknumber;
    int discnumber;
    int totaltracks; // tracks on the disc
    int totaldiscs;
} mb_track_t;

/// A single MusicBrainz release, with the tracks of all its discs.
typedef struct mb_release_s {
    char *title;
    char *artist; // album artist credit
    char *artist_ids;
    char *date;
    char *original_date;
    char *status;
    char *type; // release group primary type
    char *country;
    char *labels; // ';' separated
    char *catalog;
    char *barcode;
    char *asin;
    char *release_id;
    char *release_group_id;
    char *disc_id; // only set when the release was found through a disc ID
    char *media; // comma separated media formats, for display
    int totaldiscs;
    int is_various;
    int complete; // the whole release matches the number of selected tracks

    mb_track_t *tracks;
    int num_tracks;

    // Discs which have exactly as many tracks as the selection, and therefore
    // can be applied to it. Only used when complete is 0.
    int *candidate_discs;
    int num_candidate_discs;
} mb_release_t;

typedef struct mb_result_s {
    mb_release_t *releases;
    int num_releases;
    int aborted;
    int failed; // the server returned an error, so this is not a real empty result
} mb_result_t;

/// Returns 1 if the tracks look like a CD, i.e. they all have the same sample
/// rate of 44100 or 48000 Hz, and every track length is a whole number of CD
/// sectors. Max 99 tracks, like an audio CD.
int
mb_check_toc (DB_playItem_t **tracks, int numtracks);

/// Calculates the MusicBrainz disc ID from the track lengths, or NULL if the
/// tracks don't look like a CD. The string must be free()d by the caller.
char *
mb_make_discid (DB_playItem_t **tracks, int numtracks, const char *pregap_msf);

/// Returns the URL of the MusicBrainz TOC submission page, or NULL.
/// The string must be free()d by the caller.
char *
mb_make_toc_url (DB_playItem_t **tracks, int numtracks);

/// Looks up all releases with a matching disc ID.
mb_result_t *
mb_lookup_discid (DB_playItem_t **tracks, int numtracks, const volatile int *abort);

/// Searches releases by artist and album name.
/// If album is empty, searches for the releases of the artist alone.
mb_result_t *
mb_search (const char *artist, const char *album, int numtracks, const volatile int *abort);

/// Looks up a release, or all releases of a release group, by MusicBrainz ID.
/// entity is either "release" or "release-group".
mb_result_t *
mb_lookup_mbid (const char *entity, const char *mbid, int numtracks, const volatile int *abort);

void
mb_result_free (mb_result_t *result);

/// Writes the tags of the release into the metadata of the items, in the order
/// they were selected. discnumber selects the disc of a multi-disc release to
/// apply, and must have exactly as many tracks as the selection; pass 0 to
/// apply the whole release, which then must have exactly as many tracks as the
/// selection. Returns the number of updated items, or 0 if the release doesn't
/// fit the selection.
/// Only updates the in-memory metadata, does not write the files.
int
mb_apply_release (mb_release_t *release, int discnumber, DB_playItem_t **tracks, int numtracks);

/// Reads the "musicbrainz_albumid" / "musicbrainz_releasegroupid" tag of all
/// the tracks, and returns it if every track has the same valid value.
/// Returns NULL otherwise. The string must be free()d by the caller.
char *
mb_find_common_mbid (DB_playItem_t **tracks, int numtracks, const char *key);

/// Finds the artist/album pair common to all the tracks, for the search
/// dialog. Falls back to the title of a single track. Values must be free()d.
void
mb_find_common_artist_album (DB_playItem_t **tracks, int numtracks, char **artist, char **album);

/// Strips edition descriptors ("Remastered", "Deluxe Edition", ...) from the
/// end of the album name, to improve the search hit rate.
/// The string must be free()d by the caller.
char *
mb_strip_album_edition (const char *album);

/// 1 if the string is a MusicBrainz ID.
int
mb_is_uuid (const char *id);

/// The configured MusicBrainz server, without the trailing slash.
/// Do not free.
const char *
mb_get_server (void);

#endif // ____musicbrainz__
