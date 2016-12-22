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
#include <stdio.h>
#include <unistd.h>
#include "aacdecoder_lib.h"
#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include "../../deadbeef.h"
#include "aac_parser.h"

#include "mp4ff.h"

#include "../../shared/mp4tagutil.h"

#define min(x,y) ((x)<(y)?(x):(y))
#define max(x,y) ((x)>(y)?(x):(y))

#define trace(...) { deadbeef->log_detailed (&plugin.plugin, 0, __VA_ARGS__); }

static DB_decoder_t plugin;
DB_functions_t *deadbeef;

#define AAC_BUFFER_SIZE (1024 * 16) // FIXME: 1024 is wrong
#define OUT_BUFFER_SIZE 100000

#define MP4FILE mp4ff_t *
#define MP4FILE_CB mp4ff_callback_t


// aac channel mapping
// 0: Defined in AOT Specifc Config
// 1: 1 channel: front-center
// 2: 2 channels: front-left, front-right
// 3: 3 channels: front-center, front-left, front-right
// 4: 4 channels: front-center, front-left, front-right, back-center
// 5: 5 channels: front-center, front-left, front-right, back-left, back-right
// 6: 6 channels: front-center, front-left, front-right, back-left, back-right, LFE-channel
// 7: 8 channels: front-center, front-left, front-right, side-left, side-right, back-left, back-right, LFE-channel
// 8-15: Reserved


typedef struct {
    DB_fileinfo_t info;
    HANDLE_AACDECODER dec;
    DB_FILE *file;
    MP4FILE mp4;
    MP4FILE_CB mp4reader;
    CStreamInfo *frame_info; // last frame info, used for channel mapping
    int mp4track;
    int mp4samples;
    int mp4sample;
    int mp4framesize;
    int skipsamples;
    int startsample;
    int endsample;
    int currentsample;
    uint8_t buffer[AAC_BUFFER_SIZE];
    int remaining;
    uint8_t out_buffer[OUT_BUFFER_SIZE];
    int out_remaining;
    int num_errors;
    char *samplebuffer;
    int remap[10];
    int noremap;
    int eof;
    int junk;
} aac_info_t;

// allocate codec control structure
static DB_fileinfo_t *
aac_open (uint32_t hints) {
    DB_fileinfo_t *_info = malloc (sizeof (aac_info_t));
    aac_info_t *info = (aac_info_t *)_info;
    memset (info, 0, sizeof (aac_info_t));
    return _info;
}

static uint32_t
aac_fs_read (void *user_data, void *buffer, uint32_t length) {
    aac_info_t *info = user_data;
    return (uint32_t)deadbeef->fread (buffer, 1, length, info->file);
}
static uint32_t
aac_fs_seek (void *user_data, uint64_t position) {
    aac_info_t *info = user_data;
    return deadbeef->fseek (info->file, position+info->junk, SEEK_SET);
}


static int64_t
parse_aac_stream(DB_FILE *fp, int *psamplerate, int *pchannels, float *pduration, int64_t *ptotalsamples)
{
    size_t framepos = deadbeef->ftell (fp);
    int64_t firstframepos = -1;
    int64_t fsize = -1;
    int offs = 0;
    if (!fp->vfs->is_streaming ()) {
        int skip = deadbeef->junk_get_leading_size (fp);
        if (skip >= 0) {
            deadbeef->fseek (fp, skip, SEEK_SET);
        }
        fsize = deadbeef->fgetlength (fp);
        if (skip > 0) {
            fsize -= skip;
        }
    }

    uint8_t buf[ADTS_HEADER_SIZE*8];

    int nsamples = 0;
    int stream_sr = 0;
    int stream_ch = 0;

    int bufsize = 0;

    int frame = 0;
    int scanframes = 1000;
    if (fp->vfs->is_streaming ()) {
        scanframes = 1;
    }

    do {
        int size = sizeof (buf) - bufsize;
        if (deadbeef->fread (buf + bufsize, 1, size, fp) != size) {
            break;
        }
        bufsize = sizeof (buf);

        int channels, samplerate, bitrate, samples;
        size = aac_sync (buf, &channels, &samplerate, &bitrate, &samples);
        if (size == 0) {
            memmove (buf, buf+1, sizeof (buf)-1);
            bufsize--;
            framepos++;
            continue;
        }
        else {
            frame++;
            nsamples += samples;
            if (!stream_sr) {
                stream_sr = samplerate;
            }
            if (!stream_ch) {
                stream_ch = channels;
            }
            if (firstframepos == -1) {
                firstframepos = framepos;
            }
//            if (fp->vfs->streaming) {
//                *psamplerate = stream_sr;
//                *pchannels = stream_ch;
//            }
            framepos += size;
            if (deadbeef->fseek (fp, size-(int)sizeof(buf), SEEK_CUR) == -1) {
                break;
            }
            bufsize = 0;
        }
    } while (ptotalsamples || frame < scanframes);

    if (!frame || !stream_sr || !nsamples) {
        return -1;
    }

    *psamplerate = stream_sr;

    *pchannels = stream_ch;

    if (ptotalsamples) {
        *ptotalsamples = nsamples;
        *pduration = nsamples / (float)stream_sr;
    }
    else {
        int64_t pos = deadbeef->ftell (fp);
        int totalsamples = (double)fsize / (pos-offs) * nsamples;
        *pduration = totalsamples / (float)stream_sr;
    }

    if (*psamplerate <= 24000) {
        *psamplerate *= 2;
        if (ptotalsamples) {
            *ptotalsamples *= 2;
        }
    }
    return firstframepos;
}

