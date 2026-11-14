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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deadbeef/deadbeef.h>
#include "playlist.h"
extern "C" {
    #include "../shared/musicbrainz.h"
}
#include <gtest/gtest.h>

extern DB_functions_t *deadbeef;

// the number of samples in a cd sector, at 44100 hz
static const int SAMPLES_PER_SECTOR = 588;

static DB_playItem_t *
_make_track (playlist_t *plt, int sectors, int samplerate) {
    DB_playItem_t *it = deadbeef->pl_item_alloc ();
    deadbeef->pl_replace_meta (it, ":URI", "/nonexistent/deadbeef/mbtest.flac");

    char s[32];
    snprintf (s, sizeof (s), "%d", samplerate);
    deadbeef->pl_replace_meta (it, ":SAMPLERATE", s);

    int samples_per_sector = samplerate == 48000 ? 640 : SAMPLES_PER_SECTOR;
    deadbeef->plt_set_item_duration ((ddb_playlist_t *)plt, it, (float)sectors * samples_per_sector / samplerate);

    deadbeef->plt_insert_item ((ddb_playlist_t *)plt, NULL, it);
    return it;
}

static void
_free_tracks (DB_playItem_t **tracks, int numtracks) {
    for (int i = 0; i < numtracks; i++) {
        deadbeef->pl_item_unref (tracks[i]);
    }
}

static void
_set_str (char **dst, const char *value) {
    *dst = value ? strdup (value) : NULL;
}

#pragma mark - disc id

TEST(MusicBrainzTests, discid_matches_the_musicbrainz_reference_toc) {
    // the cd from https://musicbrainz.org/doc/Disc_ID_Calculation, with the
    // lba offsets 0, 15213, 32164, 46442, 63264, 80339 and lead-out 95312
    playlist_t *plt = plt_alloc ("mbdiscid");
    const int sectors[6] = {15213, 16951, 14278, 16822, 17075, 14973};
    DB_playItem_t *tracks[6];
    for (int i = 0; i < 6; i++) {
        tracks[i] = _make_track (plt, sectors[i], 44100);
    }

    EXPECT_TRUE (mb_check_toc (tracks, 6));

    char *discid = mb_make_discid (tracks, 6, NULL);
    EXPECT_STREQ ("49HHV7Eb8UKF3aQiNmu1GR8vKTY-", discid);
    free (discid);

    _free_tracks (tracks, 6);
    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}

TEST(MusicBrainzTests, discid_uses_the_pregap_from_the_cd) {
    playlist_t *plt = plt_alloc ("mbdiscidpg");
    DB_playItem_t *tracks[1] = {_make_track (plt, 100, 44100)};

    // the default assumption for a cd is a 2 second lead-in
    char *with_pregap = mb_make_discid (tracks, 1, NULL);
    char *without_pregap = mb_make_discid (tracks, 1, "00:00:00");
    ASSERT_TRUE (with_pregap != NULL);
    ASSERT_TRUE (without_pregap != NULL);
    EXPECT_STRNE (with_pregap, without_pregap);
    EXPECT_STREQ ("U_VmSwF3YjEsCEKo7beVb4r0Udo-", without_pregap);

    free (with_pregap);
    free (without_pregap);
    _free_tracks (tracks, 1);
    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}

TEST(MusicBrainzTests, discid_adds_the_pregap_tag_to_the_default_lead_in) {
    playlist_t *plt = plt_alloc ("mbdiscidpgtag");
    DB_playItem_t *tracks[1] = {_make_track (plt, 100, 44100)};

    // a ripper can store the pregap of the first track in its "pregap" tag,
    // which adds to the default 2 second lead-in
    deadbeef->pl_replace_meta (tracks[0], "pregap", "00:02:00");
    char *from_tag = mb_make_discid (tracks, 1, NULL);
    char *explicit_pregap = mb_make_discid (tracks, 1, "00:04:00");
    ASSERT_TRUE (from_tag != NULL);
    ASSERT_TRUE (explicit_pregap != NULL);
    EXPECT_STREQ (explicit_pregap, from_tag);

    free (from_tag);
    free (explicit_pregap);
    _free_tracks (tracks, 1);
    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}

