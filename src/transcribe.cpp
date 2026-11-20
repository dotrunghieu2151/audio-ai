#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <thread>

#include "dth/audio/audio_system.hpp"
#include "dth/audio/audio_transcriber.hpp"
#include "dth/audio/pipeline_config.hpp"
#include "dth/audio/stream_config.hpp"
#include "dth/common/spsc_ring_buffer.hpp"
#include "dth/common/timer.hpp"
#include "spdlog/spdlog.h"

namespace {

constexpr std::uint32_t wav_sample_rate = 16000;
constexpr std::uint16_t wav_channels    = 1;
struct TempBuffer
{
    // ~2 s of headroom for the writer loop.
    dth::common::SpscRingBuffer<float> ring{std::size_t{wav_sample_rate} * wav_channels * 10};
    std::atomic<std::uint64_t>         overruns{0};
};

// Real-time thread: no allocation, locks, IO or syscalls. Ring full => drop + count.
dth::audio::CallbackResult record_callback(void*                       user_data,
                                           dth::audio::AudioBufferView buffer) noexcept
{
    auto&      tempBuffer = *static_cast<TempBuffer*>(user_data);
    auto const samples    = buffer.samples();
    if (tempBuffer.ring.write(samples) < samples.size()) {
        tempBuffer.overruns.fetch_add(1, std::memory_order_relaxed);
    }
    return dth::audio::CallbackResult::continue_stream;
}

// Drains the ring into the file (little-endian host assumed: macOS). Returns bytes written.
void drain(TempBuffer& tempBuffer, dth::audio::AudioTranscriber& transcriber, std::FILE* f)
{
    std::array<float, std::size_t{wav_sample_rate} * wav_channels * 10> chunk{};
    spdlog::info("ENTERING DRAIN TRANSCRIPTION");

    tempBuffer.ring.read(chunk);
    spdlog::info("TRANSCRIBING A SEGMENT {}", chunk.size());
    const auto result = transcriber.transcribe(chunk, wav_sample_rate, 1, chunk.size());
    if (!result.text.empty()) {
        spdlog::info("TRANSCRIBING DONE, SAVING TO FILE {}", result.text.c_str());
        std::fwrite(result.text.data(), sizeof(char), result.text.size(), f);
        const char newline = '\n';
        std::fwrite(&newline, sizeof(char), 1, f);
        spdlog::info("WRITING TO ILE DONE");
    }
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

    TempBuffer        tempBuffer{};
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
    auto stream = audioSystem.make_system_audio_stream(
        record_callback, &tempBuffer,
        dth::audio::PipelineConfig{
            .processing =
                dth::audio::ProcessingFlags::resample | dth::audio::ProcessingFlags::remap_channels,
            .target = {.sample_rate = wav_sample_rate, .channels = wav_channels},
        });
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
    while (keep_running && stream.is_running()) {
        drain(tempBuffer, asr, transcriptionFile);

        spdlog::info("Transcribed done for segment. Sleeping");
        std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    }
    (void)stream.stop();  // no callback after this returns
    drain(tempBuffer, asr, transcriptionFile);
    std::fclose(transcriptionFile);

    spdlog::info("{} overruns in {} s", tempBuffer.overruns.load(), timer.stop_ms() / 1e6);
    return 0;
}