static int
mp4_track_get_info(mp4ff_t *mp4, int track, float *duration, int *samplerate, int *channels, int64_t *totalsamples, int *mp4framesize) {
    int sr = -1;
    int ch = -1;
    unsigned char*  buff = 0;
    unsigned int    buff_size = 0;
    mp4ff_get_decoder_config(mp4, track, &buff, &buff_size);
    if (buff) {
        //int objectTypeIndex = (buff[0]&0xf8)>>3; // bits 0..4
        int samplerate_index = ((buff[0]&0x07)<<1) | ((buff[1]&0x80)>>7); // bits 5..8
        if (samplerate_index >= 12) {
            free (buff);
            return -1; // invalid format
        }
        if (samplerate_index == 0x0f) {
            // skip 24 bits
        }

        static const int samplerates[] = {
            96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000
        };

        sr = samplerates[samplerate_index];

        // bits 9..12
        ch = (buff[1]&0x78)>>3;
    }

    int samples;

    // init mp4 decoding
    HANDLE_AACDECODER dec = aacDecoder_Open(TT_MP4_RAW, 1);
    if (aacDecoder_ConfigRaw(dec, &buff, &buff_size) != AAC_DEC_OK) {
        goto error;
    }
    *samplerate = sr;
    *channels = ch;
    samples = mp4ff_num_samples(mp4, track);
    
    aacDecoder_Close (dec);
    dec = NULL;

    if (samples <= 0) {
        goto error;
    }

    int i_sample_count = samples;
    int i_sample;

    int64_t total_dur = 0;
    for( i_sample = 0; i_sample < i_sample_count; i_sample++ )
    {
        total_dur += mp4ff_get_sample_duration (mp4, track, i_sample);
    }
    if (totalsamples) {
        *totalsamples = total_dur * (*samplerate) / mp4ff_time_scale (mp4, track);
        *mp4framesize = (int)((*totalsamples) / i_sample_count);
    }
    *duration = total_dur / (float)mp4ff_time_scale (mp4, track);

    return 0;
error:
    if (dec) {
        aacDecoder_Close (dec);
    }
    free (buff);
    return -1;
}

// returns -1 for error, 0 for aac
static int
aac_probe (DB_FILE *fp, float *duration, int *samplerate, int *channels, int64_t *totalsamples) {

    deadbeef->rewind (fp);
    if (parse_aac_stream (fp, samplerate, channels, duration, totalsamples) == -1) {
        return -1;
    }
    return 0;
}


