/*
 * mp4_demux.c — Sequential MP4 Demuxer Implementation
 *
 * I/O contract:
 *   fseek() is NEVER called.
 *   Every read goes through seq_read() / seq_skip() which update SeqReader.pos.
 *   SeqReader.pos always reflects the next unread byte's absolute file offset.
 *
 * Parse flow:
 *   mp4_open()
 *     └─ top-level loop: ftyp/free → skip  |  moov → walk_boxes()  |  mdat → park & return
 *
 *   walk_boxes(end_pos)  [recursive]
 *     └─ dispatch_box()
 *           ├─ trak    → push TrackInfo slot, recurse, pop
 *           ├─ moov/mdia/minf/stbl/dinf/edts → recurse
 *           ├─ hdlr    → set TrackType (vide/soun)
 *           ├─ stsc    → load compact run-length table
 *           ├─ stsz    → load per-sample size table
 *           ├─ stco    → load 32-bit chunk offsets
 *           ├─ co64    → load 64-bit chunk offsets
 *           └─ *       → seq_skip() payload
 *
 *   mp4_stream()
 *     ├─ build_chunk_index()
 *     │     ├─ expand_stsc() per track  → per-chunk sample counts
 *     │     ├─ fill CombinedChunk[]     → video + audio chunks merged
 *     │     └─ qsort() by file_offset   → physical mdat order
 *     └─ for each chunk (sorted):
 *           ├─ seq_skip() any gap to chunk.file_offset
 *           └─ for each sample: seq_read(frame_size) → callback
 */

#include "mp4_demux.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

/* ================================================================== */
/*  SeqReader primitives — no fseek, ever                             */
/* ================================================================== */

/**
 * Read exactly @n bytes into @buf.
 * Updates r->pos.  Returns 0 on success, -1 on short read / error.
 */
static int seq_read(SeqReader *r, void *buf, size_t n)
{
    size_t got = fread(buf, 1, n, r->fp);
    r->pos += (uint64_t)got;
    return (got == n) ? 0 : -1;
}

/**
 * Read a 32-bit big-endian unsigned integer.
 * Bit-shifts used explicitly — no ntohl / platform endian assumptions.
 */
static uint32_t seq_u32be(SeqReader *r)
{
    uint8_t b[4] = {0};
    seq_read(r, b, 4);
    return ((uint32_t)b[0] << 24)
         | ((uint32_t)b[1] << 16)
         | ((uint32_t)b[2] <<  8)
         |  (uint32_t)b[3];
}

/**
 * Read a 64-bit big-endian unsigned integer (two sequential u32be reads).
 */
static uint64_t seq_u64be(SeqReader *r)
{
    uint64_t hi = (uint64_t)seq_u32be(r);
    uint64_t lo = (uint64_t)seq_u32be(r);
    return (hi << 32) | lo;
}

/**
 * Discard @n bytes from the stream using a scratch buffer.
 * fseek is not called.
 */
static void seq_skip(SeqReader *r, uint64_t n)
{
    uint8_t scratch[4096];
    while (n > 0) {
        size_t chunk = (n > sizeof scratch) ? sizeof scratch : (size_t)n;
        size_t got   = fread(scratch, 1, chunk, r->fp);
        r->pos += (uint64_t)got;
        if (got == 0) break;   /* EOF or error */
        n -= (uint64_t)got;
    }
}

/* ================================================================== */
/*  Box header                                                         */
/* ================================================================== */

typedef struct {
    char     type[5];   /* 4-byte FourCC + NUL                         */
    uint64_t size;      /* total box size incl. header (UINT64_MAX=EOF) */
    uint64_t hdr_size;  /* 8 bytes normally, 16 for largesize boxes     */
} BoxHdr;

/**
 * Read one box header from the stream.
 * Handles the three ISO 14496-12 size cases:
 *   size32 > 1  → normal 8-byte header
 *   size32 == 1 → largesize: 16-byte header with 64-bit size field
 *   size32 == 0 → box extends to EOF (size stored as UINT64_MAX)
 */