TEST(MusicBrainzTests, check_toc_accepts_cd_tracks_and_rejects_others) {
    playlist_t *plt = plt_alloc ("mbtoctest");

    DB_playItem_t *cd[2] = {_make_track (plt, 100, 44100), _make_track (plt, 200, 44100)};
    EXPECT_TRUE (mb_check_toc (cd, 2));
    _free_tracks (cd, 2);

    // 48 khz tracks fit 640 samples into a sector
    DB_playItem_t *cd48[2] = {_make_track (plt, 100, 48000), _make_track (plt, 200, 48000)};
    EXPECT_TRUE (mb_check_toc (cd48, 2));
    _free_tracks (cd48, 2);

    // higher sample rates never come from a cd
    DB_playItem_t *hires[2] = {_make_track (plt, 100, 96000), _make_track (plt, 200, 96000)};
    EXPECT_FALSE (mb_check_toc (hires, 2));
    _free_tracks (hires, 2);

    // all tracks must have the same sample rate
    DB_playItem_t *mixed[2] = {_make_track (plt, 100, 48000), _make_track (plt, 200, 44100)};
    EXPECT_FALSE (mb_check_toc (mixed, 2));
    _free_tracks (mixed, 2);

    // a track which is not a whole number of sectors long
    DB_playItem_t *odd[2] = {_make_track (plt, 100, 44100), _make_track (plt, 200, 44100)};
    deadbeef->plt_set_item_duration ((ddb_playlist_t *)plt, odd[0], 31.234f);
    EXPECT_FALSE (mb_check_toc (odd, 2));
    _free_tracks (odd, 2);

    // submitting a toc to musicbrainz is stricter than making a disc id out of
    // the tracks: 44100 hz and lossless files only
    DB_playItem_t *sub[2] = {_make_track (plt, 100, 44100), _make_track (plt, 200, 44100)};
    EXPECT_TRUE (mb_check_toc_submission (sub, 2));

    deadbeef->pl_replace_meta (sub[0], ":DECODER", "stdmpg");
    EXPECT_FALSE (mb_check_toc_submission (sub, 2));
    deadbeef->pl_delete_meta (sub[0], ":DECODER");

    // the decoder of a track is identified by the id of its plugin, so the
    // lossy decoders have to be listed under their ids, not their names
    deadbeef->pl_replace_meta (sub[0], ":DECODER", "stdogg");
    EXPECT_FALSE (mb_check_toc_submission (sub, 2));
    deadbeef->pl_replace_meta (sub[1], ":DECODER", "wv");
    EXPECT_FALSE (mb_check_toc_submission (sub, 2));
    deadbeef->pl_delete_meta (sub[1], ":DECODER");

    // a lossless decoder does not block the submission
    deadbeef->pl_replace_meta (sub[0], ":DECODER", "stdflac");
    EXPECT_TRUE (mb_check_toc_submission (sub, 2));
    deadbeef->pl_delete_meta (sub[0], ":DECODER");
    _free_tracks (sub, 2);

    DB_playItem_t *sub48[2] = {_make_track (plt, 100, 48000), _make_track (plt, 200, 48000)};
    EXPECT_TRUE (mb_check_toc (sub48, 2));
    EXPECT_FALSE (mb_check_toc_submission (sub48, 2));
    _free_tracks (sub48, 2);

    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}

#pragma mark - tags

static mb_release_t
_make_release (int numtracks) {
    mb_release_t release;
    memset (&release, 0, sizeof (release));
    release.tracks = (mb_track_t *)calloc (numtracks, sizeof (mb_track_t));
    release.num_tracks = numtracks;
    release.totaldiscs = 1;
    release.complete = 1;

    _set_str (&release.title, "Please Please Me");
    _set_str (&release.artist, "The Beatles");
    _set_str (&release.artist_ids, "b10bbbfc-cf9e-42e0-179e-e2c944c176de");
    _set_str (&release.date, "1963-03-22");
    _set_str (&release.original_date, "1963-01-22");
    _set_str (&release.status, "Official");
    _set_str (&release.type, "Album");
    _set_str (&release.country, "XW");
    _set_str (&release.labels, "Parlophone;Other Label");
    _set_str (&release.catalog, "PMC 1202");
    _set_str (&release.barcode, "025218201121");
    _set_str (&release.release_id, "8f84da0e-9997-49a2-8f04-4b216a51f5b5");
    _set_str (&release.release_group_id, "282cd99c-828c-38c6-8c96-6a869bca3ecc");

    for (int i = 0; i < numtracks; i++) {
        mb_track_t *t = &release.tracks[i];
        char title[32];
        snprintf (title, sizeof (title), "Track %d", i + 1);
        _set_str (&t->title, title);
        _set_str (&t->artist, "The Beatles");
        _set_str (&t->artist_ids, "b10bbbfc-cf9e-42e0-179e-e2c944c176de");
        _set_str (&t->recording_id, "9b5fc541-4940-4f6c-9547-23154551c16e");
        _set_str (&t->release_track_id, "d4e0d9d5-2f0d-4a2d-9d0b-9e0a5b09c4a1");
        _set_str (&t->isrcs, "GBF076300010;GBF076300020");
        _set_str (&t->disc_format, "CD");
        t->tracknumber = i + 1;
        t->discnumber = 1;
        t->totaltracks = numtracks;
        t->totaldiscs = 1;
    }
    return release;
}

