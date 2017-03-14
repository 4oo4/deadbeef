/*
    Media Library plugin for DeaDBeeF Player
    Copyright (C) 2009-2017 Alexey Yakovenko

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

#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include "../../deadbeef.h"
#include "medialib.h"

DB_functions_t *deadbeef;

static int filter_id;
static char *artist_album_bc;
static char *title_bc;

typedef struct coll_item_s {
    DB_playItem_t *it;
    struct coll_item_s *next;
} coll_item_t;

typedef struct ml_string_s {
    const char *text;
    coll_item_t *items;
    int items_count;
    struct ml_string_s *bucket_next;
    struct ml_string_s *next;
} ml_string_t;

typedef struct ml_entry_s {
    const char *file;
    const char *title;
    int subtrack;
    ml_string_t *artist;
    ml_string_t *album;
    ml_string_t *genre;
    ml_string_t *folder;
    struct ml_entry_s *next;
    struct ml_entry_s *bucket_next;
} ml_entry_t;

#define ML_HASH_SIZE 4096

// a list of unique names in collection, as a list, and as a hash, with each item associated with list of tracks
typedef struct {
    ml_string_t *hash[ML_HASH_SIZE];
    ml_string_t *head;
    ml_string_t *tail;
    int count;
} collection_t;

typedef struct {
    // plain list of all tracks in the entire collection
    ml_entry_t *tracks;

    // hash formed by filename pointer
    // this hash purpose is to quickly check whether the filename is in the library already
    // NOTE: this hash doesn't contain all of the tracks from the `tracks` list, because of subtracks
    ml_entry_t *filename_hash[ML_HASH_SIZE];

    // plain lists for each index
    collection_t albums;
    collection_t artists;
    collection_t genres;
    collection_t folders;
} ml_db_t;

static uint32_t
hash_for_ptr (void *ptr) {
    // scrambling multiplier from http://vigna.di.unimi.it/ftp/papers/xorshift.pdf
    uint64_t scrambled = 1181783497276652981ULL * (uintptr_t)ptr;
    return (uint32_t)(scrambled & (ML_HASH_SIZE-1));
}

static ml_string_t *
hash_find_for_hashkey (ml_string_t **hash, const char *val, uint32_t h) {
    ml_string_t *bucket = hash[h];
    while (bucket) {
        if (bucket->text == val) {
            return bucket;
        }
        bucket = bucket->bucket_next;
    }
    return NULL;
}

ml_string_t *
hash_find (ml_string_t **hash, const char *val) {
    uint32_t h = hash_for_ptr ((void *)val) & (ML_HASH_SIZE-1);
    return hash_find_for_hashkey(hash, val, h);
}

static ml_string_t *
hash_add (ml_string_t **hash, const char *val, DB_playItem_t *it) {
    uint32_t h = hash_for_ptr ((void *)val) & (ML_HASH_SIZE-1);
    ml_string_t *s = hash_find_for_hashkey(hash, val, h);
    ml_string_t *retval = NULL;
    if (!s) {
        deadbeef->metacache_add_string (val);
        s = calloc (sizeof (ml_string_t), 1);
        s->bucket_next = hash[h];
        s->text = val;
        deadbeef->metacache_add_string (val);
        hash[h] = s;
        retval = s;
    }

    coll_item_t *item = calloc (sizeof (coll_item_t), 1);
    deadbeef->pl_item_ref (it);
    item->it = it;

    coll_item_t *tail = s->items;
    while (tail && tail->next) {
        tail = tail->next;
    }

    if (tail) {
        tail->next = item;
    }
    else {
        s->items = item;
    }

    s->items_count++;

    return retval;
}

static ddb_playlist_t *ml_playlist; // this playlist contains the actual data of the media library in plain list

static ml_db_t db; // this is the index, which can be rebuilt from the playlist at any given time

static uintptr_t mutex;

#define MAX_LISTENERS 10
static ddb_medialib_listener_t listeners[MAX_LISTENERS];
static void *listeners_ud[MAX_LISTENERS];

static ml_string_t *
ml_reg_col (collection_t *coll, const char *c, DB_playItem_t *it) {
    int need_unref = 0;
    if (!c) {
        c = deadbeef->metacache_add_string ("");
        need_unref = 1;
    }
    ml_string_t *s = hash_add (coll->hash, c, it);
    if (s) {
        if (coll->tail) {
            coll->tail->next = s;
            coll->tail = s;
        }
        else {
            coll->tail = coll->head = s;
        }
        coll->count++;
    }
    if (need_unref) {
        deadbeef->metacache_remove_string (c);
    }
    return s;
}

static void
ml_free_col (collection_t *coll) {
    ml_string_t *s = coll->head;
    while (s) {
        ml_string_t *next = s->next;

        while (s->items) {
            coll_item_t *next = s->items->next;
            deadbeef->pl_item_unref (s->items->it);
            free (s->items);
            s->items = next;
        }

        if (s->text) {
            deadbeef->metacache_remove_string (s->text);
        }
        free (s);
        s = next;
    }
    memset (coll->hash, 0, sizeof (coll->hash));
    coll->head = NULL;
    coll->tail = NULL;
}

DB_playItem_t *(*plt_insert_dir) (ddb_playlist_t *plt, DB_playItem_t *after, const char *dirname, int *pabort, int (*cb)(DB_playItem_t *it, void *data), void *user_data);

uintptr_t tid;
int scanner_terminate;

static int
add_file_info_cb (DB_playItem_t *it, void *data) {
//    fprintf (stderr, "added %s                                 \r", deadbeef->pl_find_meta (it, ":URI"));
    return 0;
}

static void
ml_free_db (void) {
    fprintf (stderr, "clearing index...\n");

    deadbeef->mutex_lock (mutex);
    ml_free_col(&db.albums);
    ml_free_col(&db.artists);
    ml_free_col(&db.genres);
    ml_free_col(&db.folders);

    while (db.tracks) {
        ml_entry_t *next = db.tracks->next;
        if (db.tracks->title) {
            deadbeef->metacache_remove_string (db.tracks->title);
        }
        if (db.tracks->file) {
            deadbeef->metacache_remove_string (db.tracks->file);
        }
        free (db.tracks);
        db.tracks = next;
    }
    deadbeef->mutex_unlock (mutex);

    memset (&db, 0, sizeof (db));
}

// This should be called only on pre-existing ml playlist.
// Subsequent indexing should be done on the fly, using fileadd listener.
static void
ml_index (void) {
    ml_free_db();

    fprintf (stderr, "building index...\n");

    struct timeval tm1, tm2;
    gettimeofday (&tm1, NULL);

    ml_entry_t *tail = NULL;

    char folder[PATH_MAX];

    DB_playItem_t *it = deadbeef->plt_get_first (ml_playlist, PL_MAIN);
    while (it && !scanner_terminate) {
        ml_entry_t *en = calloc (sizeof (ml_entry_t), 1);

        const char *uri = deadbeef->pl_find_meta (it, ":URI");
        const char *title = deadbeef->pl_find_meta (it, "title");
        const char *artist = deadbeef->pl_find_meta (it, "artist");

        // FIXME: album needs to be a combination of album + artist for indexing / library
        const char *album = deadbeef->pl_find_meta (it, "album");
        const char *genre = deadbeef->pl_find_meta (it, "genre");

        deadbeef->mutex_lock (mutex);
        ml_string_t *alb = ml_reg_col (&db.albums, album, it);
        ml_string_t *art = ml_reg_col (&db.artists, artist, it);
        ml_string_t *gnr = ml_reg_col (&db.genres, genre, it);

        char *fn = strrchr (uri, '/');
        ml_string_t *fld = NULL;
        if (fn) {
            memcpy (folder, uri, fn-uri);
            folder[fn-uri] = 0;
            const char *s = deadbeef->metacache_add_string (folder);
            fld = ml_reg_col (&db.folders, s, it);
            deadbeef->metacache_remove_string (s);
        }

        // uri and title are not indexed, only a part of track list,
        // that's why they have an extra ref for each entry
        deadbeef->metacache_add_string (uri);
        en->file = uri;
        if (title) {
            deadbeef->metacache_add_string (title);
        }
        if (deadbeef->pl_get_item_flags (it) & DDB_IS_SUBTRACK) {
            en->subtrack = deadbeef->pl_find_meta_int (it, ":TRACKNUM", -1);
        }
        else {
            en->subtrack = -1;
        }
        en->title = title;
        en->artist = art;
        en->album = alb;
        en->genre = gnr;
        en->folder = fld;

        if (tail) {
            tail->next = en;
            tail = en;
        }
        else {
            tail = db.tracks = en;
        }

        // add to the hash table
        // at this point, we only have unique pointers, and don't need a duplicate check
        uint32_t hash = hash_for_ptr ((void *)en->file);
        en->bucket_next = db.filename_hash[hash];
        db.filename_hash[hash] = en;
        deadbeef->mutex_unlock (mutex);

        DB_playItem_t *next = deadbeef->pl_get_next (it, PL_MAIN);
        deadbeef->pl_item_unref (it);
        it = next;
    }

    int nalb = 0;
    int nart = 0;
    int ngnr = 0;
    int nfld = 0;
    ml_string_t *s;
    for (s = db.albums.head; s; s = s->next, nalb++);
    for (s = db.artists.head; s; s = s->next, nart++);
    for (s = db.genres.head; s; s = s->next, ngnr++);
    for (s = db.folders.head; s; s = s->next, nfld++);
    gettimeofday (&tm2, NULL);
    long ms = (tm2.tv_sec*1000+tm2.tv_usec/1000) - (tm1.tv_sec*1000+tm1.tv_usec/1000);

    fprintf (stderr, "index build time: %f seconds (%d albums, %d artists, %d genres, %d folders)\n", ms / 1000.f, nalb, nart, ngnr, nfld);
}

static void
ml_notify_listeners (int event) {
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (listeners[i]) {
            listeners[i] (event, listeners_ud[i]);
        }
    }
}

static void
scanner_thread (void *none) {
    char plpath[PATH_MAX];
    snprintf (plpath, sizeof (plpath), "%s/medialib.dbpl", deadbeef->get_system_dir (DDB_SYS_DIR_CONFIG));

    struct timeval tm1, tm2;

    if (!ml_playlist) {
        ml_playlist = deadbeef->plt_alloc ("medialib");

        printf ("loading %s\n", plpath);
        gettimeofday (&tm1, NULL);
        DB_playItem_t *plt_head = deadbeef->plt_load2 (-1, ml_playlist, NULL, plpath, NULL, NULL, NULL);
        gettimeofday (&tm2, NULL);
        long ms = (tm2.tv_sec*1000+tm2.tv_usec/1000) - (tm1.tv_sec*1000+tm1.tv_usec/1000);
        fprintf (stderr, "ml playlist load time: %f seconds\n", ms / 1000.f);

        if (plt_head) {
            ml_index ();
            ml_notify_listeners (DDB_MEDIALIB_EVENT_CHANGED);
        }
    }

    gettimeofday (&tm1, NULL);

    const char *musicdir = deadbeef->conf_get_str_fast ("medialib.path", NULL);
    if (!musicdir) {
        return;
    }

    printf ("adding dir: %s\n", musicdir);
    deadbeef->plt_clear (ml_playlist);
    plt_insert_dir (ml_playlist, NULL, musicdir, &scanner_terminate, add_file_info_cb, NULL);
    ml_index ();
    ml_notify_listeners (DDB_MEDIALIB_EVENT_CHANGED);

    gettimeofday (&tm2, NULL);
    long ms = (tm2.tv_sec*1000+tm2.tv_usec/1000) - (tm1.tv_sec*1000+tm1.tv_usec/1000);
    fprintf (stderr, "scan time: %f seconds (%d tracks)\n", ms / 1000.f, deadbeef->plt_get_item_count (ml_playlist, PL_MAIN));

    deadbeef->plt_save (ml_playlist, NULL, NULL, plpath, NULL, NULL, NULL);
}

//#define FILTER_PERF

// intention is to skip the files which are already indexed
// how to speed this up:
// first check if a folder exists (early out?)
static int
ml_fileadd_filter (ddb_file_found_data_t *data, void *user_data) {
    int res = 0;

    if (data->plt != ml_playlist || data->is_dir) {
        return 0;
    }

#if FILTER_PERF
    struct timeval tm1, tm2;
    gettimeofday (&tm1, NULL);
#endif

    const char *s = deadbeef->metacache_get_string (data->filename);
    if (!s) {
        return 0;
    }

    uint32_t hash = (((uint32_t)(s))>>1) & (ML_HASH_SIZE-1);

    if (!db.filename_hash[hash]) {
        return 0;
    }

    ml_entry_t *en = db.filename_hash[hash];
    while (en) {
        if (en->file == s) {
            res = -1;
            break;
        }
        en = en->bucket_next;
    }

#if FILTER_PERF
    gettimeofday (&tm2, NULL);
    long ms = (tm2.tv_sec*1000+tm2.tv_usec/1000) - (tm1.tv_sec*1000+tm1.tv_usec/1000);

    if (!res) {
        fprintf (stderr, "ADD %s: file presence check took %f sec\n", s, ms / 1000.f);
    }
    else {
        fprintf (stderr, "SKIP %s: file presence check took %f sec\n", s, ms / 1000.f);
    }
#endif

    deadbeef->metacache_remove_string (s);

    return res;
}

static int
ml_connect (void) {
    tid = deadbeef->thread_start_low_priority (scanner_thread, NULL);

#if 0
    struct timeval tm1, tm2;
    gettimeofday (&tm1, NULL);
    scanner_thread(NULL);
    gettimeofday (&tm2, NULL);
    long ms = (tm2.tv_sec*1000+tm2.tv_usec/1000) - (tm1.tv_sec*1000+tm1.tv_usec/1000);
    fprintf (stderr, "whole ml init time: %f seconds\n", ms / 1000.f);
//    exit (0);
#endif
    return 0;
}

static int
ml_start (void) {
    mutex = deadbeef->mutex_create ();
    filter_id = deadbeef->register_fileadd_filter (ml_fileadd_filter, NULL);
    return 0;
}

static int
ml_stop (void) {
    if (tid) {
        scanner_terminate = 1;
        printf ("waiting for scanner thread to finish\n");
        deadbeef->thread_join (tid);
        printf ("scanner thread finished\n");
        tid = 0;
    }
    if (filter_id) {
        deadbeef->unregister_fileadd_filter (filter_id);
        filter_id = 0;
    }

    if (ml_playlist) {
        printf ("free medialib database\n");
        deadbeef->plt_free (ml_playlist);
    }

    if (artist_album_bc) {
        deadbeef->tf_free (artist_album_bc);
        artist_album_bc = NULL;
    }

    if (title_bc) {
        deadbeef->tf_free (title_bc);
        title_bc = NULL;
    }

    if (mutex) {
        deadbeef->mutex_free (mutex);
        mutex = 0;
    }
    printf ("medialib cleanup done\n");

    return 0;
}

static int
ml_add_listener (ddb_medialib_listener_t listener, void *user_data) {
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (!listeners[i]) {
            listeners[i] = listener;
            listeners_ud[i] = user_data;
            return i;
        }
    }
    return -1;
}

static void
ml_remove_listener (int listener_id) {
    listeners[listener_id] = NULL;
    listeners_ud[listener_id] = NULL;
}

static void
get_list_of_albums_for_item (ddb_medialib_item_t *libitem, const char *field, int field_tf) {
    if (!artist_album_bc) {
        artist_album_bc = deadbeef->tf_compile ("[%artist% - ]%album%");
    }
    if (!title_bc) {
        title_bc = deadbeef->tf_compile ("[%tracknumber%. ]%title%");
    }

    ml_string_t *album = db.albums.head;
    ddb_medialib_item_t *tail = NULL;
    char text[1024];

    char *tf = NULL;
    if (field_tf) {
        tf = deadbeef->tf_compile (field);
    }

    for (int i = 0; i < db.albums.count; i++, album = album->next) {
        if (!album->items_count) {
            continue;
        }

        ddb_medialib_item_t *album_item = NULL;
        ddb_medialib_item_t *album_tail = NULL;

        coll_item_t *album_coll_item = album->items;
        for (int j = 0; j < album->items_count; j++, album_coll_item = album_coll_item->next) {
            DB_playItem_t *it = album_coll_item->it;
            ddb_tf_context_t ctx = {
                ._size = sizeof (ddb_tf_context_t),
                .it = it,
            };

            const char *track_field = NULL;
            if (!tf) {
                track_field = deadbeef->pl_find_meta (it, field);
            }
            else {
                deadbeef->tf_eval (&ctx, tf, text, sizeof (text));
                track_field = text;
            }

            if (!track_field) {
                track_field = "";
            }

            // FIXME: strcasecmp might work better, but the parent list must use case-insensitive filtering first.
            if (!strcmp (track_field, libitem->text)) {
                if (!album_item) {
                    album_item = calloc (1, sizeof (ddb_medialib_item_t));
                    if (tail) {
                        tail->next = album_item;
                        tail = album_item;
                    }
                    else {
                        tail = libitem->children = album_item;
                    }

                    deadbeef->tf_eval (&ctx, artist_album_bc, text, sizeof (text));

                    album_item->text = deadbeef->metacache_add_string (text);
                    libitem->num_children++;
                }

                ddb_medialib_item_t *track_item = calloc(1, sizeof (ddb_medialib_item_t));

                if (album_tail) {
                    album_tail->next = track_item;
                    album_tail = track_item;
                }
                else {
                    album_tail = album_item->children = track_item;
                }
                album_item->num_children++;

                ddb_tf_context_t ctx = {
                    ._size = sizeof (ddb_tf_context_t),
                    .it = it,
                };

                deadbeef->tf_eval (&ctx, title_bc, text, sizeof (text));

                track_item->text = deadbeef->metacache_add_string (text);
            }
        }
    }
    if (tf) {
        deadbeef->tf_free (tf);
    }
}

static void
ml_free_list (ddb_medialib_item_t *list);

static ddb_medialib_item_t *
ml_get_list (const char *index) {
    collection_t *coll;

    const char *field = NULL;
    int use_tf = 0;

    if (!strcmp (index, "album")) {
        coll = &db.albums;
        field = "%album%";
        use_tf = 1;
    }
    else if (!strcmp (index, "artist")) {
        coll = &db.artists;
        field = "%artist%";
        use_tf = 1;
    }
    else if (!strcmp (index, "genre")) {
        coll = &db.genres;
        field = "genre";
    }
    else if (!strcmp (index, "folder")) {
        coll = &db.folders;
    }
    else {
        return NULL;
    }

    deadbeef->mutex_lock (mutex);

    struct timeval tm1, tm2;
    gettimeofday (&tm1, NULL);

    ddb_medialib_item_t *root = calloc (1, sizeof (ddb_medialib_item_t));
    root->text = deadbeef->metacache_add_string ("All Music");
    ddb_medialib_item_t *tail = NULL;

    // top level list (e.g. list of genres)
    ddb_medialib_item_t *parent = root;
    int idx = 0;
    for (ml_string_t *s = coll->head; s; s = s->next, idx++) {
        ddb_medialib_item_t *item = calloc (1, sizeof (ddb_medialib_item_t));

        item->text = deadbeef->metacache_add_string (s->text);

        get_list_of_albums_for_item (item, field, use_tf);

        if (!item->children) {
            ml_free_list (item);
            continue;
        }

        if (tail) {
            tail->next = item;
            tail = item;
        }
        else {
            tail = parent->children = item;
        }
        parent->num_children++;
    }

    gettimeofday (&tm2, NULL);
    long ms = (tm2.tv_sec*1000+tm2.tv_usec/1000) - (tm1.tv_sec*1000+tm1.tv_usec/1000);

    fprintf (stderr, "tree build time: %f seconds\n", ms / 1000.f);
    deadbeef->mutex_unlock (mutex);

    return root;
}

static void
ml_free_list (ddb_medialib_item_t *list) {
    if (list->children) {
        ml_free_list(list->children);
        list->children = NULL;
    }
    while (list->next) {
        ddb_medialib_item_t *item = list->next;
        ddb_medialib_item_t *next = item->next;
        if (item->text) {
            deadbeef->metacache_remove_string (item->text);
        }
        if (item->track) {
            deadbeef->pl_item_unref (item->track);
        }
        free (item);
        list->next = next;
    }
    free (list);
}

static int
ml_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    return 0;
}

// define plugin interface
static ddb_medialib_plugin_t plugin = {
    .plugin.plugin.api_vmajor = 1,
    .plugin.plugin.api_vminor = 10,
    .plugin.plugin.version_major = DDB_MEDIALIB_VERSION_MAJOR,
    .plugin.plugin.version_minor = DDB_MEDIALIB_VERSION_MINOR,
    .plugin.plugin.type = DB_PLUGIN_MISC,
    .plugin.plugin.id = "medialib",
    .plugin.plugin.name = "Media Library",
    .plugin.plugin.descr = "Scans disk for music files and manages them as database",
    .plugin.plugin.copyright = 
        "Media Library plugin for DeaDBeeF Player\n"
        "Copyright (C) 2009-2017 Alexey Yakovenko\n"
        "\n"
        "This software is provided 'as-is', without any express or implied\n"
        "warranty.  In no event will the authors be held liable for any damages\n"
        "arising from the use of this software.\n"
        "\n"
        "Permission is granted to anyone to use this software for any purpose,\n"
        "including commercial applications, and to alter it and redistribute it\n"
        "freely, subject to the following restrictions:\n"
        "\n"
        "1. The origin of this software must not be misrepresented; you must not\n"
        " claim that you wrote the original software. If you use this software\n"
        " in a product, an acknowledgment in the product documentation would be\n"
        " appreciated but is not required.\n"
        "\n"
        "2. Altered source versions must be plainly marked as such, and must not be\n"
        " misrepresented as being the original software.\n"
        "\n"
        "3. This notice may not be removed or altered from any source distribution.\n"
    ,
    .plugin.plugin.website = "http://deadbeef.sf.net",
    .plugin.plugin.connect = ml_connect,
    .plugin.plugin.start = ml_start,
    .plugin.plugin.stop = ml_stop,
//    .plugin.plugin.configdialog = settings_dlg,
    .plugin.plugin.message = ml_message,
    .add_listener = ml_add_listener,
    .remove_listener = ml_remove_listener,
    .get_list = ml_get_list,
    .free_list = ml_free_list,
};

DB_plugin_t *
medialib_load (DB_functions_t *api) {
    deadbeef = api;

    // hack: we need original function without overrides
    plt_insert_dir = deadbeef->plt_insert_dir;
    return DB_PLUGIN (&plugin);
}