static int read_box_hdr(SeqReader *r, BoxHdr *h)
{
    /* Read size field manually so we can distinguish EOF from size==0 */
    uint8_t sb[4];
    size_t  got = fread(sb, 1, 4, r->fp);
    r->pos += (uint64_t)got;
    if (got == 0) return -1;   /* clean EOF */
    if (got < 4)  return -1;   /* truncated */

    uint32_t size32 = ((uint32_t)sb[0] << 24)
                    | ((uint32_t)sb[1] << 16)
                    | ((uint32_t)sb[2] <<  8)
                    |  (uint32_t)sb[3];

    uint8_t typ[4];
    if (seq_read(r, typ, 4) != 0) return -1;
    memcpy(h->type, typ, 4);
    h->type[4] = '\0';

    if (size32 == 1) {
        /* ISO 14496-12 §4.2: largesize follows the type field */
        h->size     = seq_u64be(r);
        h->hdr_size = 16;
    } else if (size32 == 0) {
        /* Box extends to end of file */
        h->size     = UINT64_MAX;
        h->hdr_size = 8;
    } else {
        h->size     = (uint64_t)size32;
        h->hdr_size = 8;
    }
    return 0;
}

/* Payload length (bytes after the header) */
static inline uint64_t box_payload(const BoxHdr *h)
{
    return (h->size == UINT64_MAX) ? UINT64_MAX : h->size - h->hdr_size;
}

/* Sanity cap: reject implausibly large table entry counts */
#define MAX_SANE_ENTRIES  (16u * 1024u * 1024u)   /* 16 M entries */

/* ================================================================== */
/*  Leaf-box parsers                                                   */
/* ================================================================== */

/**
 * hdlr — sets TrackInfo.type to TRACK_VIDEO or TRACK_AUDIO.
 *
 * Layout (payload):
 *   [4] version+flags
 *   [4] pre_defined
 *   [4] handler_type  ("vide" or "soun")
 *   [12] reserved
 *   [N]  name (NUL-terminated string)
 */
static void parse_hdlr(SeqReader *r, TrackInfo *t, uint64_t payload)
{
    seq_u32be(r);   /* version + flags */
    seq_u32be(r);   /* pre_defined     */

    uint8_t handler[4];
    if (seq_read(r, handler, 4) == 0) {
        if      (memcmp(handler, "vide", 4) == 0) t->type = TRACK_VIDEO;
        else if (memcmp(handler, "soun", 4) == 0) t->type = TRACK_AUDIO;
        else                                       t->type = TRACK_UNKNOWN;

        printf("    [hdlr] handler_type='%.4s'  ->  %s\n", handler,
               t->type == TRACK_VIDEO ? "VIDEO"   :
               t->type == TRACK_AUDIO ? "AUDIO"   : "UNKNOWN");
    }

    /* Skip reserved[12] + name string */
    uint64_t consumed = 4u + 4u + 4u;
    if (payload > consumed) seq_skip(r, payload - consumed);
}

/**
 * stco — 32-bit chunk offset table.
 *
 * Layout (payload):
 *   [4] version+flags
 *   [4] entry_count
 *   entry_count × [4] chunk_offset   (32-bit absolute file offsets)
 */
static void parse_stco(SeqReader *r, TrackInfo *t, uint64_t payload)
{
    (void)payload;
    seq_u32be(r);                       /* version + flags */
    uint32_t n = seq_u32be(r);

    if (n > MAX_SANE_ENTRIES) {
        fprintf(stderr, "    [stco] entry_count %u exceeds sanity cap\n", n);
        return;
    }
    t->chunk_count   = n;
    t->chunk_offsets = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (!t->chunk_offsets) return;

    for (uint32_t i = 0; i < n; i++)
        t->chunk_offsets[i] = (uint64_t)seq_u32be(r);

    printf("    [stco] %u chunk offsets (32-bit)\n", n);
}

/**
 * co64 — 64-bit chunk offset table (same as stco but each entry is 8 bytes).
 * Used for files > 4 GB.
 */
static void parse_co64(SeqReader *r, TrackInfo *t, uint64_t payload)
{
    (void)payload;
    seq_u32be(r);                       /* version + flags */
    uint32_t n = seq_u32be(r);

    if (n > MAX_SANE_ENTRIES) {
        fprintf(stderr, "    [co64] entry_count %u exceeds sanity cap\n", n);
        return;
    }
    t->chunk_count   = n;
    t->chunk_offsets = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (!t->chunk_offsets) return;

    for (uint32_t i = 0; i < n; i++)
        t->chunk_offsets[i] = seq_u64be(r);

    printf("    [co64] %u chunk offsets (64-bit)\n", n);
}

/**
 * stsz — per-sample byte sizes.
 *
 * Layout (payload):
 *   [4] version+flags
 *   [4] sample_size   (non-zero → all samples are this size; no table follows)
 *   [4] sample_count
 *   sample_count × [4] entry_size   (only present when sample_size == 0)
 */