static void
_free_release (mb_release_t *release) {
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
    free (release->release_id);
    free (release->release_group_id);
    free (release->disc_id);
    free (release->media);
    for (int i = 0; i < release->num_tracks; i++) {
        mb_track_t *t = &release->tracks[i];
        free (t->title);
        free (t->artist);
        free (t->artist_ids);
        free (t->recording_id);
        free (t->release_track_id);
        free (t->isrcs);
        free (t->disc_format);
        free (t->disc_title);
    }
    free (release->tracks);
    free (release->candidate_discs);
    memset (release, 0, sizeof (*release));
}

TEST(MusicBrainzTests, apply_release_writes_the_deadbeef_tag_names) {
    playlist_t *plt = plt_alloc ("mbapply");
    DB_playItem_t *tracks[2] = {_make_track (plt, 100, 44100), _make_track (plt, 200, 44100)};

    mb_release_t release = _make_release (2);
    EXPECT_EQ (2, mb_apply_release (&release, 0, tracks, 2));

    EXPECT_STREQ ("Please Please Me", deadbeef->pl_find_meta (tracks[0], "album"));
    EXPECT_STREQ ("Track 1", deadbeef->pl_find_meta (tracks[0], "title"));
    EXPECT_STREQ ("The Beatles", deadbeef->pl_find_meta (tracks[0], "artist"));
    EXPECT_STREQ ("1", deadbeef->pl_find_meta (tracks[0], "track"));
    EXPECT_STREQ ("2", deadbeef->pl_find_meta (tracks[0], "numtracks"));
    // deadbeef stores the date in the year tag, and only the year by default
    EXPECT_STREQ ("1963", deadbeef->pl_find_meta (tracks[0], "year"));
    EXPECT_STREQ ("1963", deadbeef->pl_find_meta (tracks[0], "ORIGINAL_RELEASE_TIME"));
    EXPECT_STREQ ("Album", deadbeef->pl_find_meta (tracks[0], "MUSICBRAINZ_ALBUMTYPE"));
    EXPECT_STREQ ("Official", deadbeef->pl_find_meta (tracks[0], "MUSICBRAINZ_ALBUMSTATUS"));
    EXPECT_STREQ ("8f84da0e-9997-49a2-8f04-4b216a51f5b5", deadbeef->pl_find_meta (tracks[0], "MUSICBRAINZ_ALBUMID"));
    EXPECT_STREQ ("282cd99c-828c-38c6-8c96-6a869bca3ecc", deadbeef->pl_find_meta (tracks[0], "MUSICBRAINZ_RELEASEGROUPID"));
    // this is the recording id key used by deadbeef itself, and written as an
    // id3v2 UFID frame by junklib
    EXPECT_STREQ ("9b5fc541-4940-4f6c-9547-23154551c16e", deadbeef->pl_find_meta (tracks[0], "musicbrainz_trackid"));
    EXPECT_STREQ ("9b5fc541-4940-4f6c-9547-23154551c16e", deadbeef->pl_find_meta (tracks[0], "MUSICBRAINZ_TRACKID"));
    // multivalue tags are written as separate values
    EXPECT_STREQ ("GBF076300010", deadbeef->pl_find_meta (tracks[0], "ISRC"));

    EXPECT_STREQ ("Track 2", deadbeef->pl_find_meta (tracks[1], "title"));
    EXPECT_STREQ ("2", deadbeef->pl_find_meta (tracks[1], "track"));

    // a release with a single disc has no disc numbers
    EXPECT_TRUE (deadbeef->pl_find_meta (tracks[0], "disc") == NULL);
    // and a single artist release gets no album artist
    EXPECT_TRUE (deadbeef->pl_find_meta (tracks[0], "album artist") == NULL);

    _free_release (&release);
    _free_tracks (tracks, 2);
    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}

