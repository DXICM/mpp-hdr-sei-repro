# MPP issue #966 reproduction material

Repro streams and scripts for
[rockchip-linux/mpp#966](https://github.com/rockchip-linux/mpp/issues/966):
h265d attaches HDR dynamic metadata SEI (HDR10+ / HDR Vivid) only to the
first decoded frame instead of every access unit carrying the SEI.

**Note (2026-08-17):** the previously uploaded `hdr10plus_hevc.h265`
(ToS-s1.h265) turned out to carry the HDR10+ T.35 SEI **only in the first
AU**; the "6 frames" seen with FFmpeg's software decoder came from its
metadata-persistence behavior, not from the bitstream. It has been replaced
by the streams below, in which the SEI really is present in several AUs
(bitstream-verified per AU).

## Files

| File | Content | T.35 SEI per AU (bitstream) |
|---|---|---|
| `samples/hdr10plus_multi_sei.h265` | 6 all-intra frames; HDR10+ (ST 2094-40) SEI injected in AUs 0, 2, 4 - **primary repro** | `[1,0,1,0,1,0]` |
| `samples/hdr_vivid_multi_sei.h265` | same base stream; CUVA HDR Vivid SEI injected in AUs 0, 2, 4 - **Vivid repro** | `[1,0,1,0,1,0]` |
| `samples/hdr_vivid_hevc.h265` | fate-suite CUVA HDR Vivid, 1 frame / 1 SEI - control (single SEI works) | `[1]` |
| `scripts/gen_multi_sei_samples.py` | regenerates both multi-SEI streams bit-exactly (needs ffmpeg + libx265) | - |
| `scripts/compare_hdr_dynamic.sh` | per-frame side data count comparison: `hevc_rkmpp` vs software `hevc` | - |

Provenance: the HDR10+ SEI NAL is extracted from `ToS-s1.h265`
([allenk/hdr10plus_parser](https://github.com/allenk/hdr10plus_parser),
Tears of Steel excerpt, CC-BY); the Vivid SEI NAL from the FFmpeg fate-suite
vivid sample. The video base is a synthetic `testsrc2` encode (Main10,
all-intra, 6 frames), so the streams contain no licensed content.

Download:

```sh
wget https://raw.githubusercontent.com/DXICM/mpp-hdr-sei-repro/main/samples/hdr10plus_multi_sei.h265
wget https://raw.githubusercontent.com/DXICM/mpp-hdr-sei-repro/main/samples/hdr_vivid_multi_sei.h265
```

## Reproduction 1: native mpi_dec_test (MPP develop v1.0.12, 8f922ed3)

```sh
h265d_debug=8704 ./mpi_dec_test -i hdr10plus_multi_sei.h265 -t 16777220 -n 6
```

The `hdr_meta` log line appears only for poc 0, although AUs 2 and 4 carry
the same SEI as well.

## Reproduction 2: FFmpeg comparison

Build [nyanmisaka/ffmpeg-rockchip](https://github.com/nyanmisaka/ffmpeg-rockchip)
against the affected MPP, then:

```sh
FFPROBE=/path/to/ffprobe sh scripts/compare_hdr_dynamic.sh
```

Affected build (RK3576/RK3588, kernel 5.10/6.1): `hevc_rkmpp` exports the
dynamic metadata only on frame 0, while the bitstream carries it in AUs
0/2/4. Note FFmpeg's software decoder *persists* the last parsed dynamic
metadata onto following frames, so its count is >= the bitstream count; the
authoritative reference is the per-AU bitstream table above.

Expected behavior: the decoder attaches the SEI of each AU to the
corresponding `MppFrame`, so frames 0, 2 and 4 carry the metadata (frames
1/3/5 may legitimately inherit or not - the point is frames 2 and 4 must not
be limited to frame 0's values, which matters when per-AU metadata differs).

## Environment observed

- MPP develop branch v1.0.12 (8f922ed3)
- RK3576 and RK3588; kernel 5.10 and 6.1
