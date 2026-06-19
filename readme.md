# mp4-demux

A minimal, fully sequential MP4 demuxer written in pure C99.  
No `fseek`. No external dependencies beyond `minimp3.h` (single header).

---

## Design features

| Feature | Description |
|---|---|
| **No `fseek`** | All I/O goes through `SeqReader`; only `fread` is called |
| **Big-Endian** | Every multi-byte field decoded via explicit `<<` bit-shifts — no `ntohl`, no platform assumptions |
| **FastStart guard** | `mp4_open()` aborts with a clear error if `mdat` is found before `moov` |
| **Both tracks** | `active_track` context pointer routes parsed tables to the correct `TrackInfo` slot |
| **stsc expansion** | `expand_stsc()` materialises the run-length table into a flat per-chunk array |
| **Interleaved mdat** | `build_chunk_index()` sorts chunks by `file_offset`; `mp4_stream()` drains them in one pass |

---

## Quick start

```bash
# 1. Fetch the single-header minimp3 library (once)
make fetch-minimp3

# 2. Build
make

# 3. Create a FastStart test file (requires ffmpeg) and run
make test

# — or run against your own file —
./mp4demux your_movie.mp4
```

### Not FastStart? Convert first:

```bash
ffmpeg -i your_video.mp4 -c copy -movflags faststart your_video_fast.mp4
```

---

## Architecture

```
mp4_open(filename)
│
│  Top-level scan (ftyp/free → skip | moov → walk | mdat → park & return)
│
└─ walk_boxes(end_pos, depth)          ← purely sequential, never seeks
      │
      └─ dispatch_box()
            │
            ├─ "trak"   → push TrackInfo slot
            │      └─ walk_boxes() [recursive]
            │              │
            │              ├─ "mdia" / "minf" / "stbl" / "dinf" → recurse
            │              │
            │              ├─ "hdlr" → parse_hdlr()   → sets TrackType (vide/soun)
            │              ├─ "stsc" → parse_stsc()   → StscEntry[] run table
            │              ├─ "stsz" → parse_stsz()   → per-sample size array
            │              ├─ "stco" → parse_stco()   → 32-bit chunk offsets
            │              ├─ "co64" → parse_co64()   → 64-bit chunk offsets
            │              └─ *      → seq_skip(payload)
            │
            └─ pop TrackInfo slot


mp4_stream(video_cb, audio_cb)
│
├─ build_chunk_index()
│     │
│     ├─ for each track:
│     │     expand_stsc()            ← run-length → flat per_chunk[i] = sample count
│     │     fill CombinedChunk[]     ← file_offset, sample_count, first_sample_id, type
│     │
│     └─ qsort(by file_offset)       ← mirrors physical interleave in mdat
│
└─ for each CombinedChunk (in offset order):
      ├─ seq_skip(chunk.file_offset − r->pos)    ← close any gap
      └─ for s in [0, sample_count):
              seq_read(sample_sizes[first_sample_id + s])
              → video_cb() or audio_cb()
```

---

## stsc expansion

The `stsc` box is run-length encoded.  A file with 1000 video chunks might
store only 2 entries:

```
first_chunk=1   samples_per_chunk=5
first_chunk=999 samples_per_chunk=1
```

meaning "chunks 1–998 hold 5 samples each; chunks 999–1000 hold 1 sample."

`expand_stsc()` converts this into a flat `uint32_t per_chunk[chunk_count]`
array, walking each entry in ascending order and applying the last matching
`first_chunk ≤ chunk_1idx` value:

```c
for each chunk ci (1-indexed):
    for each stsc entry e in order:
        if entry.first_chunk <= ci:   spc = entry.samples_per_chunk
        else:                         break
    per_chunk[ci-1] = spc
```

---

## Interleaved mdat drainage

An interleaved MP4 typically stores chunks in physical order:

```
[mdat]
  [video chunk 0]  [audio chunk 0]  [video chunk 1]  [audio chunk 1] …
```

The demuxer handles this by:

1. Building one `CombinedChunk` per chunk for every track.
2. `qsort`-ing by `file_offset` — the sort order now matches the physical layout.
3. Walking the sorted list sequentially:
   - If `chunk.file_offset > r->pos` → `seq_skip()` the gap (padding, etc.)
   - If `chunk.file_offset < r->pos` → the file is NOT FastStart → warning + skip
   - Read each sample, dispatch to callback, advance `r->pos`.

---

## Box reference

| Box | Action |
|---|---|
| `ftyp` `free` `wide` `skip` | Skipped (`seq_skip` payload) |
| `moov` | Recursed |
| `trak` | Opens a new `TrackInfo` slot; recursed; slot closed on exit |
| `mdia` `minf` `stbl` `dinf` `udta` `edts` | Recursed |
| `hdlr` | Sets `TrackInfo.type` → `TRACK_VIDEO` or `TRACK_AUDIO` |
| `stsc` | Loads compact sample-to-chunk run table |
| `stsz` | Loads per-sample byte sizes (or records the uniform size) |
| `stco` | Loads 32-bit absolute chunk offsets |
| `co64` | Loads 64-bit absolute chunk offsets (>4 GB files) |
| `mdat` | `mp4_open` parks reader here; `mp4_stream` drains it |
| *anything else* | Silently skipped |

---

## Adding an AAC decoder

The common case for modern MP4 files is AAC audio.  Swap the body of
`on_audio()` in `main.c`:

```c
// fdk-aac example
UINT in_sz = size;
UCHAR *in_ptr = (UCHAR *)data;
aacDecoder_Fill(haac, &in_ptr, &in_sz, &in_sz);
INT_PCM pcm[2048 * 2];
aacDecoder_DecodeFrame(haac, pcm, sizeof pcm / sizeof *pcm, 0);
```

```c
// faad2 example
NeAACDecFrameInfo frame_info;
void *pcm = NeAACDecDecode(faad_handle, &frame_info, data, size);
```

The demuxer layer (`mp4_demux.c`) is unchanged — only the callback changes.

---


