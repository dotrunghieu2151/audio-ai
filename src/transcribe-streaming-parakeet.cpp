#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <thread>

#include "dth/audio/audio_system.hpp"
#include "dth/audio/audio_transcriber.hpp"
#include "dth/audio/audio_translator.hpp"
#include "dth/common/timer.hpp"
#include "live_translation.hpp"
#include "spdlog/spdlog.h"

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
    const auto options = demo::parse_options(argc, argv);
    if (!options) {
        demo::print_usage(argv[0]);
        return 1;
    }

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

    const demo::FilePtr transcription_file = demo::open_append(options->transcription_path);
    const demo::FilePtr translation_file   = demo::open_append(options->translation_path);
    if (!transcription_file || !translation_file) {
        return 1;
    }

    dth::common::Timer timer{};

    spdlog::info("Loading model");
    dth::audio::AudioTranscriber asr({
        .model      = "parakeet_tdt",
        .model_path = "assets/models/parakeet-tdt-0.6b-v3-q8_0.gguf",
        .backend    = dth::audio::AudioTranscriber::Config::Backend::Metal,
        .device     = 0,
        .threads    = 8,
        // parakeet_tdt streaming is buffered, not cache-aware: each window re-encodes left +
        // center + right context. Measured word error rate (LibriSpeech clips, audio.cpp v0.9.5):
        //   center/right/left 2/2/10 (default): 31% on 22 s, 43% on 44 s
        //   center/right/left 2/2/5 (below):   12% on 22 s, 25% on 44 s (35% through VAD +
        //                                       resampling); 1/1/5 and 3/2/5: ~39%
        //   offline_per_utterance (transcribe.cpp): 1.5%
        // Errors grow with stream length. Use this only when live partials matter more than
        // accuracy; otherwise transcribe.cpp (parakeet offline) or nemotron (transcribe-streaming).
        .session_options =
            {
                {"parakeet_tdt.offline_mode", "auto"},
                {"parakeet_tdt.audio_chunk_duration_sec", "2"},  // center, decoded once
                {"parakeet_tdt.right_context_sec", "2"},         // lookahead
                {"parakeet_tdt.left_context_sec", "5"},  // 10 (default) re-reads stale audio
            },
    });
    spdlog::info("Model loaded. Took {} s", timer.stop_ms() / 1e6);

    timer.start();
    dth::audio::AudioTranslator translator({.model_path = options->translator_model});
    spdlog::info("Translator loaded. Took {} s", timer.stop_ms() / 1e6);

    // Declared before the streams that call into it, so it outlives them.
    demo::LiveTranslation output{transcription_file.get(), translation_file.get(),
                                 options->languages.target};
    // Units are translated as soon as their sentence is complete, also mid-utterance.
    auto translation = translator.make_stream({.languages = options->languages},
                                              &demo::LiveTranslation::on_translation, &output);
    output.translate_into(translation);
    timer.start();
    const dth::audio::AudioTranscriber::StreamConfig stream_config{
        .strategy = dth::audio::AudioTranscriber::StreamConfig::Strategy::continuous_stream,
        // Utterances only bound the lines: the stream keeps its own windows.
        .max_utterance_sec = 10.0,
        // Output lag: text appears once center + right context (4 s) has been heard.
        .text_latency_sec = 4.5,
        // --diarize: speaker turns keep each translation unit to one speaker.
        .diarization_model_path = options->diarization_model,
        .speaker_callback       = &demo::LiveTranslation::on_speakers,
    };
    // Converts to the model's mono 16 kHz itself.
    auto stream = asr.make_stream(audioSystem, audioSystem.make_system_audio_source(),
                                  stream_config, &demo::LiveTranslation::on_transcript,
                                  &output);
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
    if (!result.has_value()) {
        spdlog::info("Stream stop error {}", result.error().message);
    }
    // Then the translations of the last utterances.
    const auto translated = translation.stop();
    if (!translated.has_value()) {
        spdlog::info("Translation stop error {}", translated.error().message);
    }
    if (!result.has_value() || !translated.has_value()) {
        return 1;
    }

    spdlog::info("Recorded for {} s", timer.stop_ms() / 1e6);
    return 0;
}
