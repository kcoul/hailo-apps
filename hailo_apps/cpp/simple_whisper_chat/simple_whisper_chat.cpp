/**
 * Copyright (c) 2020-2026 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
#include "resources_manager.hpp"
#include "hailo/genai/speech2text/speech2text.hpp"
#include "hailo/hailort.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kAppName = "whisper_chat";
constexpr std::string_view kSharedVDeviceGroupId = "SHARED";

struct Args {
    std::string hef_path;
    std::string audio_path;
    bool list_models = false;
};

struct WavAudioData {
    std::vector<float> samples;
    uint16_t channels = 0;
    uint16_t sample_width_bytes = 0;
    uint32_t sample_rate = 0;
    uint32_t frames = 0;
};

template<typename T>
T read_le(std::istream &stream)
{
    T value{};
    stream.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!stream) {
        throw std::runtime_error("Unexpected end of WAV file");
    }
    return value;
}

fs::path get_executable_dir()
{
#ifdef _WIN32
    char buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (0 == length || MAX_PATH == length) {
        throw std::runtime_error("Failed to locate executable directory");
    }
    return fs::path(std::string(buffer, length)).parent_path();
#else
    return fs::read_symlink("/proc/self/exe").parent_path();
#endif
}

Args parse_args(int argc, char **argv)
{
    Args args;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if ("--hef-path" == arg) {
            if ((i + 1) >= argc) {
                throw std::runtime_error("Missing value for --hef-path");
            }
            args.hef_path = argv[++i];
        } else if ("--audio" == arg) {
            if ((i + 1) >= argc) {
                throw std::runtime_error("Missing value for --audio");
            }
            args.audio_path = argv[++i];
        } else if ("--list-models" == arg) {
            args.list_models = true;
        } else if ("-h" == arg || "--help" == arg) {
            std::cout
                << "Simple Whisper Speech-to-Text Example\n\n"
                << "Usage:\n"
                << "  simple_whisper_chat [--hef-path PATH] [--audio PATH] [--list-models]\n\n"
                << "Options:\n"
                << "  --hef-path PATH   Path to a HEF file or model name from resources_config.yaml\n"
                << "  --audio PATH      Path to audio WAV file (default: audio.wav next to executable)\n"
                << "  --list-models     List available models for this application and exit\n"
                << "  -h, --help        Show this help message\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + std::string(arg));
        }
    }

    return args;
}

std::string resolve_audio_path(const std::string &audio_path)
{
    if (!audio_path.empty()) {
        return fs::absolute(audio_path).string();
    }

    return (get_executable_dir() / "audio.wav").string();
}

WavAudioData load_wav_as_float32_pcm16(const fs::path &audio_path)
{
    std::ifstream stream(audio_path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Audio file not found: " + audio_path.string());
    }

    char riff[4];
    stream.read(riff, sizeof(riff));
    if (0 != std::memcmp(riff, "RIFF", sizeof(riff))) {
        throw std::runtime_error("Invalid WAV file: missing RIFF header");
    }

    (void)read_le<uint32_t>(stream); // File size - unused.

    char wave[4];
    stream.read(wave, sizeof(wave));
    if (0 != std::memcmp(wave, "WAVE", sizeof(wave))) {
        throw std::runtime_error("Invalid WAV file: missing WAVE header");
    }

    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    std::vector<uint8_t> pcm_bytes;

    while (stream) {
        char chunk_id[4];
        stream.read(chunk_id, sizeof(chunk_id));
        if (!stream) {
            break;
        }

        const uint32_t chunk_size = read_le<uint32_t>(stream);
        const std::streampos chunk_data_start = stream.tellg();

        if (0 == std::memcmp(chunk_id, "fmt ", sizeof(chunk_id))) {
            audio_format = read_le<uint16_t>(stream);
            channels = read_le<uint16_t>(stream);
            sample_rate = read_le<uint32_t>(stream);
            (void)read_le<uint32_t>(stream); // byte_rate
            (void)read_le<uint16_t>(stream); // block_align
            bits_per_sample = read_le<uint16_t>(stream);
        } else if (0 == std::memcmp(chunk_id, "data", sizeof(chunk_id))) {
            pcm_bytes.resize(chunk_size);
            stream.read(reinterpret_cast<char*>(pcm_bytes.data()), static_cast<std::streamsize>(chunk_size));
            if (!stream) {
                throw std::runtime_error("Failed reading WAV data chunk");
            }
        }

        stream.seekg(chunk_data_start + static_cast<std::streamoff>(chunk_size));
        if (chunk_size % 2 != 0) {
            stream.seekg(1, std::ios::cur);
        }
    }

    if (1 != audio_format) {
        throw std::runtime_error("Unsupported WAV format: only PCM is supported");
    }
    if (16 != bits_per_sample) {
        throw std::runtime_error("Unsupported WAV format: only 16-bit PCM is supported");
    }
    if (pcm_bytes.empty()) {
        throw std::runtime_error("Invalid WAV file: missing data chunk");
    }
    if (0 == channels) {
        throw std::runtime_error("Invalid WAV file: channel count must be greater than zero");
    }
    if (0 == sample_rate) {
        throw std::runtime_error("Invalid WAV file: sample rate must be greater than zero");
    }

    if (pcm_bytes.size() % sizeof(int16_t) != 0) {
        throw std::runtime_error("Invalid WAV data size");
    }

    const size_t sample_count = pcm_bytes.size() / sizeof(int16_t);
    std::vector<float> float_samples(sample_count);

    const auto *pcm_samples = reinterpret_cast<const int16_t*>(pcm_bytes.data());
    std::transform(pcm_samples, pcm_samples + sample_count, float_samples.begin(),
        [](int16_t sample) {
            return static_cast<float>(sample) / 32768.0f;
        });

    WavAudioData wav_data;
    wav_data.channels = channels;
    wav_data.sample_width_bytes = static_cast<uint16_t>(bits_per_sample / 8);
    wav_data.sample_rate = sample_rate;
    wav_data.frames = static_cast<uint32_t>(sample_count / channels);
    wav_data.samples = std::move(float_samples);
    return wav_data;
}

std::string resolve_hef_path(const std::string &hef_arg, hailo_apps::ResourcesManager &resources)
{
    return resources.resolve_net_arg(std::string(kAppName), hef_arg);
}

std::shared_ptr<hailort::VDevice> create_shared_vdevice()
{
    hailo_vdevice_params_t params{};
    const auto status = hailo_init_vdevice_params(&params);
    if (HAILO_SUCCESS != status) {
        throw hailort::hailort_error(status, "Failed to initialize VDevice params");
    }

    params.group_id = kSharedVDeviceGroupId.data();

    auto vdevice = hailort::VDevice::create_shared(params);
    if (!vdevice) {
        throw hailort::hailort_error(vdevice.status(), "Failed to create VDevice");
    }
    return vdevice.release();
}

} // namespace

int main(int argc, char **argv)
{
    try {
        const Args args = parse_args(argc, argv);
        hailo_apps::ResourcesManager resources;

        if (args.list_models) {
            resources.print_models(std::string(kAppName));
            return HAILO_SUCCESS;
        }

        const std::string hef_path = resolve_hef_path(args.hef_path, resources);
        std::cout << "Using HEF: " << hef_path << '\n';
        std::cout << "Model file found: " << hef_path << "\n\n";

        std::cout << "[1/5] Initializing Hailo device...\n";
        auto vdevice = create_shared_vdevice();
        std::cout << "Hailo device initialized\n";

        std::cout << "[2/5] Loading Whisper model...\n";
        auto speech2text_params = hailort::genai::Speech2TextParams(hef_path);
        auto speech2text = hailort::genai::Speech2Text::create(vdevice, speech2text_params)
            .expect("Failed to create Speech2Text");
        std::cout << "Model loaded successfully\n";

        const fs::path audio_path = resolve_audio_path(args.audio_path);
        std::cout << "[3/5] Loading audio file: " << audio_path.string() << '\n';
        const auto wav_data = load_wav_as_float32_pcm16(audio_path);
        const double duration_sec = static_cast<double>(wav_data.frames) / static_cast<double>(wav_data.sample_rate);
        std::cout << "Audio loaded (duration: " << std::fixed << std::setprecision(2) << duration_sec
                  << "s, sample rate: " << wav_data.sample_rate
                  << "Hz, channels: " << wav_data.channels << ")\n";

        std::cout << "[4/5] Preprocessing audio...\n";
        std::cout << "Audio preprocessed (converted to float32, normalized)\n";

        std::cout << "[5/5] Transcribing audio with Whisper...\n";
        auto generator_params = speech2text.create_generator_params().expect("Failed to create generator params");
        auto status = generator_params.set_task(hailort::genai::Speech2TextTask::TRANSCRIBE);
        if (HAILO_SUCCESS != status) {
            throw hailort::hailort_error(status, "Failed to set speech-to-text task");
        }

        status = generator_params.set_language("en");
        if (HAILO_SUCCESS != status) {
            throw hailort::hailort_error(status, "Failed to set speech-to-text language");
        }

        auto segments = speech2text.generate_all_segments(
            hailort::MemoryView(wav_data.samples.data(), wav_data.samples.size() * sizeof(float)),
            generator_params,
            std::chrono::milliseconds(15000)).expect("Failed to generate transcription");

        if (!segments.empty()) {
            std::string transcription;
            for (const auto &segment : segments) {
                transcription += segment.text;
            }

            std::cout << "\nTranscription completed (" << segments.size() << " segment(s)):\n";
            std::cout << "------------------------------------------------------------\n";
            std::cout << transcription << '\n';
            std::cout << "------------------------------------------------------------\n\n";
            std::cout << "Example completed successfully\n";
        } else {
            std::cout << "\nNo transcription generated\n";
        }

        return HAILO_SUCCESS;
    } catch (const hailort::hailort_error &exception) {
        std::cerr << "Error occurred: " << exception.what() << '\n';
        return exception.status();
    } catch (const std::exception &exception) {
        std::cerr << "Error occurred: " << exception.what() << '\n';
        return HAILO_INTERNAL_FAILURE;
    }
}
