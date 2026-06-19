/*
 * mp4_demux.h — Sequential MP4 Demuxer
 *
 * Rules:
 *   - fseek() is NEVER called.  All I/O goes through SeqReader (fread only).
 *   - The file MUST be FastStart (moov before mdat).
 *   - Big-Endian multi-byte fields are decoded with explicit bit-shifts.
 *   - stsc run-length entries are expanded into per-chunk sample counts.
 *   - Video + audio CombinedChunk lists are merged and sorted by file offset,
 *     then drained in one sequential pass over mdat.
 */

#ifndef MP4_DEMUX_H
#define MP4_DEMUX_H

#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */
#define MP4_MAX_TRACKS 8

/* ------------------------------------------------------------------ */
/*  Track classification                                               */
/* ------------------------------------------------------------------ */
typedef enum { TRACK_VIDEO = 0, TRACK_AUDIO = 1, TRACK_UNKNOWN = 2 } TrackType;

/* ------------------------------------------------------------------ */
/*  stsc (Sample-to-Chunk) compact run entry                          */
/*                                                                     */
/*  The stsc box is run-length-encoded:                               */
/*    "from first_chunk through the chunk before the next entry,      */
/*     every chunk holds samples_per_chunk samples."                  */
/* ------------------------------------------------------------------ */
typedef struct {
  uint32_t first_chunk; /* 1-indexed chunk number        */
  uint32_t samples_per_chunk;
  uint32_t sample_description_idx; /* codec index (usually 1)      */
} StscEntry;

/* ------------------------------------------------------------------ */
/*  Per-track metadata (everything we pull from moov)                 */
/* ------------------------------------------------------------------ */
typedef struct {
  TrackType type;

  /* stco / co64 — chunk absolute file offsets (always stored 64-bit) */
  uint32_t chunk_count;
  uint64_t *chunk_offsets; /* malloc'd [chunk_count]        */

  /* stsz — per-sample byte sizes */
  uint32_t sample_count;
  uint32_t default_sample_size; /* non-zero ⟹ all samples equal */
  uint32_t *sample_sizes;       /* malloc'd [sample_count] or NULL */

  /* stsc — compact run table */
  uint32_t stsc_entry_count;
  StscEntry *stsc_entries; /* malloc'd [stsc_entry_count]   */
} TrackInfo;

/* ------------------------------------------------------------------ */
/*  CombinedChunk — one chunk from any track, ready for mdat drainage */
/* ------------------------------------------------------------------ */
typedef struct {
  uint64_t file_offset;     /* absolute byte offset (from stco/co64)  */
  uint32_t sample_count;    /* after expand_stsc()                     */
  uint32_t first_sample_id; /* index of chunk's first sample in stsz   */
  TrackType type;
  int track_idx; /* slot in ctx->tracks[]                   */
} CombinedChunk;

/* ------------------------------------------------------------------ */
/*  SeqReader — wraps FILE* and tracks the current byte position      */
/*  fseek is never called; pos is updated manually on every fread.    */
/* ------------------------------------------------------------------ */
typedef struct {
  FILE *fp;
  uint64_t pos; /* bytes consumed since the start of the file       */
} SeqReader;

/* ------------------------------------------------------------------ */
/*  Top-level demuxer context                                          */
/* ------------------------------------------------------------------ */
typedef struct {
  SeqReader reader;
  TrackInfo tracks[MP4_MAX_TRACKS];
  int track_count;
  int active_track; /* index while inside a trak box, else -1  */
} Mp4Demux;

/* ------------------------------------------------------------------ */
/*  User-supplied decode callbacks                                     */
/*                                                                     */
/*  video_cb  — called once per video sample (H.264 NAL unit / HEVC)  */
/*  audio_cb  — called once per audio sample (MP3 frame / AAC packet) */
/* ------------------------------------------------------------------ */
typedef void (*VideoFrameCb)(const uint8_t *data, uint32_t size,
                             void *userdata);
typedef void (*AudioPacketCb)(const uint8_t *data, uint32_t size,
                              void *userdata);

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * mp4_open() — open @filename and parse the moov box completely.
 *
 * On success the SeqReader is parked at byte 0 of the mdat payload,
 * all TrackInfo tables are populated, and 0 is returned.
 *
 * Returns -1 if:
 *   • the file cannot be opened
 *   • mdat is found before moov (not FastStart)
 *   • no mdat box exists after moov
 */
int mp4_open(Mp4Demux *ctx, const char *filename);

/**
 * mp4_stream() — drain the mdat box in sorted chunk order.
 *
 * Builds the interleaved CombinedChunk index, sorts it by file_offset,
 * then reads every sample sequentially, dispatching to the callbacks.
 * Must be called after mp4_open() has returned 0.
 *
 * Returns 0 on success, -1 on I/O or allocation error.
 */
int mp4_stream(Mp4Demux *ctx, VideoFrameCb video_cb, AudioPacketCb audio_cb,
               void *userdata);

/** mp4_close() — release all malloc'd memory and close the file. */
void mp4_close(Mp4Demux *ctx);

#endif /* MP4_DEMUX_H */