static void parse_stsz(SeqReader *r, TrackInfo *t, uint64_t payload)
{
    (void)payload;
    seq_u32be(r);                           /* version + flags */
    uint32_t def  = seq_u32be(r);          /* uniform size, or 0 */
    uint32_t n    = seq_u32be(r);          /* sample count       */

    if (n > MAX_SANE_ENTRIES) {
        fprintf(stderr, "    [stsz] sample_count %u exceeds sanity cap\n", n);
        return;
    }

    t->sample_count        = n;
    t->default_sample_size = def;

    if (def == 0) {
        /* Variable sample sizes — store each one */
        t->sample_sizes = (uint32_t *)malloc(n * sizeof(uint32_t));
        if (!t->sample_sizes) return;
        for (uint32_t i = 0; i < n; i++)
            t->sample_sizes[i] = seq_u32be(r);
    }
    /* else: uniform size — t->sample_sizes stays NULL; use default_sample_size */

    printf("    [stsz] %u samples, default_size=%u%s\n",
           n, def, def ? "" : " (per-sample table)");
}

/**
 * stsc — Sample-to-Chunk compact run-length table.
 *
 * Layout (payload):
 *   [4] version+flags
 *   [4] entry_count
 *   entry_count × { [4] first_chunk, [4] samples_per_chunk, [4] sample_description_idx }
 *
 * Semantics:
 *   Entries are in ascending first_chunk order.
 *   Entry i applies to chunks [first_chunk[i] … first_chunk[i+1]-1].
 *   The last entry applies to all remaining chunks.
 *
 * expand_stsc() (below) materialises this into a flat per-chunk array.
 */
static void parse_stsc(SeqReader *r, TrackInfo *t, uint64_t payload)
{
    (void)payload;
    seq_u32be(r);                       /* version + flags */
    uint32_t n = seq_u32be(r);

    if (n > MAX_SANE_ENTRIES) {
        fprintf(stderr, "    [stsc] entry_count %u exceeds sanity cap\n", n);
        return;
    }
    t->stsc_entry_count = n;
    t->stsc_entries     = (StscEntry *)malloc(n * sizeof(StscEntry));
    if (!t->stsc_entries) return;

    for (uint32_t i = 0; i < n; i++) {
        t->stsc_entries[i].first_chunk            = seq_u32be(r);
        t->stsc_entries[i].samples_per_chunk       = seq_u32be(r);
        t->stsc_entries[i].sample_description_idx  = seq_u32be(r);
    }
    printf("    [stsc] %u run entries\n", n);
}

/* ================================================================== */
/*  Box-type classification                                            */
/* ================================================================== */

static int is_container(const char *type)
{
    /* Boxes whose payload is a list of child boxes (no own data fields) */
    static const char * const containers[] = {
        "moov", "trak", "mdia", "minf", "stbl", "dinf", "udta", "edts", NULL
    };
    for (int i = 0; containers[i]; i++)
        if (strcmp(type, containers[i]) == 0) return 1;
    return 0;
}

/* ================================================================== */
/*  Recursive box walker                                              */
/* ================================================================== */

/* Forward declaration so dispatch_box and walk_boxes can call each other */
static void walk_boxes(Mp4Demux *ctx, uint64_t end_pos, int depth);

/**
 * dispatch_box() — process one box:
 *   • Container boxes → recurse via walk_boxes()
 *   • trak            → push track slot, recurse, pop
 *   • Known leaf boxes → dedicated parser
 *   • Unknown/unused   → seq_skip() payload
 *
 * Guarantees: after returning, r->pos == pl_start + payload
 * (any unread tail bytes are consumed by the trailing skip).
 */
