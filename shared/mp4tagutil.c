/*
    DeaDBeeF -- the music player
    Copyright (C) 2009-2016 Alexey Yakovenko and other contributors

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

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "mp4tagutil.h"
#include "mp4parser.h"

#ifndef __linux__
#define off64_t off_t
#define lseek64 lseek
#define O_LARGEFILE 0
#endif

extern DB_functions_t *deadbeef;

#define COPYRIGHT_SYM "\xa9"

static const char *_mp4_atom_map[] = {
    COPYRIGHT_SYM "alb", "album",
    COPYRIGHT_SYM "art", "artist",
    "aART", "band",
    COPYRIGHT_SYM "cmt", "comment",
    COPYRIGHT_SYM "day", "year",
    COPYRIGHT_SYM "nam", "title",
    COPYRIGHT_SYM "gen", "genre",
    "gnre", "genre",
    "trkn", "track"
    "disk", "disc",
    COPYRIGHT_SYM "wrt", "composer",
    COPYRIGHT_SYM "too", "encoder",
    "tmpo", "bpm",
    "cprt", "copyright",
    COPYRIGHT_SYM "grp", "grouping",
    "cpil", "compilation",
    "pcst", "podcast",
    "catg", "category",
    "keyw", "keyword",
    "desc", "description",
    COPYRIGHT_SYM "lyr", "lyrics",
    "purd", "purchase date",
    "MusicBrainz Track Id", "musicbrainz_trackid",

    NULL, NULL
};

/* For writing:
 * Load/get the existing udta atom
 * If present:
 *   Find ilst
 *   Remove all known non-custom fields, keep the rest
 *   Remove all custom fields
 * If not present:
 *   Create new udta/meta/ilst
 * Re-append all new non-custom fields
 * Re-append all new custom fields
 * Generate data block
 * If the new udta block can fit over the old one, with at least 8 bytes extra for the "free" atom:
 *   Overwrite the old block
 *   Pad with "free" atom if necessary
 * If can't fit: the entire moov atom has to be relocated!
 *   Rename the existing moov into "free"
 *   Append the modified moov block to the end of file, after the last atom
 *   IMPORTANT: the entirety of moov atom with all sub atoms needs to be loaded and saved
 * Further work:
 *   Find if there are "free" blocks between ftyp and mdat, and try to fit the moov there; If that works, truncate the file.
 */

static void
_remove_known_fields (mp4p_atom_t *ilst) {
    mp4p_atom_t *meta_atom = ilst->subatoms;
    while (meta_atom) {
        mp4p_atom_t *next = meta_atom->next;
        mp4p_meta_t *meta = meta_atom->data;

        for (int i = 0; _mp4_atom_map[i]; i++) {
            char type[5];
            memcpy (type, meta_atom->type, 4);
            type[4] = 0;
            if (meta->name || !strcasecmp(type, _mp4_atom_map[i])) {
                mp4p_atom_remove_subatom (ilst, meta_atom);
                break;
            }
        }
        meta_atom = next;
    }
}

