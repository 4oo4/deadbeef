//
//  StreamerTest.m
//  deadbeef
//
//  Created by Alexey Yakovenko on 5/31/17.
//  Copyright © 2017 Alexey Yakovenko. All rights reserved.
//

#import <XCTest/XCTest.h>
#include "deadbeef.h"
#include "playlist.h"
#include "plugins.h"
#include "conf.h"
#include "../../common.h"
#include "logger.h"
#include "streamer.h"
#include "threading.h"
#include "messagepump.h"

static int count_played;

static void (*_trackinfochanged_handler)(ddb_event_track_t *ev);

// super oversimplified mainloop
static void
mainloop (void) {
    for (;;) {
        uint32_t msg;
        uintptr_t ctx;
        uint32_t p1;
        uint32_t p2;
        int term = 0;
        while (messagepump_pop(&msg, &ctx, &p1, &p2) != -1) {
            if (!term) {
                DB_output_t *output = plug_get_output ();
                switch (msg) {
                    case DB_EV_TERMINATE:
                        term = 1;
                        break;
                    case DB_EV_PLAY_CURRENT:
                        streamer_play_current_track ();
                        break;
                    case DB_EV_PLAY_NUM:
                        streamer_set_nextsong (p1);
                        break;
                    case DB_EV_STOP:
                        streamer_set_nextsong (-1);
                        break;
                    case DB_EV_NEXT:
                        streamer_move_to_nextsong (1);
                        break;
                    case DB_EV_PREV:
                        streamer_move_to_prevsong (1);
                        break;
                    case DB_EV_PAUSE:
                        if (output->state () != OUTPUT_STATE_PAUSED) {
                            output->pause ();
                            messagepump_push (DB_EV_PAUSED, 0, 1, 0);
                        }
                        break;
                    case DB_EV_TOGGLE_PAUSE:
                        if (output->state () != OUTPUT_STATE_PLAYING) {
                            streamer_play_current_track ();
                        }
                        else {
                            output->pause ();
                            messagepump_push (DB_EV_PAUSED, 0, 1, 0);
                        }
                        break;
                    case DB_EV_PLAY_RANDOM:
                        streamer_move_to_randomsong (1);
                        break;
                    case DB_EV_SEEK:
                    {
                        int32_t pos = (int32_t)p1;
                        if (pos < 0) {
                            pos = 0;
                        }
                        streamer_set_seek (p1 / 1000.f);
                    }
                        break;
                    case DB_EV_SONGSTARTED:
                        count_played++;
                        break;
                    case DB_EV_TRACKINFOCHANGED:
                        if (_trackinfochanged_handler) {
                            _trackinfochanged_handler ((ddb_event_track_t *)ctx);
                        }
                        break;
                }
            }
            if (msg >= DB_EV_FIRST && ctx) {
                messagepump_event_free ((ddb_event_t *)ctx);
            }
        }
        if (term) {
            return;
        }
        messagepump_wait ();
    }
}

void
wait_until_stopped (void) {
    // wait until finished!
    BOOL finished = NO;

    while (!finished) {
        playItem_t *streaming_track = streamer_get_streaming_track();
        playItem_t *playing_track = streamer_get_playing_track();
        if (!streaming_track && !playing_track) {
            finished = YES;
        }
        if (streaming_track) {
            pl_item_unref (streaming_track);
        }
        if (playing_track) {
            pl_item_unref(playing_track);
        }
    }
}

@interface StreamerTest : XCTestCase {
    DB_plugin_t *_fakein;
    DB_output_t *_fakeout;
    uintptr_t _mainloop_tid;
}

@end

@implementation StreamerTest

- (void)setUp {
    [super setUp];

    // init deadbeef core
    NSString *resPath = [[NSBundle bundleForClass:[self class]] resourcePath];
    const char *str = [resPath UTF8String];
    strcpy (dbplugindir, str);

    ddb_logger_init ();
    conf_init ();
    conf_enable_saving (0);

    pl_init ();

    messagepump_init ();

    // register fakein and fakeout plugin
    extern DB_plugin_t * fakein_load (DB_functions_t *api);
    extern DB_plugin_t * fakeout_load (DB_functions_t *api);

    plug_init_plugin (fakein_load, NULL);
    plug_init_plugin (fakeout_load, NULL);

    _fakein = fakein_load (plug_get_api ());
    _fakeout = (DB_output_t *)fakeout_load (plug_get_api ());
    plug_register_in (_fakein);
    plug_register_out ((DB_plugin_t *)_fakeout);

    plug_set_output (_fakeout);

    streamer_init ();

    conf_set_int ("playback.loop", PLAYBACK_MODE_NOLOOP);
    count_played = 0;

    _mainloop_tid = thread_start (mainloop, NULL);
}

