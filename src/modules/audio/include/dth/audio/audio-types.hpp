#pragma once

#include <cstdint>
#include <expected>

#include "dth/audio/audio_buffer.hpp"
#include "dth/audio/audio_error.hpp"

namespace dth::audio {
enum class CallbackResult : std::uint8_t { continue_stream, stop_stream, complete_stream };
enum class StreamState : std::uint8_t { open, stopped, running, stopped_by_error, closed };

using StreamResult = std::expected<void, AudioError>;

using InputCallback = CallbackResult (*)(void* user_data, AudioBufferView) noexcept;

using OutputCallback = CallbackResult (*)(void* user_data, MutableAudioBufferView) noexcept;

using DuplexCallback = CallbackResult (*)(void* user_data, DuplexAudioBufferView) noexcept;
}  // namespace dth::audio
