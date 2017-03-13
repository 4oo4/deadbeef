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

#ifndef medialib_h
#define medialib_h

#define DDB_MEDIALIB_VERSION_MAJOR 1
#define DDB_MEDIALIB_VERSION_MINOR 0

typedef struct ddb_medialib_item_s {
    const char *text; // e.g. the genre

    DB_playItem_t *track; // NULL in non-leaf nodes

    struct ddb_medialib_item_s *next;
    struct ddb_medialib_item_s *children;
    int num_children;
} ddb_medialib_item_t;

enum {
    DDB_MEDIALIB_EVENT_CHANGED = 1
};

typedef void (* ddb_medialib_listener_t)(int event, void *user_data);

typedef struct ddb_medialib_plugin_s {
    DB_misc_t plugin;

    int (*add_listener)(ddb_medialib_listener_t listener, void *user_data);
    void (*remove_listener)(int listener_id);

    ddb_medialib_item_t * (*get_list)(const char *index);
    void (*free_list) (ddb_medialib_item_t *list);
} ddb_medialib_plugin_t;

#endif /* medialib_h */
