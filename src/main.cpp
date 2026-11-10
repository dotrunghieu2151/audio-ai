#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <thread>

#include "app-window.h"
#include "dth/audio/audio_system.hpp"
#include "dth/audio/pipeline_config.hpp"
#include "dth/audio/stream_config.hpp"
#include "dth/common/spsc_ring_buffer.hpp"
#include "dth/common/timer.hpp"
#include "spdlog/spdlog.h"

namespace {

// Recorded format. PipelineConfig converts the system audio to it, so the WAV header is known
// before the stream opens.
constexpr std::uint32_t wav_sample_rate = 48000;
constexpr std::uint16_t wav_channels    = 2;

// Bridges the real-time callback (producer) and the file writer loop (consumer).
struct WavRecorder
{
    // ~2 s of headroom for the writer loop.
    dth::common::SpscRingBuffer<float> ring{std::size_t{wav_sample_rate} * wav_channels * 2};
    std::atomic<std::uint64_t>         overruns{0};
};

// Real-time thread: no allocation, locks, IO or syscalls. Ring full => drop + count.
dth::audio::CallbackResult record_callback(void* user_data, dth::audio::AudioBufferView buffer) noexcept
{
    auto&      recorder = *static_cast<WavRecorder*>(user_data);
    auto const samples  = buffer.samples();
    if (recorder.ring.write(samples) < samples.size()) {
        recorder.overruns.fetch_add(1, std::memory_order_relaxed);
    }
    return dth::audio::CallbackResult::continue_stream;
}

void put_u16(std::FILE* f, std::uint16_t v)
{
    std::array<unsigned char, 2> const b{static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
    std::fwrite(b.data(), 1, b.size(), f);
}

void put_u32(std::FILE* f, std::uint32_t v)
{
    std::array<unsigned char, 4> const b{static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                                         static_cast<unsigned char>(v >> 16),
                                         static_cast<unsigned char>(v >> 24)};
    std::fwrite(b.data(), 1, b.size(), f);
}

// 44-byte RIFF header, IEEE float 32-bit. Sizes patched by finish_wav().
void write_wav_header(std::FILE* f, std::uint32_t data_bytes)
{
    std::fwrite("RIFF", 1, 4, f);
    put_u32(f, 36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put_u32(f, 16);  // fmt chunk size
    put_u16(f, 3);   // WAVE_FORMAT_IEEE_FLOAT
    put_u16(f, wav_channels);
    put_u32(f, wav_sample_rate);
    put_u32(f, wav_sample_rate * wav_channels * sizeof(float));  // byte rate
    put_u16(f, wav_channels * sizeof(float));                    // block align
    put_u16(f, 32);                                              // bits per sample
    std::fwrite("data", 1, 4, f);
    put_u32(f, data_bytes);
}

// Drains the ring into the file (little-endian host assumed: macOS). Returns bytes written.
std::uint64_t drain(WavRecorder& recorder, std::FILE* f)
{
    std::array<float, 4096> chunk{};
    std::uint64_t           bytes = 0;
    while (std::size_t const n = recorder.ring.read(chunk)) {
        bytes += std::fwrite(chunk.data(), sizeof(float), n, f) * sizeof(float);
    }
    return bytes;
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
    // {
    //     auto ui = AppWindow::create();

    //     ui->on_request_increase_value([&] { ui->set_counter(ui->get_counter() + 1); });

    //     ui->run();
    // }  // ui is destroyed here
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

    char const* const wav_path = argc > 1 ? argv[1] : "system_audio.wav";
    std::FILE*        wav      = std::fopen(wav_path, "wb");
    if (wav == nullptr) {
        spdlog::info("Cannot open {}", wav_path);
        return 1;
    }
    write_wav_header(wav, 0);

    WavRecorder        recorder{};
    dth::common::Timer timer{};
    auto               stream = audioSystem.make_system_audio_stream(
        record_callback, &recorder,
        dth::audio::PipelineConfig{
                          .processing = dth::audio::ProcessingFlags::resample |
                                        dth::audio::ProcessingFlags::remap_channels,
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
    std::uint64_t data_bytes = 0;
    while (keep_running && stream.is_running()) {
        data_bytes += drain(recorder, wav);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    (void)stream.stop();  // no callback after this returns
    data_bytes += drain(recorder, wav);

    // WAV sizes are 32-bit: > 4 GiB (~3 h at 48 kHz stereo float) is truncated in the header.
    std::rewind(wav);
    write_wav_header(wav, static_cast<std::uint32_t>(std::min<std::uint64_t>(data_bytes, UINT32_MAX - 36)));
    std::fclose(wav);

    spdlog::info("Wrote {} ({} bytes, {} overruns) in {} ms", wav_path, data_bytes,
                 recorder.overruns.load(), timer.stop_ms());
    return 0;
}