static int
aac_init (DB_fileinfo_t *_info, DB_playItem_t *it) {
    aac_info_t *info = (aac_info_t *)_info;

    deadbeef->pl_lock ();
    info->file = deadbeef->fopen (deadbeef->pl_find_meta (it, ":URI"));
    deadbeef->pl_unlock ();
    if (!info->file) {
        return -1;
    }

    // probe
    float duration = -1;
    int samplerate = -1;
    int channels = -1;
    int64_t totalsamples = -1;

    if (!info->file->vfs->is_streaming ()) {
        info->junk = deadbeef->junk_get_leading_size (info->file);
        if (info->junk >= 0) {
            deadbeef->fseek (info->file, info->junk, SEEK_SET);
        }
        else {
            info->junk = 0;
        }
    }
    else {
        deadbeef->fset_track (info->file, it);
    }

    info->mp4track = -1;
    info->mp4reader.read = aac_fs_read;
    info->mp4reader.write = NULL;
    info->mp4reader.seek = aac_fs_seek;
    info->mp4reader.truncate = NULL;
    info->mp4reader.user_data = info;

    info->mp4 = mp4ff_open_read (&info->mp4reader);
    if (info->mp4) {
        int ntracks = mp4ff_total_tracks (info->mp4);
        for (int i = 0; i < ntracks; i++) {
            if (mp4ff_get_track_type (info->mp4, i) != TRACK_AUDIO) {
                continue;
            }
            int res = mp4_track_get_info (info->mp4, i, &duration, &samplerate, &channels, &totalsamples, &info->mp4framesize);
            if (res >= 0 && duration > 0) {
                info->mp4track = i;
                break;
            }
        }
        if (info->mp4track >= 0) {
            // init mp4 decoding
            info->mp4samples = mp4ff_num_samples(info->mp4, info->mp4track);
            info->dec = aacDecoder_Open (TT_MP4_RAW, 1);
            unsigned char*  buff = 0;
            unsigned int    buff_size = 0;
            mp4ff_get_decoder_config (info->mp4, info->mp4track, &buff, &buff_size);
            if (aacDecoder_ConfigRaw(info->dec, &buff, &buff_size) != AAC_DEC_OK) {
                free (buff);
                return -1;
            }

            if (buff) {
                free (buff);
            }
            _info->fmt.samplerate = samplerate;
            _info->fmt.channels = channels;
        }
        else {
            mp4ff_close (info->mp4);
            info->mp4 = NULL;
        }
    }

    if (!info->mp4) {
        int64_t offs;
        if (!info->file->vfs->is_streaming ()) {
            if (info->junk >= 0) {
                deadbeef->fseek (info->file, info->junk, SEEK_SET);
            }
            else {
                deadbeef->rewind (info->file);
            }
            offs = parse_aac_stream (info->file, &samplerate, &channels, &duration, &totalsamples);
        }
        else {
            deadbeef->rewind (info->file);
            offs = 0;
        }
        if (offs == -1) {
            return -1;
        }
        if (offs > info->junk) {
            info->junk = (int)offs;
        }
        if (!info->file->vfs->is_streaming ()) {
            if (info->junk >= 0) {
                deadbeef->fseek (info->file, info->junk, SEEK_SET);
            }
            else {
                deadbeef->rewind (info->file);
            }
        }
        if (info->file->vfs->is_streaming ()) {
            deadbeef->pl_replace_meta (it, "!FILETYPE", "AAC");
        }

        _info->fmt.channels = channels;
        _info->fmt.samplerate = samplerate;

        info->dec = aacDecoder_Open(TT_MP4_RAW, 1);


        int scan_size = AAC_BUFFER_SIZE;

        while (scan_size > 0) {
            info->remaining = (int)deadbeef->fread (info->buffer, 1, AAC_BUFFER_SIZE, info->file);
            uint8_t *p = info->buffer;

            // sync the initial buffer
            unsigned long srate;
            unsigned char ch;
            long consumed = 0;
            // FIXME: raw aac untested
            while (p < info->buffer+info->remaining) {
                UINT bufsize = info->remaining-(int)(p-info->buffer);
                UINT valid = bufsize;
                AAC_DECODER_ERROR err = aacDecoder_Fill (info->dec, &p, &bufsize, &valid);
                if (err == AAC_DEC_OK && valid < bufsize) {
                    _info->fmt.channels = ch;
                    _info->fmt.samplerate = (int)srate;
                    break;
                }
                p++;
            }
            if (consumed >= 0) {
                if (consumed != info->remaining && consumed > 0) {
                    memmove (info->buffer, info->buffer + consumed, info->remaining - consumed);
                }
                info->remaining -= consumed;
                break;
            }

            scan_size -= info->remaining;
        }

        if (scan_size <= 0) {
            return -1;
        }
    }

    _info->fmt.bps = 16;
    _info->plugin = &plugin;

    if (!info->file->vfs->is_streaming ()) {
        if (it->endsample > 0) {
            info->startsample = it->startsample;
            info->endsample = it->endsample;
            plugin.seek_sample (_info, 0);
        }
        else {
            info->startsample = 0;
            info->endsample = (int)totalsamples-1;
        }
    }
    if (_info->fmt.channels == 7) {
        _info->fmt.channels = 8;
    }

    for (int i = 0; i < _info->fmt.channels; i++) {
        _info->fmt.channelmask |= 1 << i;
    }
    info->noremap = 0;
    for (int i = 0; i < sizeof (info->remap) / sizeof (int); i++) {
        info->remap[i] = -1;
    }

    return 0;
}

