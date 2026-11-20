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

struct TranscriptWriter
{
    std::FILE* file      = nullptr;
    bool       line_open = false;  // an utterance's partials are being written
};

// Transcriber worker thread. Partials are written as they arrive, one line per utterance; the
// final call repeats the utterance's whole text, so it only ends the line.
void on_transcript(void* user_data, const dth::audio::AudioTranscriber::TranscribedSegment& segment)
{
    auto& writer = *static_cast<TranscriptWriter*>(user_data);
    if (segment.is_final) {
        spdlog::info("[{:.2f}s - {:.2f}s] {}", segment.start_sec, segment.end_sec, segment.text);
        if (writer.line_open) {
            std::fputc('\n', writer.file);
            writer.line_open = false;
        }
        else {
            std::fprintf(writer.file, "[%.2f] %s\n", segment.start_sec, segment.text.c_str());
        }
    }
    else {
        if (!writer.line_open) {
            std::fprintf(writer.file, "[%.2f] ", segment.start_sec);
            writer.line_open = true;
        }
        std::fputs(segment.text.c_str(), writer.file);
        std::fputs(segment.text.c_str(), stdout);
        std::fflush(stdout);
    }
    std::fflush(writer.file);
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
    TranscriptWriter writer{.file = transcriptionFile};

    spdlog::info("Loading model");
    dth::audio::AudioTranscriber asr({
        .model      = "nemotron_asr",
        .model_path = "assets/models/nemotron-3.5-asr-streaming-0.6b-q8_0.gguf",
        .backend    = dth::audio::AudioTranscriber::Config::Backend::Metal,
        .device     = 0,
        .threads    = 8,
        // Cache-aware: one streaming session for the whole recording keeps its context; VAD
        // utterances only group its text.
        .stream_strategy = dth::audio::AudioTranscriber::Config::StreamStrategy::continuous_stream,
        // Text arrives as partials anyway; utterances only bound the lines. The stream keeps its
        // context, so shorter utterances cost no accuracy.
        .max_utterance_sec = 10.0,
        // Output lag: 320 ms chunk plus the default 3 x 80 ms lookahead.
        .stream_text_latency_sec = 0.6,
    });
    spdlog::info("Model loaded. Took {} s", timer.stop_ms() / 1e6);
    timer.start();
    // Converts to the model's mono 16 kHz itself.
    auto stream = asr.make_stream(audioSystem, audioSystem.make_system_audio_source(),
                                  on_transcript, &writer);
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
