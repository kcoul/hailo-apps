/**
 * Copyright (c) 2020-2026 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
#include "resources_manager.hpp"
#include "hailo/genai/speech2text/speech2text.hpp"
#include "hailo/hailort.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <portaudio.h>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kAppName = "whisper_chat";
constexpr std::string_view kSharedVDeviceGroupId = "SHARED";
constexpr double kTargetSampleRate = 16000.0;
constexpr unsigned long kTargetChunkFrames = 1024;

struct Args {
    std::string hef_path;
    std::string language = "en";
    double chunk_duration_sec = 5.0;
    bool list_models = false;
};

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
        } else if ("--language" == arg) {
            if ((i + 1) >= argc) {
                throw std::runtime_error("Missing value for --language");
            }
            args.language = argv[++i];
        } else if ("--chunk-duration" == arg) {
            if ((i + 1) >= argc) {
                throw std::runtime_error("Missing value for --chunk-duration");
            }
            args.chunk_duration_sec = std::stod(argv[++i]);
        } else if ("--list-models" == arg) {
            args.list_models = true;
        } else if ("-h" == arg || "--help" == arg) {
            std::cout
                << "Realtime Whisper Speech-to-Text Example\n\n"
                << "Usage:\n"
                << "  realtime_whisper_chat [--hef-path PATH] [--language ISO639-1]\n"
                << "                        [--chunk-duration SECONDS] [--list-models]\n\n"
                << "Options:\n"
                << "  --hef-path PATH         Path to a HEF file or model name from resources_config.yaml\n"
                << "  --language CODE         Language hint for transcription (default: en)\n"
                << "  --chunk-duration SEC    Live transcription chunk duration in seconds (default: 5.0)\n"
                << "  --list-models           List available models for this application and exit\n"
                << "  -h, --help              Show this help message\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + std::string(arg));
        }
    }

    if (args.chunk_duration_sec <= 0.25) {
        throw std::runtime_error("--chunk-duration must be greater than 0.25 seconds");
    }

    return args;
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

std::vector<float> resample_linear(const std::vector<float> &input, double input_rate, double output_rate)
{
    if (input.empty()) {
        return {};
    }
    if (std::abs(input_rate - output_rate) < 1e-3) {
        return input;
    }

    const double ratio = output_rate / input_rate;
    const size_t output_size = static_cast<size_t>(std::llround(static_cast<double>(input.size()) * ratio));
    std::vector<float> output(output_size);

    for (size_t i = 0; i < output_size; ++i) {
        const double source_index = static_cast<double>(i) / ratio;
        const size_t left_index = static_cast<size_t>(source_index);
        const size_t right_index = std::min(left_index + 1, input.size() - 1);
        const double fraction = source_index - static_cast<double>(left_index);
        const double interpolated = (1.0 - fraction) * input[left_index] + fraction * input[right_index];
        output[i] = static_cast<float>(interpolated);
    }

    return output;
}

class PortAudioSystem final {
public:
    PortAudioSystem()
    {
        const auto status = Pa_Initialize();
        if (paNoError != status) {
            throw std::runtime_error("Failed to initialize PortAudio: " + std::string(Pa_GetErrorText(status)));
        }
    }

    ~PortAudioSystem()
    {
        Pa_Terminate();
    }

    PortAudioSystem(const PortAudioSystem &) = delete;
    PortAudioSystem &operator=(const PortAudioSystem &) = delete;
};

class MicrophoneCapture final {
public:
    explicit MicrophoneCapture(double requested_sample_rate)
    {
        m_device_index = Pa_GetDefaultInputDevice();
        if (paNoDevice == m_device_index) {
            throw std::runtime_error("No default microphone input device found");
        }

        const PaDeviceInfo *device_info = Pa_GetDeviceInfo(m_device_index);
        if (nullptr == device_info) {
            throw std::runtime_error("Failed to query default microphone device");
        }

        m_effective_sample_rate = requested_sample_rate;
        m_chunk_frames = kTargetChunkFrames;

        PaStreamParameters input_parameters{};
        input_parameters.device = m_device_index;
        input_parameters.channelCount = 1;
        input_parameters.sampleFormat = paFloat32;
        input_parameters.suggestedLatency = device_info->defaultLowInputLatency;
        input_parameters.hostApiSpecificStreamInfo = nullptr;

        auto status = Pa_OpenStream(
            &m_stream,
            &input_parameters,
            nullptr,
            requested_sample_rate,
            kTargetChunkFrames,
            paClipOff,
            &MicrophoneCapture::stream_callback,
            this);

        if (paNoError != status) {
            const double fallback_rate = (device_info->defaultSampleRate > 0.0) ? device_info->defaultSampleRate : requested_sample_rate;
            status = Pa_OpenStream(
                &m_stream,
                &input_parameters,
                nullptr,
                fallback_rate,
                static_cast<unsigned long>(std::max(1.0, std::round(kTargetChunkFrames * fallback_rate / requested_sample_rate))),
                paClipOff,
                &MicrophoneCapture::stream_callback,
                this);

            if (paNoError != status) {
                throw std::runtime_error("Failed to open microphone stream: " + std::string(Pa_GetErrorText(status)));
            }

            m_effective_sample_rate = fallback_rate;
            m_chunk_frames = static_cast<unsigned long>(std::max(1.0, std::round(kTargetChunkFrames * fallback_rate / requested_sample_rate)));
        }

        m_device_name = device_info->name ? device_info->name : "Unknown input";
    }

    ~MicrophoneCapture()
    {
        stop();
        if (nullptr != m_stream) {
            Pa_CloseStream(m_stream);
            m_stream = nullptr;
        }
    }

    void start()
    {
        if (m_running.exchange(true)) {
            return;
        }

        m_stop_requested.store(false);
        const auto status = Pa_StartStream(m_stream);
        if (paNoError != status) {
            m_running.store(false);
            throw std::runtime_error("Failed to start microphone stream: " + std::string(Pa_GetErrorText(status)));
        }
    }

    void stop()
    {
        if (!m_running.exchange(false)) {
            return;
        }

        m_stop_requested.store(true);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_data_available = true;
        }
        m_condition.notify_all();

        const auto status = Pa_StopStream(m_stream);
        if (paNoError != status && paStreamIsStopped != status) {
            std::cerr << "Warning: failed to stop microphone stream cleanly: " << Pa_GetErrorText(status) << '\n';
        }
    }

    std::vector<float> wait_and_pop_chunk(size_t target_samples)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait(lock, [&]() {
            return m_stop_requested.load() || m_pending_samples.size() >= target_samples || m_data_available;
        });

        m_data_available = false;

        if (m_pending_samples.empty()) {
            return {};
        }

        if ((m_pending_samples.size() < target_samples) && !m_stop_requested.load()) {
            return {};
        }

        const size_t sample_count = std::min(target_samples, m_pending_samples.size());
        std::vector<float> chunk(m_pending_samples.begin(), m_pending_samples.begin() + static_cast<std::ptrdiff_t>(sample_count));
        m_pending_samples.erase(m_pending_samples.begin(), m_pending_samples.begin() + static_cast<std::ptrdiff_t>(sample_count));
        return chunk;
    }

    std::vector<float> flush_remaining_samples()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<float> remaining;
        remaining.swap(m_pending_samples);
        return remaining;
    }

    bool stop_requested() const
    {
        return m_stop_requested.load();
    }

    double effective_sample_rate() const
    {
        return m_effective_sample_rate;
    }

    unsigned long chunk_frames() const
    {
        return m_chunk_frames;
    }

    const std::string &device_name() const
    {
        return m_device_name;
    }

private:
    static int stream_callback(const void *input_buffer, void *, unsigned long frames_per_buffer,
        const PaStreamCallbackTimeInfo *, PaStreamCallbackFlags, void *user_data)
    {
        auto *capture = static_cast<MicrophoneCapture*>(user_data);
        capture->append_input(static_cast<const float*>(input_buffer), frames_per_buffer);
        return capture->m_stop_requested.load() ? paComplete : paContinue;
    }

    void append_input(const float *samples, unsigned long frame_count)
    {
        if ((nullptr == samples) || (0 == frame_count)) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending_samples.insert(m_pending_samples.end(), samples, samples + frame_count);
        m_data_available = true;
        m_condition.notify_one();
    }

    double m_effective_sample_rate = 0.0;
    unsigned long m_chunk_frames = 0;
    PaDeviceIndex m_device_index = paNoDevice;
    PaStream *m_stream = nullptr;
    std::string m_device_name;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop_requested{false};
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::vector<float> m_pending_samples;
    bool m_data_available = false;
};

std::string transcribe_chunk(hailort::genai::Speech2Text &speech2text,
    const std::vector<float> &audio_chunk, const std::string &language)
{
    if (audio_chunk.empty()) {
        return {};
    }

    auto generator_params = speech2text.create_generator_params().expect("Failed to create generator params");
    auto status = generator_params.set_task(hailort::genai::Speech2TextTask::TRANSCRIBE);
    if (HAILO_SUCCESS != status) {
        throw hailort::hailort_error(status, "Failed to set speech-to-text task");
    }

    if (!language.empty()) {
        status = generator_params.set_language(language);
        if (HAILO_SUCCESS != status) {
            throw hailort::hailort_error(status, "Failed to set speech-to-text language");
        }
    }

    auto text = speech2text.generate_all_text(
        hailort::MemoryView(const_cast<float*>(audio_chunk.data()), audio_chunk.size() * sizeof(float)),
        generator_params,
        std::chrono::milliseconds(15000)).expect("Failed to generate transcription");
    return text;
}

void run_live_session(hailort::genai::Speech2Text &speech2text, const Args &args)
{
    PortAudioSystem portaudio;
    MicrophoneCapture capture(kTargetSampleRate);
    const size_t target_samples = static_cast<size_t>(std::llround(args.chunk_duration_sec * capture.effective_sample_rate()));

    std::cout << "Default microphone: " << capture.device_name() << '\n';
    std::cout << "Capture sample rate: " << std::llround(capture.effective_sample_rate()) << " Hz\n";
    if (std::abs(capture.effective_sample_rate() - kTargetSampleRate) > 1e-3) {
        std::cout << "Audio will be resampled to 16000 Hz before transcription\n";
    }
    std::cout << "Live chunk duration: " << std::fixed << std::setprecision(1) << args.chunk_duration_sec << "s\n";
    std::cout << "\nPress Enter to start recording. Press Enter again to stop. Type 'q' to quit.\n";

    while (true) {
        std::string command;
        std::cout << "\nReady > ";
        if (!std::getline(std::cin, command)) {
            break;
        }
        if ("q" == command || "Q" == command) {
            break;
        }

        capture.start();
        std::cout << "Recording... transcription will print every " << args.chunk_duration_sec << " seconds.\n";

        std::atomic<bool> session_done{false};
        std::thread processor([&]() {
            size_t chunk_index = 0;
            while (!session_done.load()) {
                auto captured_chunk = capture.wait_and_pop_chunk(target_samples);
                if (captured_chunk.empty()) {
                    if (capture.stop_requested()) {
                        break;
                    }
                    continue;
                }

                if (std::abs(capture.effective_sample_rate() - kTargetSampleRate) > 1e-3) {
                    captured_chunk = resample_linear(captured_chunk, capture.effective_sample_rate(), kTargetSampleRate);
                }

                const auto text = transcribe_chunk(speech2text, captured_chunk, args.language);
                if (!text.empty()) {
                    ++chunk_index;
                    std::cout << "[chunk " << chunk_index << "] " << text << '\n';
                }
            }

            auto tail = capture.flush_remaining_samples();
            if (!tail.empty()) {
                if (std::abs(capture.effective_sample_rate() - kTargetSampleRate) > 1e-3) {
                    tail = resample_linear(tail, capture.effective_sample_rate(), kTargetSampleRate);
                }

                const auto text = transcribe_chunk(speech2text, tail, args.language);
                if (!text.empty()) {
                    std::cout << "[final] " << text << '\n';
                }
            }
        });

        std::string stop_line;
        std::getline(std::cin, stop_line);
        capture.stop();
        session_done.store(true);
        if (processor.joinable()) {
            processor.join();
        }
        std::cout << "Recording stopped.\n";
    }
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

        auto vdevice = create_shared_vdevice();
        auto speech2text_params = hailort::genai::Speech2TextParams(hef_path);
        auto speech2text = hailort::genai::Speech2Text::create(vdevice, speech2text_params)
            .expect("Failed to create Speech2Text");

        run_live_session(speech2text, args);
        return HAILO_SUCCESS;
    } catch (const hailort::hailort_error &exception) {
        std::cerr << "Error occurred: " << exception.what() << '\n';
        return exception.status();
    } catch (const std::exception &exception) {
        std::cerr << "Error occurred: " << exception.what() << '\n';
        return HAILO_INTERNAL_FAILURE;
    }
}