TEST(MusicBrainzTests, apply_release_clears_the_stale_tags_of_the_previous_release) {
    playlist_t *plt = plt_alloc ("mbapplystale");
    DB_playItem_t *tracks[2] = {_make_track (plt, 100, 44100), _make_track (plt, 200, 44100)};

    // a multdisc release leaves disc numbers and a disc subtitle
    mb_release_t multidisc = _make_release (2);
    multidisc.totaldiscs = 2;
    for (int i = 0; i < 2; i++) {
        multidisc.tracks[i].totaldiscs = 2;
        _set_str (&multidisc.tracks[i].disc_title, "Disc one");
    }
    EXPECT_EQ (2, mb_apply_release (&multidisc, 0, tracks, 2));
    EXPECT_STREQ ("1", deadbeef->pl_find_meta (tracks[0], "disc"));
    EXPECT_STREQ ("2", deadbeef->pl_find_meta (tracks[0], "numdiscs"));
    EXPECT_STREQ ("Disc one", deadbeef->pl_find_meta (tracks[0], "discsubtitle"));
    EXPECT_STREQ ("1963", deadbeef->pl_find_meta (tracks[0], "year"));

    // a single disc release without a date has to remove all of them again
    mb_release_t release = _make_release (2);
    free (release.date);
    release.date = NULL;
    EXPECT_EQ (2, mb_apply_release (&release, 0, tracks, 2));
    EXPECT_TRUE (deadbeef->pl_find_meta (tracks[0], "disc") == NULL);
    EXPECT_TRUE (deadbeef->pl_find_meta (tracks[0], "numdiscs") == NULL);
    EXPECT_TRUE (deadbeef->pl_find_meta (tracks[0], "discsubtitle") == NULL);
    EXPECT_TRUE (deadbeef->pl_find_meta (tracks[0], "year") == NULL);
    EXPECT_TRUE (deadbeef->pl_find_meta (tracks[0], "ORIGINAL_RELEASE_TIME") == NULL);

    _free_release (&multidisc);
    _free_release (&release);
    _free_tracks (tracks, 2);
    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}

TEST(MusicBrainzTests, apply_release_writes_album_artist_for_various_artists) {
    playlist_t *plt = plt_alloc ("mbapplyva");
    DB_playItem_t *tracks[2] = {_make_track (plt, 100, 44100), _make_track (plt, 200, 44100)};

    mb_release_t release = _make_release (2);
    free (release.tracks[1].artist);
    _set_str (&release.tracks[1].artist, "Other Artist");

    EXPECT_EQ (2, mb_apply_release (&release, 0, tracks, 2));
    EXPECT_STREQ ("The Beatles", deadbeef->pl_find_meta (tracks[0], "album artist"));
    EXPECT_STREQ ("Other Artist", deadbeef->pl_find_meta (tracks[1], "artist"));

    _free_release (&release);
    _free_tracks (tracks, 2);
    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}

TEST(MusicBrainzTests, apply_release_writes_disc_numbers_of_multidisc_releases) {
    playlist_t *plt = plt_alloc ("mbapplydiscs");
    DB_playItem_t *tracks[2] = {_make_track (plt, 100, 44100), _make_track (plt, 200, 44100)};

    // a release of 2 discs with 2 tracks each; only the second disc is selected
    mb_release_t release = _make_release (4);
    release.totaldiscs = 2;
    release.complete = 0;
    for (int i = 0; i < 4; i++) {
        release.tracks[i].totaldiscs = 2;
        release.tracks[i].totaltracks = 2;
        release.tracks[i].discnumber = i < 2 ? 1 : 2;
        // musicbrainz numbers the tracks of every disc from 1
        release.tracks[i].tracknumber = i % 2 + 1;
    }
    _set_str (&release.tracks[2].disc_title, "Bonus Tracks");
    _set_str (&release.tracks[3].disc_title, "Bonus Tracks");

    EXPECT_EQ (2, mb_apply_release (&release, 2, tracks, 2));
    EXPECT_STREQ ("Track 3", deadbeef->pl_find_meta (tracks[0], "title"));
    EXPECT_STREQ ("Track 4", deadbeef->pl_find_meta (tracks[1], "title"));
    EXPECT_STREQ ("2", deadbeef->pl_find_meta (tracks[0], "disc"));
    EXPECT_STREQ ("2", deadbeef->pl_find_meta (tracks[0], "numdiscs"));
    EXPECT_STREQ ("1", deadbeef->pl_find_meta (tracks[0], "track"));
    EXPECT_STREQ ("2", deadbeef->pl_find_meta (tracks[0], "numtracks"));
    EXPECT_STREQ ("Bonus Tracks", deadbeef->pl_find_meta (tracks[0], "discsubtitle"));

    // the first disc does not match a selection of 3 files
    DB_playItem_t *three[3] = {tracks[0], tracks[1], _make_track (plt, 300, 44100)};
    EXPECT_EQ (0, mb_apply_release (&release, 1, three, 3));
    deadbeef->pl_item_unref (three[2]);

    _free_release (&release);
    _free_tracks (tracks, 2);
    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}

