#pragma once

#include <concepts>
#include <optional>
#include <vector>

#include "dth/audio/audio-types.hpp"

namespace dth::audio {
template <typename T>
concept AudioStreamImpl = requires(T& impl) {
    { impl.is_open() } -> std::same_as<bool>;
    { impl.is_running() } -> std::same_as<bool>;
    { impl.is_stopped() } -> std::same_as<bool>;
    { impl.open() } -> std::same_as<StreamResult>;
    { impl.start() } -> std::same_as<StreamResult>;
    { impl.stop() } -> std::same_as<StreamResult>;
    { impl.close() } -> std::same_as<StreamResult>;
};
}  // namespace dth::audio