static void
aac_free (DB_fileinfo_t *_info) {
    aac_info_t *info = (aac_info_t *)_info;
    if (info) {
        if (info->file) {
            deadbeef->fclose (info->file);
        }
        if (info->mp4) {
            mp4ff_close (info->mp4);
        }
        if (info->dec) {
            aacDecoder_Close (info->dec);
        }
        free (info);
    }
}

static int
aac_read (DB_fileinfo_t *_info, char *bytes, int size) {
    aac_info_t *info = (aac_info_t *)_info;
    if (info->eof) {
        return 0;
    }

    int samplesize = _info->fmt.channels * _info->fmt.bps / 8;
    if (!info->file->vfs->is_streaming ()) {
        if (info->currentsample + size / samplesize > info->endsample) {
            size = (info->endsample - info->currentsample + 1) * samplesize;
            if (size <= 0) {
                return 0;
            }
        }
    }

    int initsize = size;

    while (size > 0) {
        if (info->skipsamples > 0 && info->out_remaining > 0) {
            int skip = min (info->out_remaining, info->skipsamples);
            if (skip < info->out_remaining) {
                memmove (info->out_buffer, info->out_buffer + skip * samplesize, (info->out_remaining - skip) * samplesize);
            }
            info->out_remaining -= skip;
            info->skipsamples -= skip;
        }
        if (info->out_remaining > 0) {
            int n = size / samplesize;
            n = min (info->out_remaining, n);

            uint8_t *src = info->out_buffer;
            if (info->noremap) {
                memcpy (bytes, src, n * samplesize);
                bytes += n * samplesize;
                src += n * samplesize;
            }
            else {
                int i, j;

                if (info->remap[0] == -1) {
                    // build remap mtx
                    for (i = 0; i < _info->fmt.channels; i++) {
                        AUDIO_CHANNEL_TYPE idx = info->frame_info->pChannelIndices[i];
                        info->remap[idx] = i;
                    }
                    if (info->remap[0] == -1) {
                        info->remap[0] = 0;
                    }
                    if ((_info->fmt.channels == 1 && info->remap[0] == 0)
                        || (_info->fmt.channels == 2 && info->remap[0] == 0 && info->remap[1] == 1)) {
                        info->noremap = 1;
                    }
                }

                for (i = 0; i < n; i++) {
                    for (j = 0; j < _info->fmt.channels; j++) {
                        if (info->remap[j] == -1) {
                            ((int16_t *)bytes)[j] = 0;
                        }
                        else {
                            ((int16_t *)bytes)[j] = ((int16_t *)src)[info->remap[j]];
                        }
                    }
                    src += samplesize;
                    bytes += samplesize;
                }
            }
            size -= n * samplesize;

            if (n == info->out_remaining) {
                info->out_remaining = 0;
            }
            else {
                memmove (info->out_buffer, src, (info->out_remaining - n) * samplesize);
                info->out_remaining -= n;
            }
            continue;
        }

        char *samples = NULL;

        if (info->mp4) {
            if (info->mp4sample >= info->mp4samples) {
                break;
            }
            
            unsigned char *buffer = NULL;
            uint32_t buffer_size = 0;
            int rc = mp4ff_read_sample (info->mp4, info->mp4track, info->mp4sample, &buffer, &buffer_size);
            if (rc == 0) {
                info->eof = 1;
                break;
            }
            info->mp4sample++;

            samples = malloc (8*2*1024);

            UINT consumed = buffer_size;
            AAC_DECODER_ERROR err = aacDecoder_Fill(info->dec, &buffer, &buffer_size, &consumed);
            err = aacDecoder_DecodeFrame(info->dec, (short *)samples, 8*2*1024, 0);

            if (buffer) {
                free (buffer);
            }
            if (!samples) {
                break;
            }
        }
        else {
            if (info->remaining < AAC_BUFFER_SIZE) {
                size_t res = deadbeef->fread (info->buffer + info->remaining, 1, AAC_BUFFER_SIZE-info->remaining, info->file);
                info->remaining += res;
                if (!info->remaining) {
                    break;
                }
            }

            samples = malloc (8*2*1024);

            UINT bytesValid = info->remaining;
            aacDecoder_Fill(info->dec, (uint8_t **)&info->buffer, (UINT *)&info->remaining, &bytesValid);
            aacDecoder_DecodeFrame(info->dec, (short *)samples, 8*2*1024, 0);

            if (!samples) {
                if (info->num_errors > 10) {
                    break;
                }
                info->num_errors++;
                info->remaining = 0;
                continue;
            }
            info->num_errors=0;
            unsigned long consumed = bytesValid;
            if (consumed > info->remaining) {
                break;
            }
            if (consumed == info->remaining) {
                info->remaining = 0;
            }
            else if (consumed > 0) {
                memmove (info->buffer, info->buffer + consumed, info->remaining - consumed);
                info->remaining -= consumed;
            }
        }

        CStreamInfo *stream_info = aacDecoder_GetStreamInfo(info->dec);
        info->frame_info = stream_info;
        int frame_size = stream_info->frameSize * stream_info->numChannels;
        if (frame_size > 0) {
            memcpy (info->out_buffer, samples, frame_size * 2);
            info->out_remaining = (int)stream_info->frameSize;
        }
    }

    info->currentsample += (initsize-size) / samplesize;

    return initsize-size;
}