TEST(MusicBrainzTests, apply_release_rejects_a_release_that_does_not_fit_the_selection) {
    playlist_t *plt = plt_alloc ("mbapplyreject");
    DB_playItem_t *tracks[2] = {_make_track (plt, 100, 44100), _make_track (plt, 200, 44100)};

    mb_release_t release = _make_release (3);
    EXPECT_EQ (0, mb_apply_release (&release, 0, tracks, 2));

    _free_release (&release);
    _free_tracks (tracks, 2);
    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}

#pragma mark - helpers

TEST(MusicBrainzTests, is_uuid_accepts_only_mbid_format) {
    EXPECT_TRUE (mb_is_uuid ("8f84da0e-9997-49a2-8f04-4b216a51f5b5"));
    EXPECT_FALSE (mb_is_uuid ("8f84da0e-9997-49a2-8f04-4b216a51f5b"));
    EXPECT_FALSE (mb_is_uuid ("8f84da0e-9997-49a2-8f04-4b216a51f5bz"));
    EXPECT_FALSE (mb_is_uuid ("8f84da0e999749a28f044b216a51f5b5"));
    EXPECT_FALSE (mb_is_uuid (""));
    EXPECT_FALSE (mb_is_uuid (NULL));
}

TEST(MusicBrainzTests, strip_album_edition_removes_trailing_descriptors) {
    char *stripped = mb_strip_album_edition ("Please Please Me [Deluxe Edition]");
    EXPECT_STREQ ("Please Please Me", stripped);
    free (stripped);

    // every descriptor at the end is removed
    stripped = mb_strip_album_edition ("Please Please Me (20th Anniversary Remastered)");
    EXPECT_STREQ ("Please Please Me", stripped);
    free (stripped);

    // a descriptor in the middle of the name is kept
    stripped = mb_strip_album_edition ("Deluxe Edition Sessions");
    EXPECT_STREQ ("Deluxe Edition Sessions", stripped);
    free (stripped);

    stripped = mb_strip_album_edition ("Please Please Me");
    EXPECT_STREQ ("Please Please Me", stripped);
    free (stripped);
}

TEST(MusicBrainzTests, extract_mbid_finds_the_id_in_a_pasted_url) {
    char *mbid = mb_extract_mbid ("8f84da0e-9997-49a2-8f04-4b216a51f5b5");
    EXPECT_STREQ ("8f84da0e-9997-49a2-8f04-4b216a51f5b5", mbid);
    free (mbid);

    // the address of the release page, copied from a browser
    mbid = mb_extract_mbid ("https://musicbrainz.org/release/8f84da0e-9997-49a2-8f04-4b216a51f5b5#work");
    EXPECT_STREQ ("8f84da0e-9997-49a2-8f04-4b216a51f5b5", mbid);
    free (mbid);

    EXPECT_TRUE (mb_extract_mbid ("please please me") == NULL);
    EXPECT_TRUE (mb_extract_mbid ("8f84da0e-9997-49a2-8f04-4b216a51f5b") == NULL);
    EXPECT_TRUE (mb_extract_mbid ("") == NULL);
    EXPECT_TRUE (mb_extract_mbid (NULL) == NULL);
}

