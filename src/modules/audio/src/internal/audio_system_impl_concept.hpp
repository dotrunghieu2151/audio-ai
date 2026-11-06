#pragma once

#include <concepts>
#include <optional>
#include <vector>

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_device.hpp"
#include "dth/audio/audio_stream.hpp"
#include "dth/audio/stream_config.hpp"

namespace dth::audio {
template <typename T>
concept AudioSystemImpl = requires(
    T& impl, const AudioDevice& inputDevice, const AudioDevice& outputDevice,
    const InputStreamConfig& inputConfig, InputCallback inputCallback,
    const OutputStreamConfig& outputConfig, OutputCallback outputCallback,
    const DuplexStreamConfig& duplexConfig, DuplexCallback duplexCallback, void* userData) {
    { impl.devices() } -> std::same_as<std::vector<AudioDevice>>;
    { impl.default_input_device() } -> std::same_as<std::optional<AudioDevice>>;
    { impl.default_output_device() } -> std::same_as<std::optional<AudioDevice>>;
    {
        impl.make_stream(inputDevice, inputConfig, inputCallback, userData)
    } -> std::same_as<AudioStream>;
    {
        impl.make_stream(outputDevice, outputConfig, outputCallback, userData)
    } -> std::same_as<AudioStream>;
    {
        impl.make_stream(inputDevice, outputDevice, duplexConfig, duplexCallback, userData)
    } -> std::same_as<AudioStream>;
};
}  // namespace dth::audio