// returns -1 on error, 0 on success
int
seek_raw_aac (aac_info_t *info, int sample) {
    uint8_t buf[ADTS_HEADER_SIZE*8];

    int bufsize = 0;

    int frame = 0;

    int frame_samples = 0;
    int curr_sample = 0;

    do {
        curr_sample += frame_samples;
        int size = sizeof (buf) - bufsize;
        if (deadbeef->fread (buf + bufsize, 1, size, info->file) != size) {
            break;
        }
        bufsize = sizeof (buf);

        int channels, samplerate, bitrate;
        size = aac_sync (buf, &channels, &samplerate, &bitrate, &frame_samples);
        if (size == 0) {
            memmove (buf, buf+1, sizeof (buf)-1);
            bufsize--;
            continue;
        }
        else {
            frame++;
            if (deadbeef->fseek (info->file, size-(int)sizeof(buf), SEEK_CUR) == -1) {
                break;
            }
            bufsize = 0;
        }
        if (samplerate <= 24000) {
            frame_samples *= 2;
        }
    } while (curr_sample + frame_samples < sample);

    if (curr_sample + frame_samples < sample) {
        return -1;
    }

    return sample - curr_sample;
}

static int
aac_seek_sample (DB_fileinfo_t *_info, int sample) {
    aac_info_t *info = (aac_info_t *)_info;

    sample += info->startsample;
    if (info->mp4) {
        int totalsamples = 0;
        int i;
        int num_sample_byte_sizes = mp4ff_get_num_sample_byte_sizes (info->mp4, info->mp4track);
        int scale = _info->fmt.samplerate / mp4ff_time_scale (info->mp4, info->mp4track);
        for (i = 0; i < num_sample_byte_sizes; i++)
        {
            unsigned int thissample_duration = 0;
            unsigned int thissample_bytesize = 0;

            mp4ff_get_sample_info(info->mp4, info->mp4track, i, &thissample_duration,
                    &thissample_bytesize);

            if (totalsamples + thissample_duration > sample / scale) {
                info->skipsamples = sample - totalsamples * scale;
                break;
            }
            totalsamples += thissample_duration;
        }
//        i = sample / info->mp4framesize;
//        info->skipsamples = sample - info->mp4sample * info->mp4framesize;
        info->mp4sample = i;
    }
    else {
        int skip = deadbeef->junk_get_leading_size (info->file);
        if (skip >= 0) {
            deadbeef->fseek (info->file, skip, SEEK_SET);
        }
        else {
            deadbeef->fseek (info->file, 0, SEEK_SET);
        }

        int res = seek_raw_aac (info, sample);
        if (res < 0) {
            return -1;
        }
        info->skipsamples = res;
    }
    info->remaining = 0;
    info->out_remaining = 0;
    info->currentsample = sample;
    _info->readpos = (float)(info->currentsample - info->startsample) / _info->fmt.samplerate;
    return 0;
}

static int
aac_seek (DB_fileinfo_t *_info, float t) {
    return aac_seek_sample (_info, t * _info->fmt.samplerate);
}

typedef struct {
    char *title;
    int32_t startsample;
    int32_t endsample;
} aac_chapter_t;

