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
| `samples/hdr10p_gop12_sei_048.h265` | **realistic case**: 12 frames, one CVS with B-frames (AUD-delimited), HDR10+ SEI in AUs 0, 4, 8 | `[1,0,0,0,1,0,0,0,1,0,0,0]` |
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

## Findings (develop 8f922ed34, RK3576 kernel 6.1.75 and RK3566 kernel 5.10)

The parser parses every SEI (`h265d_debug=8704` shows `hdr_meta_index`
0..3). The defect is in the **attachment of the parsed metadata to
frames**, and it is format- and packetization-dependent.

**1. User-visible at FFmpeg level with a realistic stream.**
`hdr10p_gop12_sei_048.h265` (single CVS, B-frames, HDR10+ SEI in AUs
0/4/8), ffprobe per-frame side data, deterministic over repeated runs:

```
hevc_rkmpp: 6/12 frames carry dynamic metadata  (pattern 100011011010)
hevc (sw) : 12/12 frames
```

**2. All-intra streams mask it at FFmpeg level** (`hevc_rkmpp` 6/6,
same as software), but the MPP API level (`hdr_probe`, AU-by-AU feeding,
with or without parser fast mode) still shows wrong attachment:

| Stream | MPP API frames with meta (of 6) |
|---|---|
| CUVA Vivid, SEI in AUs 0/2/4 | 4 - first AU's SEI dropped, refresh works (A/B payloads verified) |
| CUVA Vivid, single SEI (any AU) | 0 - a lone SEI is never attached |
| HDR10+, SEI in AUs 0/2/4 | 0 in probe usage, while FFmpeg usage gets 6/6 |

**3. Packetization-dependent:** the same stream gives different counts
with AU-by-AU packets vs 8 KB chunks vs FFmpeg's packetization.

Expected behavior: every frame whose AU carries the SEI gets that AU's
metadata (inheritance to following frames is fine), independent of how
the caller packets the input; in particular the first AU's SEI and lone
SEIs must not be lost, and a normal-GOP stream must not drop metadata on
half of its frames.

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

- MPP develop v1.0.12 (8f922ed34) on RK3566 (kernel 5.10) and RK3576
  (kernel 6.1.75), cross-built develop library: identical, deterministic
  AU-wise probe result on both SoCs (frames 1-2 absent, 3-6 present; A/B
  refresh works), with and without parser fast mode.
- The manifestation depends on packetization:

  | Feeding | Frames with dynamic meta (of 6) |
  |---|---|
  | AU-by-AU packets (`hdr_probe`) | 4 (frames 1-2 absent) |
  | 8 KB chunks (mpi_dec_test style) | 6 |
  | FFmpeg `hevc_rkmpp` (nyanmisaka/ffmpeg-rockchip PR #268 build) | 6 |

  FFmpeg's current packetization masks the defect at that level; the
  underlying SEI->frame association is still wrong and packetization-
  dependent (the first AU's SEI is lost with clean per-AU packets).
