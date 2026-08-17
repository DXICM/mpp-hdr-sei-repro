# MPP issue #966 reproduction material

Repro streams and tools for
[rockchip-linux/mpp#966](https://github.com/rockchip-linux/mpp/issues/966):
h265d does not reliably associate HDR dynamic metadata SEI (HDR10+ / HDR
Vivid) with the frames whose access units carry the SEI.

**History of this repo:**

1. The first sample (`ToS-s1.h265`) carried the T.35 SEI only in the first
   AU - invalid repro, replaced.
2. The current multi-SEI streams carry the SEI in AUs 0/2/4, verified at the
   bitstream level.
3. Refined finding (2026-08-17, develop 8f922ed34 on RK3566): the parser
   *does* parse every SEI (`h265d_debug=8704` shows `hdr_meta_index` 0, 1, 2
   - three hdr log lines), but the **attachment to frames is wrong and
   timing-dependent**, see "Findings" below.

## Files

| File | Content | T.35 SEI per AU (bitstream) |
|---|---|---|
| `samples/hdr10plus_multi_sei.h265` | 6 all-intra frames; HDR10+ (ST 2094-40) SEI in AUs 0, 2, 4 - **primary repro** | `[1,0,1,0,1,0]` |
| `samples/hdr_vivid_multi_sei.h265` | same base; CUVA HDR Vivid SEI in AUs 0, 2, 4 | `[1,0,1,0,1,0]` |
| `samples/hdr_vivid_ab.h265` | same base; vivid payload **A** in AU0, modified payload **B** in AU2, A in AU4 - shows whether/when metadata is refreshed | `[A,0,B,0,A,0]` |
| `samples/hdr_vivid_hevc.h265` | fate-suite CUVA HDR Vivid, 1 frame / 1 SEI - control | `[1]` |
| `scripts/gen_multi_sei_samples.py` | regenerates all multi-SEI streams bit-exactly (needs ffmpeg + libx265) | - |
| `scripts/hdr_probe.c` | standalone MPP test: feeds AUs one by one (as FFmpeg does) and prints per-frame `mpp_frame_get_hdr_dynamic_meta()`; `-f` enables parser fast mode (rkmppdec default) | - |
| `scripts/compare_hdr_dynamic.sh` | per-frame side data count: `hevc_rkmpp` vs software `hevc` | - |

Provenance: the HDR10+ SEI NAL is extracted from `ToS-s1.h265`
([allenk/hdr10plus_parser](https://github.com/allenk/hdr10plus_parser),
Tears of Steel excerpt, CC-BY); the Vivid SEI NAL from the FFmpeg fate-suite
vivid sample (variant B = same SEI with two payload bytes changed). The
video base is a synthetic `testsrc2` encode (Main10, all-intra, 6 frames),
so the streams contain no licensed content.

## Findings (develop 8f922ed34, RK3566, `hdr_probe`)

Feeding complete AUs one by one (the same packetization FFmpeg's rkmppdec
produces), with or without `MPP_DEC_SET_PARSER_FAST_MODE`:

`hdr_vivid_ab.h265` (SEI A@AU0, B@AU2, A@AU4):

```
frame #1 poc= 0 meta=absent            <- AU0 carries SEI A, but no metadata!
frame #2 poc= 0 meta=absent
frame #3 poc= 0 meta=PRESENT head=...eeee..   <- B appears (AU2 parsed)
frame #4 poc= 0 meta=PRESENT head=...eeee..
frame #5 poc= 0 meta=PRESENT head=...ffff..   <- back to A (AU4 parsed)
frame #6 poc= 0 meta=PRESENT head=...ffff..
```

- The parser parses all three SEIs (refresh works: B shows up at AU2, A
  returns at AU4), so it is not a "parse only the first SEI" problem.
- But the **first AU's metadata is lost**: frame 1, whose AU actually
  carries the SEI, reports no dynamic metadata at all.
- The result is timing-dependent: feeding the same file in 8 KB chunks
  (mpi_dec_test style) instead attaches metadata to all 6 frames, including
  frame 1. Same stream, same MPP, different association.
- `hdr10plus_multi_sei.h265` behaves identically (frames 1-2 absent,
  3-6 present) with AU-wise feeding.

Expected behavior: each frame whose AU carries the SEI gets that AU's
metadata; at minimum frame 1 must not lose AU0's SEI, and the result must
not depend on how the caller chunks the input.

## Reproduction

Native parser log (all pocs are 0 because every AU is its own CVS; count
the hdr log lines, not the pocs):

```sh
h265d_debug=8704 ./mpi_dec_test -i hdr10plus_multi_sei.h265 -t 16777220 -n 6
# -> three "hdr_meta_index" lines (0, 1, 2): all three SEIs are parsed
```

Per-frame attachment:

```sh
gcc scripts/hdr_probe.c -o hdr_probe -lrockchip_mpp
./hdr_probe -f samples/hdr_vivid_ab.h265
```

FFmpeg side data comparison (needs an ffmpeg-rockchip build that exports
dynamic metadata, e.g. nyanmisaka/ffmpeg-rockchip PR #268):

```sh
FFPROBE=/path/to/ffprobe sh scripts/compare_hdr_dynamic.sh
```

Note FFmpeg's software decoder *persists* the last parsed dynamic metadata
onto following frames, so its count is >= the bitstream count; the
authoritative reference is the per-AU bitstream table above.

## Environments observed

- MPP develop v1.0.12 (8f922ed34) on RK3566 (kernel 5.10): attachment bug as
  above, reproduced with the cross-built develop library.
- MPP develop v1.0.12 (8f922ed3) on RK3576 and RK3588 (kernel 5.10/6.1):
  original FFmpeg-level observation (`hevc_rkmpp` exported the metadata on
  only 1 of 6 frames); pending re-verification with the AU-wise probe.
