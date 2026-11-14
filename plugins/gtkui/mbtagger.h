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

#ifndef ____mbtagger__
#define ____mbtagger__

#include <deadbeef/deadbeef.h>

// The "Tagging/MusicBrainz" context menu actions, which look up releases on
// MusicBrainz and tag the selected tracks with the data of the selected one.

// returns the head of the MusicBrainz action list
DB_plugin_action_t *
mbtagger_get_actions (DB_playItem_t *it);

// appends the GTKUI action list after the MusicBrainz actions; must be called
// before the actions are used
void
mbtagger_link_actions (DB_plugin_action_t *gtkui_actions);

#endif // ____mbtagger__