static void dispatch_box(Mp4Demux *ctx, const BoxHdr *h, int depth)
{
    SeqReader *r       = &ctx->reader;
    uint64_t   payload = box_payload(h);
    uint64_t   pl_start = r->pos;   /* first byte of the box payload */

    TrackInfo *t = (ctx->active_track >= 0)
                 ? &ctx->tracks[ctx->active_track]
                 : NULL;

    /* Pretty-print the box tree */
    for (int i = 0; i < depth * 2; i++) putchar(' ');
    printf("[%s]  size=%" PRIu64 "\n", h->type, h->size);

    /* ---- trak: push a new track slot ----------------------------- */
    if (strcmp(h->type, "trak") == 0) {
        int idx = ctx->track_count;
        if (idx < MP4_MAX_TRACKS) {
            memset(&ctx->tracks[idx], 0, sizeof(TrackInfo));
            ctx->tracks[idx].type = TRACK_UNKNOWN;
            ctx->active_track     = idx;
            ctx->track_count++;
        }
        uint64_t end = (payload == UINT64_MAX) ? UINT64_MAX : pl_start + payload;
        walk_boxes(ctx, end, depth + 1);
        /* Drain any unread bytes within trak */
        if (payload != UINT64_MAX && r->pos < pl_start + payload)
            seq_skip(r, (pl_start + payload) - r->pos);
        ctx->active_track = -1;
        return;
    }

    /* ---- generic containers -------------------------------------- */
    if (is_container(h->type)) {
        uint64_t end = (payload == UINT64_MAX) ? UINT64_MAX : pl_start + payload;
        walk_boxes(ctx, end, depth + 1);
        if (payload != UINT64_MAX && r->pos < pl_start + payload)
            seq_skip(r, (pl_start + payload) - r->pos);
        return;
    }

    /* ---- leaf boxes ---------------------------------------------- */
    if (t != NULL) {
        /* Inside a track context — dispatch to specialised parsers */
        if      (strcmp(h->type, "hdlr") == 0) parse_hdlr(r, t, payload);
        else if (strcmp(h->type, "stco") == 0) parse_stco(r, t, payload);
        else if (strcmp(h->type, "co64") == 0) parse_co64(r, t, payload);
        else if (strcmp(h->type, "stsz") == 0) parse_stsz(r, t, payload);
        else if (strcmp(h->type, "stsc") == 0) parse_stsc(r, t, payload);
        else if (payload != UINT64_MAX)          seq_skip(r, payload);
    } else {
        /* Outside track context (ftyp, mvhd, udta children, …) */
        if (payload != UINT64_MAX) seq_skip(r, payload);
    }

    /* Consume any bytes the parser left behind (protects position) */
    if (payload != UINT64_MAX) {
        uint64_t consumed = r->pos - pl_start;
        if (consumed < payload)
            seq_skip(r, payload - consumed);
    }
}

/**
 * walk_boxes() — read and dispatch child boxes until end_pos is reached
 * or EOF/error occurs.
 */
static void walk_boxes(Mp4Demux *ctx, uint64_t end_pos, int depth)
{
    SeqReader *r = &ctx->reader;
    for (;;) {
        if (feof(r->fp) || ferror(r->fp)) break;
        if (end_pos != UINT64_MAX && r->pos >= end_pos) break;

        BoxHdr h;
        if (read_box_hdr(r, &h) != 0) break;

        dispatch_box(ctx, &h, depth);
    }
}

/* ================================================================== */
/*  mp4_open — top-level scan: moov → parse tables; mdat → park      */
/* ================================================================== */

int mp4_open(Mp4Demux *ctx, const char *filename)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->active_track = -1;

    ctx->reader.fp = fopen(filename, "rb");
    if (!ctx->reader.fp) {
        fprintf(stderr, "mp4_open: cannot open '%s'\n", filename);
        return -1;
    }

    printf("==========================================\n");
    printf("  MP4 Sequential Demuxer\n");
    printf("  File: %s\n", filename);
    printf("==========================================\n\n");

    int moov_found = 0;

    while (!feof(ctx->reader.fp) && !ferror(ctx->reader.fp)) {
        BoxHdr   h;
        if (read_box_hdr(&ctx->reader, &h) != 0) break;

        uint64_t payload  = box_payload(&h);
        uint64_t pl_start = ctx->reader.pos;

        printf("[TOP]  [%s]  size=%" PRIu64 "  @offset=%" PRIu64 "\n",
               h.type, h.size, pl_start - h.hdr_size);

        /* ---- moov: recursively parse all child boxes ------------- */
        if (strcmp(h.type, "moov") == 0) {
            uint64_t end = (payload == UINT64_MAX) ? UINT64_MAX : pl_start + payload;
            walk_boxes(ctx, end, 1);
            /* Consume any trailing gap inside moov */
            if (payload != UINT64_MAX && ctx->reader.pos < pl_start + payload)
                seq_skip(&ctx->reader, (pl_start + payload) - ctx->reader.pos);
            moov_found = 1;
        }

        /* ---- mdat: verify moov was first, then park and return --- */
        else if (strcmp(h.type, "mdat") == 0) {
            if (!moov_found) {
                fprintf(stderr,
                    "\nFATAL: mdat appears before moov.\n"
                    "This file is NOT FastStart  - the demuxer cannot seek back.\n"
                    "Re-encode with:\n"
                    "  ffmpeg -i input.mp4 -movflags faststart output.mp4\n");
                fclose(ctx->reader.fp);
                ctx->reader.fp = NULL;
                return -1;
            }
            printf("\n[mdat] payload starts at absolute offset %" PRIu64 "\n",
                   ctx->reader.pos);
            return 0;   /* ← success; reader parked at mdat byte 0 */
        }

        /* ---- everything else: skip (ftyp, free, wide, skip, …) -- */
        else {
            if (payload != UINT64_MAX) seq_skip(&ctx->reader, payload);
        }
    }

    fprintf(stderr, "mp4_open: no mdat box found after moov.\n");
    return -1;
}

