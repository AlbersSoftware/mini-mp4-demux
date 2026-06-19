/*
 * main.c — MP4 Demuxer demonstration with minimp3 audio decoding
 *
 * Build:
 *   make fetch-minimp3   # downloads minimp3.h once
 *   make                 # compiles everything
 *
 * Run:
 *   ./mp4demux your_faststart.mp4
 *
 * Audio codec notes:
 *   - minimp3 handles MP3 frames natively (audio track codec = ".mp3" / 0x6B).
 *   - AAC audio (the common case in modern MP4 files) will print raw packet
 *     info.  To decode AAC swap in fdk-aac or faad2 inside on_audio().
 *   - The demuxer layer is identical regardless of codec — only the callback
 *     body changes.
 *
 * Video codec notes:
 *   - Samples are raw AVCC-framed H.264 NAL units (4-byte length prefix) or
 *     HEVC NAL units depending on the track's stsd box.
 *   - Feed them to a H.264 decoder (e.g. libavcodec, OpenH264) inside on_video().
 */

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "mp4_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/*  Application state — threaded through both callbacks               */
/* ------------------------------------------------------------------ */
typedef struct {
    /* minimp3 decoder state */
    mp3dec_t             dec;
    mp3dec_frame_info_t  frame_info;

    /* Running totals (printed in the final summary) */
    uint64_t video_frames;
    uint64_t audio_packets;
    uint64_t pcm_samples_decoded; /* non-zero only for MP3 audio tracks */
} AppState;

/* ------------------------------------------------------------------ */
/*  Video callback                                                      */
/*                                                                     */
/*  data — one H.264 / HEVC sample in AVCC format:                    */
/*          [4-byte big-endian NAL unit length][NAL unit bytes]...     */
/*  size — total byte length of data[]                                 */
/* ------------------------------------------------------------------ */
static void on_video(const uint8_t *data, uint32_t size, void *ud)
{
    AppState *app = (AppState *)ud;
    app->video_frames++;

    /*
     * Show the first 4 bytes.  In AVCC format those bytes are a big-endian
     * length field rather than a start code — a H.264 decoder that needs
     * Annex-B start codes (0x00 0x00 0x00 0x01) must convert them first.
     */
    printf("    [V] frame #%" PRIu64 "  %u bytes  avcc_hdr=[%02x %02x %02x %02x]\n",
           app->video_frames, size,
           size > 0 ? data[0] : 0u,
           size > 1 ? data[1] : 0u,
           size > 2 ? data[2] : 0u,
           size > 3 ? data[3] : 0u);

    /*
     * === PLUG IN YOUR H.264 DECODER HERE ===
     *
     * Example with OpenH264:
     *   SBufferInfo dst_info;
     *   uint8_t *dst_planes[3];
     *   decoder->DecodeFrameNoDelay(data, size, dst_planes, &dst_info);
     *
     * Example with libavcodec:
     *   av_packet_from_data(pkt, data, size);
     *   avcodec_send_packet(codec_ctx, pkt);
     *   avcodec_receive_frame(codec_ctx, frame);
     */
    (void)data;
}

