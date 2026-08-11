# MPP issue #966 reproduction material

Repro streams and scripts for
[rockchip-linux/mpp#966](https://github.com/rockchip-linux/mpp/issues/966):
h265d attaches HDR dynamic metadata SEI (HDR10+ / HDR Vivid) only to the
first decoded frame instead of every access unit carrying the SEI.

## Files

| File | Content | Result (hw vs sw) |
|---|---|---|
| `samples/hdr10plus_hevc.h265` | SMPTE ST 2094-40 (HDR10+) HEVC, 6 frames, SEI in every AU - **primary repro** | hw=1 sw=6 |
| `samples/hdr_vivid_hevc_repeat.h265` | CUVA HDR Vivid HEVC, 6 frames (single vivid AU repeated), SEI in every AU - **Vivid repro** | hw=1 sw=6 |
| `samples/hdr_vivid_hevc.h265` | CUVA HDR Vivid HEVC, 1 frame, 1 SEI - control (single SEI works) | hw=1 sw=1 |
| `scripts/compare_hdr_dynamic.sh` | per-frame side data count comparison: `hevc_rkmpp` vs software `hevc` | - |

Provenance:

- `hdr10plus_hevc.h265` is `ToS-s1.h265` from
  [allenk/hdr10plus_parser](https://github.com/allenk/hdr10plus_parser)
  (Tears of Steel excerpt, CC-BY).
- `hdr_vivid_hevc.h265` is the FFmpeg fate-suite CUVA HDR Vivid sample;
  `hdr_vivid_hevc_repeat.h265` is the same access unit concatenated 6 times
  (bit-exact IDR repetition, generated with a 4-line Python script).

Download:

```sh
wget https://raw.githubusercontent.com/DXICM/mpp-hdr-sei-repro/main/samples/hdr10plus_hevc.h265
wget https://raw.githubusercontent.com/DXICM/mpp-hdr-sei-repro/main/samples/hdr_vivid_hevc_repeat.h265
```

## Reproduction 1: native mpi_dec_test (MPP develop v1.0.12, 8f922ed3)

```sh
h265d_debug=8704 ./mpi_dec_test -i hdr10plus_hevc.h265 -t 16777220 -n 6
```

The parser log prints an `hdr_meta` line only for poc 0, although the SEI is
present in all 6 access units (verifiable in any bitstream viewer).

## Reproduction 2: FFmpeg comparison

Build [nyanmisaka/ffmpeg-rockchip](https://github.com/nyanmisaka/ffmpeg-rockchip)
against the affected MPP, then:

```sh
FFPROBE=/path/to/ffprobe sh scripts/compare_hdr_dynamic.sh
```

Expected on a healthy decoder: hw count == sw count per stream.
Affected build (RK3576/RK3588, kernel 5.10/6.1):

```
== hdr10plus_hevc.h265 ==
  [SMPTE2094-40] MISMATCH  hw=1 sw=6
== hdr_vivid_hevc_repeat.h265 ==
  [CUVA] MISMATCH  hw=1 sw=6
== hdr_vivid_hevc.h265 ==
  [CUVA] MATCH  hw=1 sw=1
```

The software decoder exports the dynamic metadata on every frame carrying the
SEI; `hevc_rkmpp` exports it only on the first frame. Streams that carry the
SEI in every AU are only "safe" because frame 0 gets the metadata.

## Environment observed

- MPP develop branch v1.0.12 (8f922ed3)
- RK3576 and RK3588; kernel 5.10 and 6.1
