#!/usr/bin/env bash
# Plays a stock cat clip through a v4l2loopback virtual webcam and runs far-sight on it.
# Footage: "Ginger Cat Walks on Sunny Kitchen Counter" by Alex Moliski from Pexels
# https://www.pexels.com/video/ginger-cat-walks-on-sunny-kitchen-counter-35674403/
# Usage: ./run-demo.sh [extra far-sight options]
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"

device="${DEVICE:-/dev/video10}"
bin="${FARSIGHT_BIN:-$root/build/far-sight}"
model="${MODEL:-$root/models/SmolVLM-500M-Instruct-Q8_0.gguf}"
mmproj="${MMPROJ:-$root/models/mmproj-SmolVLM-500M-Instruct-Q8_0.gguf}"
media="$here/media"
clip="$media/ginger-cat-kitchen-counter.mp4"
demo="$media/demo.mp4"
output="${OUTPUT:-$here/captures}"
clip_url="https://www.pexels.com/download/video/35674403/"

die() { echo "error: $*" >&2; exit 1; }

command -v ffmpeg >/dev/null || die "ffmpeg is required (sudo apt install ffmpeg)"
[[ -x "$bin" ]] || die "far-sight binary not found at $bin; build it first (see the top-level README)"
if [[ ! -f "$model" || ! -f "$mmproj" ]]; then
    "$root/scripts/download-model.sh"
fi

if [[ ! -e "$device" ]]; then
    die "$device does not exist. Create the virtual webcam first:
    sudo modprobe v4l2loopback video_nr=${device#/dev/video} card_label=\"far-sight demo\" exclusive_caps=1"
fi
if command -v v4l2-ctl >/dev/null && ! v4l2-ctl -d "$device" --info 2>/dev/null | grep -q "v4l2 loopback"; then
    die "$device is not a v4l2loopback device; refusing to write video into it"
fi

mkdir -p "$media"
if [[ -n "${CLIP:-}" ]]; then
    [[ -f "$CLIP" ]] || die "CLIP file not found: $CLIP"
    demo="$CLIP"
elif [[ ! -f "$clip" ]]; then
    echo "downloading demo clip (Alex Moliski / Pexels)..."
    curl -fL --progress-bar -A "Mozilla/5.0" -o "$clip.part" "$clip_url" \
        || die "download failed; save the clip from the Pexels page as $clip and rerun"
    mv "$clip.part" "$clip"
fi

# The clip opens with the cat already on the counter, so prepend its empty-counter ending.
if [[ -z "${CLIP:-}" && ( ! -f "$demo" || "$clip" -nt "$demo" ) ]]; then
    echo "preparing demo video..."
    ffmpeg -v error -y -i "$clip" -filter_complex \
        "[0:v]scale=1280:720,split[s1][s2];[s1]trim=11:15.1,setpts=PTS-STARTPTS[a];[s2]trim=0:15.1,setpts=PTS-STARTPTS[b];[a][b]concat=n=2:v=1[out]" \
        -map "[out]" -an -c:v libx264 -preset fast -crf 20 "$demo"
fi

ffmpeg -v error -re -stream_loop -1 -i "$demo" -vf scale=1280:720 -pix_fmt yuyv422 -f v4l2 "$device" &
feeder=$!
trap 'kill "$feeder" 2>/dev/null || true' EXIT

# With exclusive_caps=1 the device only appears as a camera once something is writing to it.
sleep 2

echo "far-sight is watching $device; press Ctrl-C to stop. Captures go to $output"
"$bin" -m "$model" --mmproj "$mmproj" \
    -d "$device" \
    -p "a cat on the kitchen counter" \
    --interval 1s --consecutive 2 --cooldown 15s \
    -o "$output" \
    "$@"
