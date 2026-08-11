#!/bin/sh
# Compare per-frame HDR dynamic metadata side data between the MPP hardware
# decoder (hevc_rkmpp) and the FFmpeg software decoder (hevc).
#
# Usage: sh compare_hdr_dynamic.sh /path/to/ffmpeg|ffprobe [max_frames]
#   FFMPEG=/path/ffmpeg sh compare_hdr_dynamic.sh
#
# Expected on a healthy build: hw count == sw count.
# Affected MPP (develop v1.0.12): hw == 1, sw == N for streams whose
# dynamic-meta SEI repeats across access units.

FFPROBE="${FFPROBE:-ffprobe}"
[ -n "$1" ] && FFPROBE="$1"
MAX_FRAMES="${2:-${HDR_MAX_FRAMES:-300}}"
HDR_DIR="$(dirname "$0")/../samples"

count_side_data() {
    "$FFPROBE" -v error -c:v "$2" -read_intervals "%+#$MAX_FRAMES" -i "$1" \
        -show_frames -select_streams v:0 \
        -show_entries "side_data=side_data_type" -of csv=p=0 2>/dev/null \
        | grep -c "$3"
}

for f in "$HDR_DIR"/*.h265; do
    [ -f "$f" ] || continue
    name=$(basename "$f")
    echo "== $name (first $MAX_FRAMES frames) =="
    for t in "HDR Dynamic Metadata SMPTE2094-40" "HDR Dynamic Metadata CUVA"; do
        hw=$(count_side_data "$f" hevc_rkmpp "$t")
        sw=$(count_side_data "$f" hevc "$t")
        tag=${t#HDR Dynamic Metadata }
        if [ "$hw" -eq 0 ] && [ "$sw" -eq 0 ]; then
            echo "  [$tag] not present in stream"
        elif [ "$hw" -eq "$sw" ]; then
            echo "  [$tag] MATCH  hw=$hw sw=$sw"
        else
            echo "  [$tag] MISMATCH  hw=$hw sw=$sw"
        fi
    done
done