/* ================================================================== */
/*  stsc expansion                                                     */
/*                                                                     */
/*  Converts the compact run-length stsc table into a flat array      */
/*  where per_chunk[i] = number of samples in chunk i+1.             */
/*                                                                     */
/*  Algorithm:                                                         */
/*    For each chunk (1-indexed), walk the stsc entries in order.     */
/*    Each entry whose first_chunk ≤ chunk_number overwrites the      */
/*    current sample count.  Entries are ascending, so we break as    */
/*    soon as first_chunk > chunk_number.                             */
/*                                                                     */
/*  Caller must free() the returned pointer.                          */
/* ================================================================== */
static uint32_t *expand_stsc(const TrackInfo *t)
{
    uint32_t *per_chunk = (uint32_t *)calloc(t->chunk_count, sizeof(uint32_t));
    if (!per_chunk) return NULL;

    for (uint32_t ci = 0; ci < t->chunk_count; ci++) {
        uint32_t chunk_1idx = ci + 1;   /* stsc uses 1-based chunk numbers */
        uint32_t spc        = 1;         /* safe fallback                   */

        for (uint32_t e = 0; e < t->stsc_entry_count; e++) {
            if (t->stsc_entries[e].first_chunk <= chunk_1idx)
                spc = t->stsc_entries[e].samples_per_chunk;
            else
                break; /* entries are ascending — no later entry can apply */
        }
        per_chunk[ci] = spc;
    }
    return per_chunk;
}

/* ================================================================== */
/*  build_chunk_index                                                  */
/*                                                                     */
/*  Produces the merged, sorted CombinedChunk array used to drain     */
/*  the interleaved mdat box without any seeking:                      */
/*                                                                     */
/*  1. For each track, expand stsc → per-chunk sample counts.         */
/*  2. Fill CombinedChunk entries (file_offset, sample_count,         */
/*     first_sample_id, type, track_idx).                             */
/*  3. qsort() by file_offset — mirrors the physical interleave.      */
/* ================================================================== */

static int cmp_chunks(const void *a, const void *b)
{
    const CombinedChunk *ca = (const CombinedChunk *)a;
    const CombinedChunk *cb = (const CombinedChunk *)b;
    if (ca->file_offset < cb->file_offset) return -1;
    if (ca->file_offset > cb->file_offset) return  1;
    return 0;
}

static CombinedChunk *build_chunk_index(const Mp4Demux *ctx, int *total_out)
{
    *total_out = 0;

    /* Count total chunks across all recognised tracks */
    int total = 0;
    for (int ti = 0; ti < ctx->track_count; ti++)
        if (ctx->tracks[ti].type != TRACK_UNKNOWN)
            total += (int)ctx->tracks[ti].chunk_count;

    if (total == 0) return NULL;

    CombinedChunk *list = (CombinedChunk *)malloc((size_t)total * sizeof *list);
    if (!list) return NULL;

    int idx = 0;
    for (int ti = 0; ti < ctx->track_count; ti++) {
        const TrackInfo *t = &ctx->tracks[ti];
        if (t->type == TRACK_UNKNOWN) continue;

        /* Expand the compact stsc table into a flat per-chunk array */
        uint32_t *spc = expand_stsc(t);
        if (!spc) continue;

        uint32_t sample_cursor = 0;
        for (uint32_t ci = 0; ci < t->chunk_count; ci++) {
            list[idx].file_offset    = t->chunk_offsets[ci];
            list[idx].sample_count   = spc[ci];
            /* first_sample_id = global per-track index of chunk's first sample */
            list[idx].first_sample_id = sample_cursor;
            list[idx].type           = t->type;
            list[idx].track_idx      = ti;

            sample_cursor += spc[ci];
            idx++;
        }
        free(spc);
    }

    /* Sort by absolute file offset — now in physical mdat order */
    qsort(list, (size_t)idx, sizeof *list, cmp_chunks);

    *total_out = idx;
    return list;
}