static aac_chapter_t *
aac_load_itunes_chapters (mp4ff_t *mp4, /* out */ int *num_chapters, int samplerate) {
    *num_chapters = 0;
    int i_entry_count = mp4ff_chap_get_num_tracks (mp4);
    int i_tracks = mp4ff_total_tracks (mp4);
    int i, j;
    for( i = 0; i < i_entry_count; i++ )
    {
        for( j = 0; j < i_tracks; j++ )
        {
            if(mp4ff_chap_get_track_id (mp4, i)  == mp4ff_get_track_id (mp4, j) &&
                    mp4ff_get_track_type (mp4, j) == TRACK_TEXT) {
                break;
            }
        }
        if( j < i_tracks )
        {
            int i_sample_count = mp4ff_num_samples (mp4, j);
            int i_sample;

            aac_chapter_t *chapters = malloc (sizeof (aac_chapter_t) * i_sample_count);
            memset (chapters, 0, sizeof (aac_chapter_t) * i_sample_count);
            *num_chapters = 0;

            int64_t total_dur = 0;
            int64_t curr_sample = 0;
            for( i_sample = 0; i_sample < i_sample_count; i_sample++ )
            {
                int32_t dur = (int64_t)1000 * mp4ff_get_sample_duration (mp4, j, i_sample) / mp4ff_time_scale (mp4, j); // milliseconds
                total_dur += dur;
                unsigned char *buffer = NULL;
                uint32_t buffer_size = 0;

                int rc = mp4ff_read_sample (mp4, j, i_sample, &buffer, &buffer_size);

                if (rc == 0 || !buffer) {
                    continue;
                }
                int len = (buffer[0] << 8) | buffer[1];
                len = min (len, buffer_size - 2);
                if (len > 0) {
                    chapters[*num_chapters].title = strndup ((const char *)&buffer[2], len);
                }
                chapters[*num_chapters].startsample = (int)curr_sample;
                curr_sample += (int64_t)dur * samplerate / 1000.f;
                chapters[*num_chapters].endsample = (int)curr_sample - 1;
                if (buffer) {
                    free (buffer);
                }
                (*num_chapters)++;
            }
            return chapters;
        }
    }
    return NULL;
}

static DB_playItem_t *
aac_insert_with_chapters (ddb_playlist_t *plt, DB_playItem_t *after, DB_playItem_t *origin, aac_chapter_t *chapters, int num_chapters, int64_t totalsamples, int samplerate) {
    deadbeef->pl_lock ();
    DB_playItem_t *ins = after;
    for (int i = 0; i < num_chapters; i++) {
        const char *uri = deadbeef->pl_find_meta_raw (origin, ":URI");
        const char *dec = deadbeef->pl_find_meta_raw (origin, ":DECODER");
        const char *ftype= "MP4 AAC";//pl_find_meta_raw (origin, ":FILETYPE");

        DB_playItem_t *it = deadbeef->pl_item_alloc_init (uri, dec);
        deadbeef->pl_set_meta_int (it, ":TRACKNUM", i);
        deadbeef->pl_set_meta_int (it, "TRACK", i);
        // poor-man utf8 check
        if (!chapters[i].title || deadbeef->junk_detect_charset (chapters[i].title)) {
            char s[1000];
            snprintf (s, sizeof (s), "chapter %d", i+1);
            deadbeef->pl_add_meta (it, "title", s);
        }
        else {
            deadbeef->pl_add_meta (it, "title", chapters[i].title);
        }
        it->startsample = chapters[i].startsample;
        it->endsample = chapters[i].endsample;
        deadbeef->pl_replace_meta (it, ":FILETYPE", ftype);
        deadbeef->plt_set_item_duration (plt, it, (float)(it->endsample - it->startsample + 1) / samplerate);
        after = deadbeef->plt_insert_item (plt, after, it);
        deadbeef->pl_item_unref (it);
    }
    deadbeef->pl_item_ref (after);
    
    DB_playItem_t *first = deadbeef->pl_get_next (ins, PL_MAIN);
    
    if (!first) {
        first = deadbeef->plt_get_first (plt, PL_MAIN);
    }

    if (!first) {
        deadbeef->pl_unlock ();
        return NULL;
    }
    // copy metadata from embedded tags
    uint32_t f = deadbeef->pl_get_item_flags (origin);
    f |= DDB_IS_SUBTRACK;
    deadbeef->pl_set_item_flags (origin, f);
    deadbeef->pl_items_copy_junk (origin, first, after);
    deadbeef->pl_item_unref (first);

    deadbeef->pl_unlock ();
    return after;
}

