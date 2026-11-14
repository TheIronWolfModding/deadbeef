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
     in a product, an acknowledgment would be appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.

    3. This notice may not be removed or altered from any source distribution.
*/

#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <deadbeef/deadbeef.h>
#include "playlist.h"
extern "C" {
    #include "../shared/trkproperties_shared.h"
}
#include <gtest/gtest.h>

extern DB_functions_t *deadbeef;
extern char dbplugindir[PATH_MAX];

static int
_copy_file (const char *src, const char *dst) {
    FILE *in = fopen (src, "rb");
    if (!in) {
        return -1;
    }
    FILE *out = fopen (dst, "wb");
    if (!out) {
        fclose (in);
        return -1;
    }
    char buf[4096];
    size_t n;
    while ((n = fread (buf, 1, sizeof (buf), in)) > 0) {
        if (fwrite (buf, 1, n, out) != n) {
            fclose (in);
            fclose (out);
            return -1;
        }
    }
    fclose (in);
    fclose (out);
    return 0;
}

static DB_playItem_t *
_make_track (playlist_t *plt, const char *uri) {
    DB_playItem_t *it = deadbeef->pl_item_alloc ();
    deadbeef->pl_replace_meta (it, ":URI", uri);
    deadbeef->pl_replace_meta (it, ":DECODER", "stdmpg");
    deadbeef->pl_add_meta (it, "title", "Some Title");
    deadbeef->pl_add_meta (it, "artist", "Some Artist");
    deadbeef->pl_add_meta (it, "album", "Some Album");
    deadbeef->pl_add_meta (it, "comment", "Some Comment");
    deadbeef->pl_set_item_flags (it, DDB_TAG_ID3V23);
    deadbeef->plt_insert_item ((ddb_playlist_t *)plt, NULL, it);
    return it;
}

TEST(ClearTagsTests, clear_tags_removes_user_tags_and_keeps_internal_metadata) {
    playlist_t *plt = plt_alloc ("testplt");
    DB_playItem_t *it = _make_track (plt, "/nonexistent/deadbeef/test.mp3");

    EXPECT_TRUE (deadbeef->pl_find_meta (it, "title") != NULL);

    trkproperties_clear_tags (&it, 1);

    EXPECT_TRUE (deadbeef->pl_find_meta (it, "title") == NULL);
    EXPECT_TRUE (deadbeef->pl_find_meta (it, "artist") == NULL);
    EXPECT_TRUE (deadbeef->pl_find_meta (it, "album") == NULL);
    EXPECT_TRUE (deadbeef->pl_find_meta (it, "comment") == NULL);
    EXPECT_TRUE ((deadbeef->pl_get_item_flags (it) & DDB_TAG_MASK) == 0);

    EXPECT_TRUE (deadbeef->pl_find_meta (it, ":URI") != NULL);
    EXPECT_TRUE (deadbeef->pl_find_meta (it, ":DECODER") != NULL);

    deadbeef->plt_unref ((ddb_playlist_t *)plt);
}

TEST(ClearTagsTests, clear_tags_removes_tags_from_the_file_on_disk) {
    char src[PATH_MAX];
    char dst[PATH_MAX];
    snprintf (src, sizeof (src), "%s/TestData/tone1sec_id3v1.mp3", dbplugindir);
    snprintf (dst, sizeof (dst), "%s/TestData/cleartags_copy.mp3", dbplugindir);

    ASSERT_EQ (_copy_file (src, dst), 0);

    DB_decoder_t *dec = (DB_decoder_t *)deadbeef->plug_get_for_id ("stdmpg");
    if (dec == NULL || dec->write_metadata == NULL) {
        // the mp3 plugin is not available in this build, nothing to write
        unlink (dst);
        return;
    }

    playlist_t *plt = plt_alloc ("testplt");
    DB_playItem_t *it = _make_track (plt, dst);

    EXPECT_EQ (trkproperties_clear_tags (&it, 1), 1);
    EXPECT_TRUE (deadbeef->pl_find_meta (it, "title") == NULL);

    // re-read the tags from the rewritten file, they must be gone
    EXPECT_EQ (dec->read_metadata (it), 0);
    EXPECT_TRUE (deadbeef->pl_find_meta (it, "title") == NULL);
    EXPECT_TRUE (deadbeef->pl_find_meta (it, "artist") == NULL);
    EXPECT_TRUE (deadbeef->pl_find_meta (it, "album") == NULL);

    deadbeef->plt_unref ((ddb_playlist_t *)plt);
    unlink (dst);
}