/* ================================================================== */
/*  Sample size helper (handles uniform and per-sample tables)        */
/* ================================================================== */

static uint32_t get_sample_size(const TrackInfo *t, uint32_t sample_idx)
{
    if (t->default_sample_size != 0)
        return t->default_sample_size;
    if (sample_idx < t->sample_count && t->sample_sizes)
        return t->sample_sizes[sample_idx];
    return 0;
}

/* ================================================================== */
/*  mp4_stream — drain mdat sequentially in sorted chunk order        */
/* ================================================================== */

int mp4_stream(Mp4Demux    *ctx,
               VideoFrameCb  video_cb,
               AudioPacketCb audio_cb,
               void         *userdata)
{
    int            total  = 0;
    CombinedChunk *chunks = build_chunk_index(ctx, &total);
    if (!chunks || total == 0) {
        fprintf(stderr, "mp4_stream: no chunks to process.\n");
        free(chunks);
        return -1;
    }

    printf("\n==========================================\n");
    printf("  Streaming %d interleaved chunks\n", total);
    printf("==========================================\n\n");

    SeqReader *r = &ctx->reader;

    for (int ci = 0; ci < total; ci++) {
        const CombinedChunk *ch = &chunks[ci];
        const TrackInfo     *t  = &ctx->tracks[ch->track_idx];

        /* ---- advance to this chunk's absolute file position ------ */
        if (ch->file_offset > r->pos) {
            /* Gap between chunks (alignment padding, etc.) */
            seq_skip(r, ch->file_offset - r->pos);

        } else if (ch->file_offset < r->pos) {
            /* We've already passed this offset — overlapping layout  */
            /* or moov came after mdat.  Skip rather than corrupt.    */
            fprintf(stderr,
                "WARNING: chunk %" PRId32 " offset %" PRIu64
                " < current pos %" PRIu64 "  - skipping.\n",
                ci, ch->file_offset, r->pos);
            continue;
        }

        printf("  Chunk %d/%d  [%s]  offset=%" PRIu64
               "  samples=%u  first_sample_id=%u\n",
               ci + 1, total,
               ch->type == TRACK_VIDEO ? "VIDEO" : "AUDIO",
               ch->file_offset,
               ch->sample_count,
               ch->first_sample_id);

        /* ---- read and dispatch each sample in this chunk --------- */
        for (uint32_t s = 0; s < ch->sample_count; s++) {
            uint32_t sample_idx = ch->first_sample_id + s;
            uint32_t frame_sz   = get_sample_size(t, sample_idx);

            if (frame_sz == 0) {
                fprintf(stderr,
                    "    WARNING: zero-size sample %u in %s track  - skip\n",
                    sample_idx,
                    ch->type == TRACK_VIDEO ? "video" : "audio");
                continue;
            }

            uint8_t *buf = (uint8_t *)malloc(frame_sz);
            if (!buf) {
                fprintf(stderr, "OOM: sample %u (%u bytes)\n", sample_idx, frame_sz);
                free(chunks);
                return -1;
            }

            if (seq_read(r, buf, frame_sz) != 0) {
                fprintf(stderr,
                    "ERROR: truncated read  - sample %u expected %u bytes\n",
                    sample_idx, frame_sz);
                free(buf);
                free(chunks);
                return -1;
            }

            /* Dispatch to the caller's decode callback */
            if      (ch->type == TRACK_VIDEO && video_cb)
                video_cb(buf, frame_sz, userdata);
            else if (ch->type == TRACK_AUDIO && audio_cb)
                audio_cb(buf, frame_sz, userdata);

            free(buf);
        }
    }

    printf("\n==========================================\n");
    printf("  Streaming complete.\n");
    printf("==========================================\n");

    free(chunks);
    return 0;
}

/* ================================================================== */
/*  mp4_close                                                         */
/* ================================================================== */

void mp4_close(Mp4Demux *ctx)
{
    if (ctx->reader.fp) {
        fclose(ctx->reader.fp);
        ctx->reader.fp = NULL;
    }
    for (int i = 0; i < ctx->track_count; i++) {
        TrackInfo *t = &ctx->tracks[i];
        free(t->chunk_offsets);
        free(t->sample_sizes);
        free(t->stsc_entries);
    }
    memset(ctx, 0, sizeof *ctx);
}
