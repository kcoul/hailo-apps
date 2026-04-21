# Simple Whisper Chat

A minimal C++ example for transcribing WAV audio with HailoRT's native GenAI `Speech2Text` API.

## Features

- Uses the C++ HailoRT GenAI API directly
- Resolves Whisper HEFs through the shared `resources_config.yaml`
- Supports `--list-models`
- Loads 16-bit PCM WAV audio and converts it to normalized `float32`

## Build

```bash
cd hailo_apps/cpp/simple_whisper_chat
cmake -S . -B build
cmake --build build
```

If the repo was cloned without C++ third-party submodules, either:

- initialize `hailo_apps/cpp/external/{yaml-cpp,curl}`, or
- configure with `-DHAILO_USE_SYSTEM_DEPS=ON` and provide installed `yaml-cpp` and `libcurl`.

## Usage

```bash
./simple_whisper_chat --audio /path/to/audio.wav
```

Optional arguments:

- `--hef-path PATH`: custom HEF path or model name from `resources_config.yaml`
- `--list-models`: print available Whisper models for the connected device
- `--audio PATH`: WAV file to transcribe

If `--audio` is omitted, the app looks for `audio.wav` next to the executable.

## Audio Format

This example currently expects:

- WAV container
- PCM format
- 16-bit samples

The samples are converted to normalized `float32` before being passed to HailoRT.
