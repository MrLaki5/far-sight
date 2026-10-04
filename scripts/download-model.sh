#!/usr/bin/env bash
# Fetch a small GGUF vision model + projector into ./models.
set -euo pipefail

repo="${1:-ggml-org/SmolVLM-500M-Instruct-GGUF}"
model="${2:-SmolVLM-500M-Instruct-Q8_0.gguf}"
mmproj="${3:-mmproj-SmolVLM-500M-Instruct-Q8_0.gguf}"
dest="$(dirname "$0")/../models"

mkdir -p "$dest"
for f in "$model" "$mmproj"; do
    if [[ -f "$dest/$f" ]]; then
        echo "have $f"
    else
        echo "downloading $f"
        curl -fL --progress-bar -o "$dest/$f.part" "https://huggingface.co/$repo/resolve/main/$f"
        mv "$dest/$f.part" "$dest/$f"
    fi
done