static DB_playItem_t *
aac_insert (ddb_playlist_t *plt, DB_playItem_t *after, const char *fname) {
    DB_FILE *fp = deadbeef->fopen (fname);
    if (!fp) {
        return NULL;
    }
    aac_info_t info = {0};
    info.junk = deadbeef->junk_get_leading_size (fp);
    if (info.junk >= 0) {
        deadbeef->fseek (fp, info.junk, SEEK_SET);
    }
    else {
        info.junk = 0;
    }

    const char *ftype = NULL;
    float duration = -1;
    int64_t totalsamples = 0;
    int samplerate = 0;
    int channels = 0;

    if (fp->vfs->is_streaming ()) {
        ftype = "RAW AAC";
    }
    else {
        // slowwww!
        info.file = fp;
        MP4FILE_CB cb = {
            .read = aac_fs_read,
            .write = NULL,
            .seek = aac_fs_seek,
            .truncate = NULL,
            .user_data = &info
        };
        mp4ff_t *mp4 = mp4ff_open_read (&cb);
        if (mp4) {
            int ntracks = mp4ff_total_tracks (mp4);
            int i;
            for (i = 0; i < ntracks; i++) {
                if (mp4ff_get_track_type (mp4, i) != TRACK_AUDIO) {
                    continue;
                }
                int mp4framesize;
                int res = mp4_track_get_info (mp4, i, &duration, &samplerate, &channels, &totalsamples, &mp4framesize);
                if (res >= 0 && duration > 0) {

                    int num_chapters = 0;
                    aac_chapter_t *chapters = NULL;
                    if (mp4ff_chap_get_num_tracks(mp4) > 0) {
                        chapters = aac_load_itunes_chapters (mp4, &num_chapters, samplerate);
                    }

                    DB_playItem_t *it = deadbeef->pl_item_alloc_init (fname, plugin.plugin.id);
                    ftype = "MP4 AAC";
                    deadbeef->pl_add_meta (it, ":FILETYPE", ftype);
                    deadbeef->pl_set_meta_int (it, ":TRACKNUM", i);
                    deadbeef->plt_set_item_duration (plt, it, duration);

                    deadbeef->rewind (fp);
                    mp4_read_metadata_file(it, fp);

                    int64_t fsize = deadbeef->fgetlength (fp);
                    deadbeef->fclose (fp);

                    char s[100];
                    snprintf (s, sizeof (s), "%lld", fsize);
                    deadbeef->pl_add_meta (it, ":FILE_SIZE", s);
                    deadbeef->pl_add_meta (it, ":BPS", "16");
                    snprintf (s, sizeof (s), "%d", channels);
                    deadbeef->pl_add_meta (it, ":CHANNELS", s);
                    snprintf (s, sizeof (s), "%d", samplerate);
                    deadbeef->pl_add_meta (it, ":SAMPLERATE", s);
                    int br = (int)roundf(fsize / duration * 8 / 1000);
                    snprintf (s, sizeof (s), "%d", br);
                    deadbeef->pl_add_meta (it, ":BITRATE", s);

                    // embedded chapters
                    deadbeef->pl_lock (); // FIXME: is it needed?
                    if (chapters && num_chapters > 0) {
                        DB_playItem_t *cue = aac_insert_with_chapters (plt, after, it, chapters, num_chapters, totalsamples, samplerate);
                        for (int n = 0; n < num_chapters; n++) {
                            if (chapters[n].title) {
                                free (chapters[n].title);
                            }
                        }
                        free (chapters);
                        if (cue) {
                            mp4ff_close (mp4);
                            deadbeef->pl_item_unref (it);
                            deadbeef->pl_item_unref (cue);
                            deadbeef->pl_unlock ();
                            return cue;
                        }
                    }

                    // embedded cue
                    const char *cuesheet = deadbeef->pl_find_meta (it, "cuesheet");
                    DB_playItem_t *cue = NULL;

                    if (cuesheet) {
                        cue = deadbeef->plt_insert_cue_from_buffer (plt, after, it, (const uint8_t *)cuesheet, (int)strlen (cuesheet), (int)totalsamples, samplerate);
                        if (cue) {
                            mp4ff_close (mp4);
                            deadbeef->pl_item_unref (it);
                            deadbeef->pl_item_unref (cue);
                            deadbeef->pl_unlock ();
                            return cue;
                        }
                    }
                    deadbeef->pl_unlock ();

                    cue  = deadbeef->plt_insert_cue (plt, after, it, (int)totalsamples, samplerate);
                    if (cue) {
                        deadbeef->pl_item_unref (it);
                        deadbeef->pl_item_unref (cue);
                        return cue;
                    }

                    after = deadbeef->plt_insert_item (plt, after, it);
                    deadbeef->pl_item_unref (it);
                    break;
                }
            }
            mp4ff_close (mp4);
            if (i < ntracks) {
                return after;
            }
            // mp4 container found, but no valid aac tracks in it
            return NULL;
        }
    }
    int res = aac_probe (fp, &duration, &samplerate, &channels, &totalsamples);
    if (res == -1) {
        deadbeef->fclose (fp);
        return NULL;
    }
    ftype = "RAW AAC";
    DB_playItem_t *it = deadbeef->pl_item_alloc_init (fname, plugin.plugin.id);
    deadbeef->pl_add_meta (it, ":FILETYPE", ftype);
    deadbeef->plt_set_item_duration (plt, it, duration);

    // read tags
    (void)deadbeef->junk_apev2_read (it, fp);
    (void)deadbeef->junk_id3v2_read (it, fp);
    (void)deadbeef->junk_id3v1_read (it, fp);

    int64_t fsize = deadbeef->fgetlength (fp);

    deadbeef->fclose (fp);

    if (duration > 0) {
        char s[100];
        snprintf (s, sizeof (s), "%lld", fsize);
        deadbeef->pl_add_meta (it, ":FILE_SIZE", s);
        deadbeef->pl_add_meta (it, ":BPS", "16");
        snprintf (s, sizeof (s), "%d", channels);
        deadbeef->pl_add_meta (it, ":CHANNELS", s);
        snprintf (s, sizeof (s), "%d", samplerate);
        deadbeef->pl_add_meta (it, ":SAMPLERATE", s);
        int br = (int)roundf(fsize / duration * 8 / 1000);
        snprintf (s, sizeof (s), "%d", br);
        deadbeef->pl_add_meta (it, ":BITRATE", s);
        // embedded cue
        deadbeef->pl_lock ();
        const char *cuesheet = deadbeef->pl_find_meta (it, "cuesheet");
        DB_playItem_t *cue = NULL;

        if (cuesheet) {
            cue = deadbeef->plt_insert_cue_from_buffer (plt, after, it, (uint8_t *)cuesheet, (int)strlen (cuesheet), (int)totalsamples, samplerate);
            if (cue) {
                deadbeef->pl_item_unref (it);
                deadbeef->pl_item_unref (cue);
                deadbeef->pl_unlock ();
                return cue;
            }
        }
        deadbeef->pl_unlock ();

        cue  = deadbeef->plt_insert_cue (plt, after, it, (int)totalsamples, samplerate);
        if (cue) {
            deadbeef->pl_item_unref (it);
            deadbeef->pl_item_unref (cue);
            return cue;
        }
    }

    after = deadbeef->plt_insert_item (plt, after, it);
    deadbeef->pl_item_unref (it);

    return after;
}