void
mp4tagutil_modify_meta (mp4p_atom_t *mp4file, DB_playItem_t *it) {
    mp4p_atom_t *udta = NULL;
    mp4p_atom_t *meta = NULL;
    mp4p_atom_t *ilst = NULL;

    udta = mp4p_atom_find(mp4file, "moov/udta");
    if (udta) {
        meta = mp4p_atom_find(udta, "udta/meta");
    }
    else {
        udta = mp4p_atom_append (mp4file, mp4p_atom_new ("udta"));
    }

    if (meta) {
        ilst = mp4p_atom_find(meta, "meta/ilst");
    }
    else {
        meta = mp4p_atom_append (udta, mp4p_atom_new ("meta"));
    }

    if (ilst) {
        _remove_known_fields (ilst);
    }
    else {
        ilst = mp4p_atom_append(meta, mp4p_atom_new ("ilst"));
    }

    deadbeef->pl_lock ();
    DB_metaInfo_t *m = deadbeef->pl_get_metadata_head (it);
    while (m) {
        if (strchr (":!_", m->key[0])) {
            break;
        }

        if (!strcasecmp (m->key, "track")
            || !strcasecmp (m->key, "numtracks")
            || !strcasecmp (m->key, "disc")
            || !strcasecmp (m->key, "numdiscs")
            || !strcasecmp (m->key, "genre")) {
            m = m->next;
            continue;
        }

        int i;
        for (i = 0; _mp4_atom_map[i]; i += 2) {
            if (!strcasecmp (_mp4_atom_map[i+1], m->key)) {
                break;
            }
        }

        const char *value = m->value;
        const char *end = m->value + m->valuesize;
        while (value < end) {
            if (!_mp4_atom_map[i] || strlen (_mp4_atom_map[i]) != 4) {
                mp4p_ilst_append_custom(mp4file, _mp4_atom_map[i] ? _mp4_atom_map[i] : m->key, value);
            }
            else {
                mp4p_ilst_append_text(ilst, _mp4_atom_map[i], value);
            }
            size_t l = strlen (value) + 1;
            value += l;
        }
        m = m->next;
    }

    const char *genre = deadbeef->pl_find_meta (it, "genre");
    if (genre) {
        mp4p_ilst_append_genre (ilst, genre);
    }
    const char *track = deadbeef->pl_find_meta (it, "track");
    const char *numtracks = deadbeef->pl_find_meta (it, "numtracks");
    const char *disc = deadbeef->pl_find_meta (it, "disc");
    const char *numdiscs = deadbeef->pl_find_meta (it, "numdiscs");

    uint16_t itrack = 0, inumtracks = 0, idisc = 0, inumdiscs = 0;
    if (track) {
        itrack = atoi (track);
    }
    if (numtracks) {
        inumtracks = atoi (numtracks);
    }
    if (disc) {
        idisc = atoi (disc);
    }
    if (numdiscs) {
        inumdiscs = atoi (numdiscs);
    }
    if (itrack || inumtracks) {
        mp4p_ilst_append_track_disc(mp4file, "trck", itrack, inumtracks);
    }
    if (idisc || inumdiscs) {
        mp4p_ilst_append_track_disc(mp4file, "disk", itrack, inumtracks);
    }

    static const char *tag_rg_names[] = {
        "replaygain_album_gain",
        "replaygain_album_peak",
        "replaygain_track_gain",
        "replaygain_track_peak",
        NULL
    };

    // replaygain key names in deadbeef internal metadata
    static const char *ddb_internal_rg_keys[] = {
        ":REPLAYGAIN_ALBUMGAIN",
        ":REPLAYGAIN_ALBUMPEAK",
        ":REPLAYGAIN_TRACKGAIN",
        ":REPLAYGAIN_TRACKPEAK",
        NULL
    };

    // add replaygain values
    for (int n = 0; ddb_internal_rg_keys[n]; n++) {
        if (deadbeef->pl_find_meta (it, ddb_internal_rg_keys[n])) {
            float value = deadbeef->pl_get_item_replaygain (it, n);
            char s[100];
            snprintf (s, sizeof (s), "%f", value);
            mp4p_ilst_append_custom(mp4file, tag_rg_names[n], s);
        }
    }
    
    deadbeef->pl_unlock ();
}

int
mp4_write_metadata (DB_playItem_t *it) {
    deadbeef->pl_lock ();
    DB_FILE *fp = deadbeef->fopen (deadbeef->pl_find_meta (it, ":URI"));
    deadbeef->pl_unlock ();

    if (!fp) {
        return -1;
    }

    int junk = deadbeef->junk_get_leading_size (fp);
    if (junk >= 0) {
        deadbeef->fseek (fp, junk, SEEK_SET);
    }
    else {
        junk = 0;
    }

    mp4p_file_callbacks_t mp4reader;
    mp4reader.data = fp;
    mp4reader.fread = (size_t (*) (void *ptr, size_t size, size_t nmemb, void *stream))deadbeef->fread;
    mp4reader.fseek = (int (*) (void *stream, int64_t offset, int whence))deadbeef->fseek;
    mp4reader.ftell = (int64_t (*) (void *stream))deadbeef->ftell;
    mp4p_atom_t *mp4file = mp4p_open(NULL, &mp4reader);

    deadbeef->fclose (fp);

    if (!mp4file) {
        return -1;
    }

    mp4tagutil_modify_meta(mp4file, it);

    return mp4p_update_metadata (mp4file);
}

