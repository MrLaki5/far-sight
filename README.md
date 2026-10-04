# far-sight

Watches a webcam and asks a small local vision-language model whether something you describe is happening, such as "a person waving" or "the garage door is open". When it is, far-sight saves a JPEG of that moment with the capture time in the filename and burned into the image, and appends a row to `events.csv`.

It is a single C++ binary for Linux. It links llama.cpp (including its `libmtmd` multimodal library) statically and captures from the camera through V4L2 directly, so it needs no OpenCV or other runtime dependencies.

## Build

You need CMake 3.20 or newer, a C++17 compiler and git. llama.cpp is fetched automatically at configure time and pinned to a tag through `FARSIGHT_LLAMA_TAG`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The default build runs on the CPU. To use a GPU, pass llama.cpp's backend flags:

```sh
cmake -S . -B build -DGGML_CUDA=ON     # NVIDIA, needs the CUDA toolkit (nvcc)
cmake -S . -B build -DGGML_VULKAN=ON   # Intel/AMD/NVIDIA, needs libvulkan-dev + glslc
```

To build against an existing llama.cpp checkout, add `-DFETCHCONTENT_SOURCE_DIR_LLAMA_CPP=/path/to/llama.cpp`.

## Get a model

```sh
./scripts/download-model.sh   # SmolVLM-500M Q8_0 (~540 MB) into ./models
```

Any llama.cpp-supported vision GGUF works, as long as you have both the model and its `mmproj-*.gguf` projector. Options include SmolVLM 256M/500M/2.2B, Qwen2.5-VL-3B, Gemma 3 4B and Moondream2. Pass the Hugging Face repo and file names as arguments to download a different one.

## Use

Check whether the model understands your prompt on a still image first. The exit code is 0 for a match and 2 for no match:

```sh
./build/far-sight -m models/SmolVLM-500M-Instruct-Q8_0.gguf \
    --mmproj models/mmproj-SmolVLM-500M-Instruct-Q8_0.gguf \
    -p "a newspaper" --image photo.jpg
# p_yes=0.999 match=yes (785 ms)
```

Then watch the camera:

```sh
./build/far-sight -m models/SmolVLM-500M-Instruct-Q8_0.gguf \
    --mmproj models/mmproj-SmolVLM-500M-Instruct-Q8_0.gguf \
    -p "a person waving" --interval 1s --consecutive 2 --cooldown 30s \
    -o captures --on-match 'notify-send "far-sight" "$FARSIGHT_TIME"'
```

Each event writes:

- `captures/2026-10-04_16-14-15.431.jpg`: the full-resolution frame, named after its capture time, with the time stamped in the bottom-left corner. Use `--no-stamp` to leave the image clean.
- A row in `captures/events.csv`: `captured_at` (ISO 8601 with UTC offset), the image name, the score and the prompt.
- A line on stdout, `<time> <path> <score>`, which you can pipe into other tools.

The `--on-match` command runs detached through `/bin/sh`. It receives `FARSIGHT_IMAGE`, `FARSIGHT_TIME`, `FARSIGHT_SCORE` and `FARSIGHT_PROMPT` as environment variables.

Run `far-sight --help` for every option.

## How it decides

1. **Grab the newest frame.** Frames that queued up during the previous check are dropped. The capture time comes from the driver's timestamp, not from when processing finished.
2. **Motion gate.** The frame is shrunk to a 64×48 grayscale thumbnail and compared with the last frame the model saw. If the scene barely changed (`--motion-threshold`), the model is skipped. Every `--max-idle` interval the model runs anyway, and it always runs while a match streak is in progress.
3. **Ask the model.** The frame is downscaled to `--vlm-size` (512 px by default) and the model is asked *"Does this image show &lt;prompt&gt;? Answer with yes or no."* Instead of generating text, far-sight reads the logits for the first answer token and computes `P(yes) / (P(yes) + P(no))`. A prompt ending in `?` is sent as written.
4. **Fire.** An event fires when the score reaches `--threshold` on `--consecutive` checks in a row and the `--cooldown` period has passed since the last event.

Measured on a laptop CPU with SmolVLM-500M Q8_0 at 512 px: about 0.75 s per check.

## Run in the background

```sh
cmake --install build --component far-sight --prefix ~/.local
mkdir -p ~/.local/share/far-sight && cp models/*.gguf ~/.local/share/far-sight/
cp contrib/far-sight.service ~/.config/systemd/user/   # edit the prompt/paths first
systemctl --user enable --now far-sight
journalctl --user -u far-sight -f
```

## Caveats

- **The camera is exclusive.** While far-sight runs, Zoom or Meet usually can't open the camera. To share it, fan the stream out with `v4l2loopback`.
- **The camera LED stays on** the whole time far-sight is running.
- **Low light hurts accuracy.** The first frames after the camera opens are discarded (`--warmup-frames`) while auto-exposure settles.
- **Battery.** Continuous inference uses a lot of power. A longer `--interval` and the motion gate both help.

## License

Licensed under the [Apache License 2.0](LICENSE). You may use, modify and redistribute far-sight, including commercially, provided you keep the copyright notice and the [NOTICE](NOTICE) file crediting this project.