static const char * exts[] = { "aac", "mp4", "m4a", "m4b", NULL };

// define plugin interface
static DB_decoder_t plugin = {
    .plugin.api_vmajor = 1,
    .plugin.api_vminor = 0,
    .plugin.version_major = 2,
    .plugin.version_minor = 0,
//    .plugin.flags = DDB_PLUGIN_FLAG_LOGGING,
    .plugin.type = DB_PLUGIN_DECODER,
    .plugin.id = "aac",
    .plugin.name = "AAC player",
    .plugin.descr = "plays aac files, supports raw aac files, as well as mp4 container",
    .plugin.copyright = 
        "AAC DeaDBeeF Player Plugin\n"
        "Copyright (c) 2009-2014 Alexey Yakovenko <waker@users.sourceforge.net>\n"
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
        "\n"
        "\n"
        "libmp4ff (modified)\n"
        "Code from MP4FF is copyright (c) Nero AG, www.nero.com\n"
        "deadbeef-related modifications (c) 2009-2014 Alexey Yakovenko\n"
        "\n"
        "Relies on libfaad2\n"
        "Code from FAAD2 is copyright (c) Nero AG, www.nero.com\n"
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
        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n"
    ,
    .plugin.website = "http://deadbeef.sf.net",
    .open = aac_open,
    .init = aac_init,
    .free = aac_free,
    .read = aac_read,
    .seek = aac_seek,
    .seek_sample = aac_seek_sample,
    .insert = aac_insert,
    .read_metadata = mp4_read_metadata,
    .write_metadata = mp4_write_metadata,
    .exts = exts,
};

DB_plugin_t *
aac_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}
