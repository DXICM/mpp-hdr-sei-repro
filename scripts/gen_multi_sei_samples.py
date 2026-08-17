#!/usr/bin/env python3
"""Regenerate the multi-SEI repro streams for rockchip-linux/mpp#966.

Encodes a 6-frame all-intra Main10 HEVC stream (x265 via ffmpeg), then
injects a genuine HDR dynamic metadata T.35 SEI into access units 0, 2 and 4
only. Requires: ffmpeg with libx265, python3.

Usage: python3 gen_multi_sei_samples.py [outdir]
"""
import re
import subprocess
import sys
import os

OUTDIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "samples")

# HDR10+ (SMPTE ST 2094-40) SEI NAL, annexb; payload type 4, country 0xB5,
# provider 0x003C - extracted from ToS-s1.h265 (allenk/hdr10plus_parser).
HDR10PLUS_SEI = bytes.fromhex(
    "0000014e010440b5003c0001040140000c808b4c41ff1bd601036408000c28db"
    "205000acc800e190036e581032d02a6af848f318e1b4004044102509a6ae5c83"
    "50ddf98ec7bd0080")

# CUVA HDR Vivid SEI NAL, annexb; payload type 4 - extracted from the
# FFmpeg fate-suite vivid sample (samples/hdr_vivid_hevc.h265 in this repo).
VIVID_SEI = bytes.fromhex(
    "0000014e01042f260004000501000be65fffffeb4ab3331198005288011c0000"
    "ffbfeff041599988cc002944008e00007fdff7fa261980")


def encode_base(path):
    subprocess.run([
        "ffmpeg", "-v", "error", "-y",
        "-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=25",
        "-frames:v", "6", "-pix_fmt", "yuv420p10le",
        "-color_primaries", "bt2020", "-color_trc", "smpte2084",
        "-colorspace", "bt2020nc",
        "-c:v", "libx265",
        "-x265-params", "keyint=1:scenecut=0:no-open-gop:crf=40:log-level=error",
        path], check=True)


def split_aus(data):
    starts = [m.start() for m in re.finditer(b"\x00\x00\x01", data)]
    pos = []
    for s in starts:
        if s > 0 and data[s - 1] == 0 and (not pos or pos[-1] != s - 1):
            pos.append(s - 1)
        elif not pos or pos[-1] != s:
            pos.append(s)
    nals = []
    for k, s in enumerate(pos):
        e = pos[k + 1] if k + 1 < len(pos) else len(data)
        p = s + (4 if data[s + 2] == 0 else 3)
        nals.append(((data[p] >> 1) & 0x3F, s, e))
    aus, cur = [], []
    for t, s, e in nals:
        cur.append((t, s, e))
        if t < 32:  # AU ends at its VCL NAL
            aus.append(cur)
            cur = []
    if cur:
        aus.append(cur)
    return aus


def inject(data, aus, sei, out, at=(0, 2, 4)):
    buf = b""
    for n, au in enumerate(aus):
        if n in at:
            buf += sei
        for _, s, e in au:
            buf += data[s:e]
    with open(out, "wb") as f:
        f.write(buf)
    print("wrote", out)


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    base = os.path.join(OUTDIR, "base_6f.h265")
    encode_base(base)
    data = open(base, "rb").read()
    aus = split_aus(data)
    assert len(aus) == 6, len(aus)
    inject(data, aus, HDR10PLUS_SEI, os.path.join(OUTDIR, "hdr10plus_multi_sei.h265"))
    inject(data, aus, VIVID_SEI, os.path.join(OUTDIR, "hdr_vivid_multi_sei.h265"))
    os.unlink(base)


if __name__ == "__main__":
    main()
