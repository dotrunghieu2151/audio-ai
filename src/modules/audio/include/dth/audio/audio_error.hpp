#pragma once

#include <cstdint>
#include <optional>
#include <string>
namespace dth::audio {
enum class AudioErrorCode : std::uint8_t {
    initialization_failed,
    device_not_found,
    unsupported_configuration,
    stream_open_failed,
    stream_start_failed,
    stream_stop_failed,
    stream_close_failed,
    backend_error
};

struct AudioError
{
    AudioErrorCode code;
    std::string    message;

    // Optional backend-specific error code.
    std::optional<int> native_code;
};
}  // namespace dth::audio