TEST(MusicBrainzTests, make_toc_url_puts_the_lead_out_first) {
    // the cd from https://musicbrainz.org/doc/Disc_ID_Calculation, whose lead
    // out is at sector 95462 and whose tracks start at 150, 15363, 32314,
    // 46592, 63414 and 80489
    playlist_t *plt = plt_alloc ("mbtocurl");
    const int sectors[6] = {15213, 16951, 14278, 16822, 17075, 14973};
    DB_playItem_t *tracks[6];
    for (int i = 0; i < 6; i++) {
        tracks[i] = _make_track (plt, sectors[i], 44100);
    }

    char *url = mb_make_toc_url (tracks, 6);
    ASSERT_TRUE (url != NULL);
    EXPECT_TRUE (strstr (url, "cdtoc/attach?toc=1+6+95462+150+15363+32314+46592+63414+80489") != NULL);
    // the disc id the toc calculates to, which the submission form also takes
    EXPECT_TRUE (strstr (url, "&tracks=6&id=49HHV7Eb8UKF3aQiNmu1GR8vKTY-") != NULL);
    free (url);

    _free_tracks (tracks, 6);
    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}

TEST(MusicBrainzTests, find_common_mbid_requires_the_same_id_in_every_track) {
    playlist_t *plt = plt_alloc ("mbfindmbid");
    DB_playItem_t *tracks[2] = {_make_track (plt, 100, 44100), _make_track (plt, 200, 44100)};

    EXPECT_TRUE (mb_find_common_mbid (tracks, 2, "MUSICBRAINZ_ALBUMID") == NULL);

    deadbeef->pl_replace_meta (tracks[0], "MUSICBRAINZ_ALBUMID", "8f84da0e-9997-49a2-8f04-4b216a51f5b5");
    EXPECT_TRUE (mb_find_common_mbid (tracks, 2, "MUSICBRAINZ_ALBUMID") == NULL);

    deadbeef->pl_replace_meta (tracks[1], "MUSICBRAINZ_ALBUMID", "8f84da0e-9997-49a2-8f04-4b216a51f5b5");
    char *mbid = mb_find_common_mbid (tracks, 2, "MUSICBRAINZ_ALBUMID");
    EXPECT_STREQ ("8f84da0e-9997-49a2-8f04-4b216a51f5b5", mbid);
    free (mbid);

    // a different id in one of the tracks means there is nothing common
    deadbeef->pl_replace_meta (tracks[1], "MUSICBRAINZ_ALBUMID", "282cd99c-828c-38c6-8c96-6a869bca3ecc");
    EXPECT_TRUE (mb_find_common_mbid (tracks, 2, "MUSICBRAINZ_ALBUMID") == NULL);

    _free_tracks (tracks, 2);
    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}

TEST(MusicBrainzTests, find_common_artist_album_uses_the_selection_tags) {
    playlist_t *plt = plt_alloc ("mbfindaa");
    DB_playItem_t *tracks[2] = {_make_track (plt, 100, 44100), _make_track (plt, 200, 44100)};

    char *artist = NULL;
    char *album = NULL;

    // nothing is tagged
    mb_find_common_artist_album (tracks, 2, &artist, &album);
    EXPECT_TRUE (artist == NULL);
    EXPECT_TRUE (album == NULL);

    deadbeef->pl_replace_meta (tracks[0], "album artist", "The Beatles");
    deadbeef->pl_replace_meta (tracks[0], "album", "Please Please Me");
    deadbeef->pl_replace_meta (tracks[1], "album artist", "The Beatles");
    deadbeef->pl_replace_meta (tracks[1], "album", "Please Please Me");
    mb_find_common_artist_album (tracks, 2, &artist, &album);
    EXPECT_STREQ ("The Beatles", artist);
    EXPECT_STREQ ("Please Please Me", album);
    free (artist);
    free (album);

    // different albums still have the artist in common
    deadbeef->pl_replace_meta (tracks[1], "album", "With The Beatles");
    mb_find_common_artist_album (tracks, 2, &artist, &album);
    EXPECT_STREQ ("The Beatles", artist);
    EXPECT_TRUE (album == NULL);
    free (artist);

    // an untagged track only gives up its own values, not the ones the other
    // tracks agree on
    deadbeef->pl_replace_meta (tracks[1], "album", "Please Please Me");
    deadbeef->pl_delete_meta (tracks[1], "album artist");
    deadbeef->pl_delete_meta (tracks[1], "artist");
    mb_find_common_artist_album (tracks, 2, &artist, &album);
    EXPECT_TRUE (artist == NULL);
    EXPECT_STREQ ("Please Please Me", album);
    free (album);

    _free_tracks (tracks, 2);
    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}