- (void)tearDown {
    deadbeef->sendmessage (DB_EV_TERMINATE, 0, 0, 0);
    thread_join (_mainloop_tid);

    _fakeout->stop ();
    streamer_free ();

    plug_disconnect_all ();
    plug_unload_all ();
    pl_free ();
    conf_free ();
    ddb_logger_free ();

    [super tearDown];
}

- (void)test_Play2TracksNoLoop_Sends2SongChanged {
    ddb_playlist_t *plt = deadbeef->plt_get_curr ();
    // create two test fake tracks
    DB_playItem_t *_sinewave = deadbeef->plt_insert_file2 (0, plt, NULL, "sine.fake", NULL, NULL, NULL);
    DB_playItem_t *_squarewave = deadbeef->plt_insert_file2 (0, plt, _sinewave, "square.fake", NULL, NULL, NULL);

    streamer_set_nextsong (0);
    streamer_yield ();

    wait_until_stopped ();

    deadbeef->pl_item_unref (_sinewave);
    deadbeef->pl_item_unref (_squarewave);
    deadbeef->plt_unref (plt);
    XCTAssert (count_played = 2);
}

// This test is a complicated
// Given two tracks A and B
// Start track A
// Start track B
// Monitor trackinfochanged events, and make sure that track A is never in "playing" state after track B started "buffering"

static DB_playItem_t *switchtest_tracks[2];
static int switchtest_counts[2];
static void switchtest_trackinfochanged_handler (ddb_event_track_t *ev) {
    if (deadbeef->streamer_ok_to_read (-1)) {
        DB_playItem_t *playing = deadbeef->streamer_get_playing_track ();
        if (ev->track == switchtest_tracks[0] && playing == ev->track) {
            printf ("track A playing!\n");
            switchtest_counts[0]++;
        }
        else if (ev->track == switchtest_tracks[1] && playing == ev->track) {
            switchtest_counts[1]++;
        }
        if (playing) {
            deadbeef->pl_item_unref (playing);
        }
    }
}

- (void)test_SwitchBetweenTracks_DoesNotJumpBackToPrevious {
    // for this test, we want "loop single" mode, to make sure first track is playing when we start the 2nd one.
    conf_set_int ("playback.loop", PLAYBACK_MODE_LOOP_SINGLE);

    ddb_playlist_t *plt = deadbeef->plt_get_curr ();
    // create two test fake tracks
    switchtest_tracks[0] = deadbeef->plt_insert_file2 (0, plt, NULL, "sine.fake", NULL, NULL, NULL);
    switchtest_tracks[1] = deadbeef->plt_insert_file2 (0, plt, switchtest_tracks[0], "square.fake", NULL, NULL, NULL);

    switchtest_counts[0] = switchtest_counts[1] = 0;

    printf ("start track A...\n");
    streamer_set_nextsong (0);
    streamer_yield ();

    printf ("wait...\n");
    // FIXME: sleeping is not predictable, should instead make fakeout plugin consume samples
    // E.g.: fakeout_setmanual (1); ... fakeout_consume (4096);
    usleep (1000000);
    printf ("start track B...\n");
    streamer_set_nextsong (1);
    streamer_yield ();

    _trackinfochanged_handler = switchtest_trackinfochanged_handler;

    printf ("wait...\n");
    usleep (1000000);

    deadbeef->pl_item_unref (switchtest_tracks[0]);
    deadbeef->pl_item_unref (switchtest_tracks[1]);
    deadbeef->plt_unref (plt);
    _trackinfochanged_handler = NULL;
    XCTAssert (switchtest_counts[0] == 0);
    XCTAssert (count_played = 2);
}


@end
