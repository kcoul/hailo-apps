# Realtime Whisper Chat

A C++ live-microphone speech-to-text example built on HailoRT's native GenAI `Speech2Text` API and PortAudio.

This example is intended as an upstream-friendly companion to `simple_whisper_chat`:

- microphone input instead of WAV file input
- chunked live transcription in the terminal
- no GUI framework dependency

## Features

- Uses the C++ HailoRT GenAI API directly
- Captures microphone audio with PortAudio
- Prints chunked live transcriptions while recording
- Resolves Whisper HEFs through the shared `resources_config.yaml`
- Supports `--list-models`

## Dependencies

On Debian / Raspberry Pi OS:

```bash
sudo apt install portaudio19-dev
```

You still need the existing C++ dependencies for this repo:

- `yaml-cpp`
- `libcurl`
- `HailoRT`

The simplest path in this repo is usually:

```bash
git submodule update --init --recursive hailo_apps/cpp/external/yaml-cpp hailo_apps/cpp/external/curl
```

## Build

```bash
cd hailo_apps/cpp/realtime_whisper_chat
cmake -S . -B build
cmake --build build
```

If you prefer system packages for `yaml-cpp` and `libcurl`, configure with:

```bash
cmake -S . -B build -DHAILO_USE_SYSTEM_DEPS=ON
```

## Usage

```bash
./build/realtime_whisper_chat
```

Optional arguments:

- `--hef-path PATH`: custom HEF path or model name from `resources_config.yaml`
- `--language CODE`: language hint for transcription, default `en`
- `--chunk-duration SECONDS`: chunk size for live transcription, default `5.0`
- `--list-models`: print available Whisper models for the connected device

## Interaction Model

1. Press Enter to start recording.
2. Speak into the default microphone.
3. The app transcribes fixed-duration chunks while recording.
4. Press Enter again to stop.
5. Type `q` and press Enter to quit.

## Notes

- This is near-realtime chunked transcription, not token-streaming ASR.
- The current implementation targets the same GenAI `Speech2Text` flow as `simple_whisper_chat`.
- If the microphone does not support 16 kHz directly, the app falls back to the device sample rate and resamples before transcription.
