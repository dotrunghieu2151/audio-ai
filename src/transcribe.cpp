#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <thread>

#include "dth/audio/audio_system.hpp"
#include "dth/audio/audio_transcriber.hpp"
#include "dth/common/timer.hpp"
#include "spdlog/spdlog.h"

namespace {

// Transcriber worker thread: one call per utterance (offline_per_utterance has no partials), so
// each utterance is shown on stdout as soon as it is transcribed.
void on_transcript(void* user_data, const dth::audio::AudioTranscriber::TranscribedSegment& segment)
{
    auto* const file = static_cast<std::FILE*>(user_data);
    spdlog::info("[{:.2f}s - {:.2f}s] {}", segment.start_sec, segment.end_sec, segment.text);
    std::fprintf(file, "[%.2f - %.2f] %s\n", segment.start_sec, segment.end_sec,
                 segment.text.c_str());
    std::fflush(file);
}

}  // namespace

// Use a volatile atomic flag for async-signal safety
std::atomic<bool> keep_running{true};

// Signal handler function
void signalHandler(int signal)
{
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nReceived signal (" << signal << "). Shutting down gracefully...\n";
        keep_running = false;
    }
}

int main(int argc, char** argv)
{
    dth::audio::AudioSystem audioSystem{};
    auto                    inputDevice{audioSystem.default_input_device()};
    auto                    outputDevice{audioSystem.default_output_device()};
    auto                    devices{audioSystem.devices()};
    for (const auto& device : devices) {
        spdlog::info("Device: {} | ID: {} | Mode: {}", device.name, device.id,
                     device.max_input_channels > 0 ? "input" : "output");
        if (device.id == 3) {
            outputDevice = device;
        }
        if (device.id == 0) {
            inputDevice = device;
        }
    }
    if (!inputDevice.has_value() || !outputDevice.has_value()) {
        spdlog::info("No input or output device found");
        return 1;
    }

    spdlog::info("Input device found: {}", inputDevice.value().name);
    spdlog::info("Output device found: {}", outputDevice.value().name);

    char const* const transcription_path = argc > 1 ? argv[1] : "transcription.txt";

    dth::common::Timer timer{};

    std::FILE* transcriptionFile = std::fopen(transcription_path, "a");
    if (transcriptionFile == nullptr) {
        spdlog::info("Cannot open {}", transcription_path);
        return 1;
    }

    spdlog::info("Loading model");
    dth::audio::AudioTranscriber asr({
        .model      = "parakeet_tdt",
        .model_path = "assets/models/parakeet-tdt-0.6b-v3-q8_0.gguf",
        .backend    = dth::audio::AudioTranscriber::Config::Backend::Metal,
        .device     = 0,
        .threads    = 8,
    });
    spdlog::info("Model loaded. Took {} s", timer.stop_ms() / 1e6);
    timer.start();
    const dth::audio::AudioTranscriber::StreamConfig stream_config{
        // Each VAD utterance is transcribed offline on its own.
        .strategy = dth::audio::AudioTranscriber::StreamConfig::Strategy::offline_per_utterance,
        // Utterances end at phrase pauses (300 ms) rather than sentence pauses, for more frequent
        // results. max_utterance_sec stays 20: each forced cut in pause-free speech can lose words
        // at the cut with parakeet, so a lower cap trades accuracy for latency.
        .vad_min_silence_ms = 300,
        // Long utterances end at ever shorter pauses (~300 ms after 5 s, ~200 ms after 10 s)
        // before the 20 s cap forces a cut.
        .vad_adaptive_silence = {{.after_sec = 5.0, .min_silence_ms = 150},
                                 {.after_sec = 10.0, .min_silence_ms = 50}},
    };
    // Converts to the model's mono 16 kHz itself.
    auto stream = asr.make_stream(audioSystem, audioSystem.make_system_audio_source(),
                                  stream_config, on_transcript, transcriptionFile);
    // auto               stream = audioSystem.make_stream(
    //     inputDevice.value(), outputDevice.value(), dth::audio::DuplexStreamConfig{},
    //     [](void*, dth::audio::DuplexAudioBufferView buffer) noexcept {
    //         for (auto data : buffer.input.samples()) {
    //             std::fill_n(buffer.output.data, buffer.output.channels, data);
    //             buffer.output.data += buffer.output.channels;
    //         }
    //         return dth::audio::CallbackResult::continue_stream;
    //     },
    //     nullptr);

    // Register the signal handler for SIGINT (Ctrl+C) and SIGTERM
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    auto result = stream.open();
    if (!result.has_value()) {
        spdlog::info("Stream open error {}", result.error().message);
        return 1;
    }
    result = stream.start();
    if (!result.has_value()) {
        spdlog::info("Stream start error {}", result.error().message);
        return 1;
    }
    spdlog::info("Stream started. Recording..");
    // Transcription runs on the stream's worker thread; this thread only waits for shutdown.
    while (keep_running && stream.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Flushes the tail and delivers the last utterance; no callback after this returns.
    result = stream.stop();
    std::fclose(transcriptionFile);
    if (!result.has_value()) {
        spdlog::info("Stream stop error {}", result.error().message);
        return 1;
    }

    spdlog::info("Recorded for {} s", timer.stop_ms() / 1e6);
    return 0;
}
