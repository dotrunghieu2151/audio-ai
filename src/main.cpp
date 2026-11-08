#include <algorithm>
#include <atomic>
#include <csignal>

#include "app-window.h"
#include "dth/audio/audio_system.hpp"
#include "dth/audio/stream_config.hpp"
#include "dth/common/timer.hpp"
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
        if (device.id == 4) {
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

    dth::common::Timer timer{};
    auto               stream = audioSystem.make_stream(
        inputDevice.value(), outputDevice.value(), dth::audio::DuplexStreamConfig{},
        [](void*, dth::audio::DuplexAudioBufferView buffer) noexcept {
            for (auto data : buffer.input.samples()) {
                std::fill_n(buffer.output.data, buffer.output.channels, data);
                buffer.output.data += buffer.output.channels;
            }
            return dth::audio::CallbackResult::continue_stream;
        },
        nullptr);

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
    while (keep_running && stream.is_running()) {
        // spdlog::info("Recording...");
    }
    spdlog::info("hi there {}", timer.stop_ms());
    return 0;
}