static void
mp4_load_tags (mp4p_atom_t *mp4file, DB_playItem_t *it) {
    int got_itunes_tags = 0;

    mp4p_atom_t *ilst_atom = mp4p_atom_find (mp4file, "moov/udta/meta/ilst");
    mp4p_atom_t *meta_atom = ilst_atom->subatoms;

    while (meta_atom) {
        got_itunes_tags = 1;

        mp4p_meta_t *meta = meta_atom->data;

        char type[5];
        memcpy (type, meta_atom->type, 4);
        type[4] = 0;
        const char *name = meta->name ? meta->name : type;

        if (!strcasecmp (name, "replaygain_track_gain")) {
            deadbeef->pl_set_item_replaygain (it, DDB_REPLAYGAIN_TRACKGAIN, atof (meta->text));
            continue;
        }
        else if (!strcasecmp (name, "replaygain_album_gain")) {
            deadbeef->pl_set_item_replaygain (it, DDB_REPLAYGAIN_ALBUMGAIN, atof (meta->text));
            continue;
        }
        else if (!strcasecmp (name, "replaygain_track_peak")) {
            deadbeef->pl_set_item_replaygain (it, DDB_REPLAYGAIN_TRACKPEAK, atof (meta->text));
            continue;
        }
        else if (!strcasecmp (name, "replaygain_album_peak")) {
            deadbeef->pl_set_item_replaygain (it, DDB_REPLAYGAIN_ALBUMPEAK, atof (meta->text));
            continue;
        }

        for (int i = 0; _mp4_atom_map[i]; i += 2) {
            if (!strcasecmp (name, _mp4_atom_map[i])) {
                if (meta->text) {
                    deadbeef->pl_append_meta (it, _mp4_atom_map[i+1], meta->text);
                }
                else if (meta->values) {
                    if (!memcmp (meta_atom->type, "trkn", 4)) {
                        if (meta->data_size >= 6) { // leading + idx + total
                            uint16_t track = meta->values[1];
                            uint16_t total = meta->values[2];
                            char s[10];
                            if (track) {
                                snprintf (s, sizeof (s), "%d", (int)track);
                                deadbeef->pl_replace_meta (it, "track", s);
                            }
                            if (total) {
                                snprintf (s, sizeof (s), "%d", (int)total);
                                deadbeef->pl_replace_meta (it, "numtracks", s);
                            }
                        }
                    }
                    else if (!memcmp (meta_atom->type, "disk", 4)) {
                        if (meta->data_size >= 6) { // leading + idx + total
                            uint16_t track = meta->values[1];
                            uint16_t total = meta->values[2];
                            char s[10];
                            if (track) {
                                snprintf (s, sizeof (s), "%d", (int)track);
                                deadbeef->pl_replace_meta (it, "disc", s);
                            }
                            if (total) {
                                snprintf (s, sizeof (s), "%d", (int)total);
                                deadbeef->pl_replace_meta (it, "numdiscs", s);
                            }
                        }
                    }
                    else if (!strcmp (_mp4_atom_map[i+1], "genre")) {
                        if (meta->values[0]) {
                            const char *genre = mp4p_genre_name_for_index(meta->values[0]);
                            if (genre) {
                                deadbeef->pl_replace_meta (it, _mp4_atom_map[i+1], genre);
                            }
                        }
                    }
                    else {
                        char s[10];
                        snprintf (s, sizeof (s), "%d", (int)meta->values[0]);
                        deadbeef->pl_replace_meta (it, _mp4_atom_map[i+1], s);
                    }
                }
                break;
            }
        }
        meta_atom = meta_atom->next;
    }
    if (got_itunes_tags) {
        uint32_t f = deadbeef->pl_get_item_flags (it);
        f |= DDB_TAG_ITUNES;
        deadbeef->pl_set_item_flags (it, f);
    }
}

int
mp4_read_metadata_file (DB_playItem_t *it, DB_FILE *fp) {
    int junk = deadbeef->junk_get_leading_size (fp);
    if (junk >= 0) {
        deadbeef->fseek (fp, junk, SEEK_SET);
    }
    else {
        junk = 0;
    }

    mp4p_file_callbacks_t mp4reader;
    mp4reader.data = fp;
    mp4reader.fread = (size_t (*) (void *ptr, size_t size, size_t nmemb, void *stream))deadbeef->fread;
    mp4reader.fseek = (int (*) (void *stream, int64_t offset, int whence))deadbeef->fseek;
    mp4reader.ftell = (int64_t (*) (void *stream))deadbeef->ftell;
    mp4p_atom_t *mp4file = mp4p_open(NULL, &mp4reader);

    deadbeef->pl_delete_all_meta (it);

    // convert
    mp4_load_tags (mp4file, it);
    mp4p_atom_free (mp4file);

    (void)deadbeef->junk_apev2_read (it, fp);
    (void)deadbeef->junk_id3v2_read (it, fp);
    (void)deadbeef->junk_id3v1_read (it, fp);
    return 0;
}

int
mp4_read_metadata (DB_playItem_t *it) {
    deadbeef->pl_lock ();
    DB_FILE *fp = deadbeef->fopen (deadbeef->pl_find_meta (it, ":URI"));
    deadbeef->pl_unlock ();
    if (!fp) {
        return -1;
    }

    if (fp->vfs->is_streaming ()) {
        deadbeef->fclose (fp);
        return -1;
    }

    int res = mp4_read_metadata_file(it, fp);
    deadbeef->fclose (fp);

    return res;
}