/* ------------------------------------------------------------------ */
/*  Audio callback                                                      */
/*                                                                     */
/*  data — one raw audio packet:                                       */
/*           MP3 track  → complete MP3 frame (ID3 / sync word)        */
/*           AAC track  → raw AAC-LC / HE-AAC frame (no ADTS header)  */
/*  size — byte length of data[]                                       */
/* ------------------------------------------------------------------ */
static void on_audio(const uint8_t *data, uint32_t size, void *ud)
{
    AppState *app = (AppState *)ud;
    app->audio_packets++;

    /*
     * Attempt MP3 decode via minimp3.
     * mp3dec_decode_frame() returns the number of PCM samples decoded,
     * or 0 if the buffer is not a valid MP3 frame (e.g. AAC data).
     */
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int samples = mp3dec_decode_frame(&app->dec,
                                      data, (int)size,
                                      pcm, &app->frame_info);

    if (samples > 0) {
        /* ---- MP3 path: we got real PCM output ---- */
        app->pcm_samples_decoded += (uint64_t)samples;

        printf("    [A/MP3] pkt #%" PRIu64
               "  decoded=%d samples  ch=%d  hz=%d  total_pcm=%" PRIu64 "\n",
               app->audio_packets, samples,
               app->frame_info.channels,
               app->frame_info.hz,
               app->pcm_samples_decoded);

        /*
         * === PLUG IN YOUR PCM SINK HERE ===
         *
         * pcm[] holds (samples * channels) interleaved 16-bit signed samples.
         *
         * Example: write to a WAV file or audio ring-buffer
         *   fwrite(pcm, sizeof(mp3d_sample_t),
         *          (size_t)samples * app->frame_info.channels, wav_fp);
         *
         * Example: feed an SDL2 audio queue
         *   SDL_QueueAudio(audio_device_id, pcm,
         *                  (uint32_t)(samples * app->frame_info.channels
         *                             * sizeof(int16_t)));
         */
        (void)pcm;

    } else {
        /* ---- Non-MP3 path (most likely raw AAC) ---- */
        printf("    [A/RAW] pkt #%" PRIu64
               "  %u bytes  hdr=[%02x %02x %02x %02x]\n",
               app->audio_packets, size,
               size > 0 ? data[0] : 0u,
               size > 1 ? data[1] : 0u,
               size > 2 ? data[2] : 0u,
               size > 3 ? data[3] : 0u);

        /*
         * === PLUG IN YOUR AAC DECODER HERE ===
         *
         * Example with fdk-aac:
         *   UINT in_sz = size;
         *   UCHAR *in_ptr = (UCHAR *)data;
         *   aacDecoder_Fill(haac, &in_ptr, &in_sz, &in_sz);
         *   aacDecoder_DecodeFrame(haac, pcm_buf, pcm_buf_sz, 0);
         *
         * Example with faad2:
         *   NeAACDecDecode(faad_handle, &frameinfo, data, size);
         */
        (void)data;
    }
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <faststart.mp4>\n\n"
            "  The MP4 must have moov before mdat (FastStart encoding).\n\n"
            "  Convert any MP4 to FastStart with ffmpeg:\n"
            "    ffmpeg -i input.mp4 -c copy -movflags faststart output.mp4\n\n"
            "  To generate a small test file (H.264 + MP3 audio):\n"
            "    ffmpeg -f lavfi -i testsrc=duration=5:size=320x240:rate=24 \\\n"
            "           -f lavfi -i sine=frequency=440:duration=5 \\\n"
            "           -c:v libx264 -c:a libmp3lame \\\n"
            "           -movflags faststart test.mp4\n",
            argv[0]);
        return 1;
    }

    /* ---- initialise minimp3 ---------------------------------------- */
    AppState app;
    memset(&app, 0, sizeof app);
    mp3dec_init(&app.dec);

    /* ---- parse moov and park at mdat ------------------------------- */
    Mp4Demux demux;
    if (mp4_open(&demux, argv[1]) != 0)
        return 1;

    /* ---- print track summary --------------------------------------- */
    printf("\nTrack summary:\n");
    for (int i = 0; i < demux.track_count; i++) {
        const TrackInfo *t = &demux.tracks[i];
        printf("  [%d] %-7s  chunks=%-6u  samples=%-8u  stsc_runs=%u\n",
               i,
               t->type == TRACK_VIDEO ? "VIDEO"   :
               t->type == TRACK_AUDIO ? "AUDIO"   : "UNKNOWN",
               t->chunk_count,
               t->sample_count,
               t->stsc_entry_count);
    }
    printf("\n");

    /* ---- drain mdat ------------------------------------------------ */
    if (mp4_stream(&demux, on_video, on_audio, &app) != 0) {
        mp4_close(&demux);
        return 1;
    }

    /* ---- final summary --------------------------------------------- */
    printf("\n+----------------------------------+\n");
    printf("|  Session summary                 |\n");
    printf("+----------------------------------+\n");
    printf("|  Video frames       : %-10" PRIu64 " |\n", app.video_frames);
    printf("|  Audio packets      : %-10" PRIu64 " |\n", app.audio_packets);
    printf("|  PCM samples (MP3)  : %-10" PRIu64 " |\n", app.pcm_samples_decoded);
    printf("+----------------------------------+\n");

    mp4_close(&demux);
    return 0;
}
