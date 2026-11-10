#pragma once

#include <cstdint>
#include <type_traits>

#include "dth/audio/audio_format.hpp"

namespace dth::audio {

// Bitset operators for enums that opt in via enable_flag_operators.
template <typename E>
inline constexpr bool enable_flag_operators = false;

template <typename E>
concept FlagEnum = std::is_enum_v<E> && enable_flag_operators<E>;

template <FlagEnum E>
[[nodiscard]] constexpr E operator|(E a, E b) noexcept
{
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(static_cast<U>(a) | static_cast<U>(b)));
}

template <FlagEnum E>
[[nodiscard]] constexpr E operator&(E a, E b) noexcept
{
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(static_cast<U>(a) & static_cast<U>(b)));
}

template <FlagEnum E>
[[nodiscard]] constexpr E operator~(E a) noexcept
{
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(~static_cast<U>(a)));
}

template <FlagEnum E>
constexpr E& operator|=(E& a, E b) noexcept
{
    return a = a | b;
}

template <FlagEnum E>
constexpr E& operator&=(E& a, E b) noexcept
{
    return a = a & b;
}

template <FlagEnum E>
[[nodiscard]] constexpr bool has_flag(E flags, E flag) noexcept
{
    return (flags & flag) == flag;
}

// Processing a pipeline stream inserts between its source and its sink / callback. All of it runs
// on the source's real-time thread.
enum class ProcessingFlags : std::uint8_t {
    none           = 0,       // raw: the consumer gets the source's native format
    resample       = 1u << 0, // to PipelineConfig::target.sample_rate (fixed ratio)
    remap_channels = 1u << 1, // to PipelineConfig::target.channels (mono <-> N, else copy / zero)
};

template <>
inline constexpr bool enable_flag_operators<ProcessingFlags> = true;

struct PipelineConfig
{
    ProcessingFlags processing = ProcessingFlags::none;
    // Only the fields of the enabled flags are used.
    AudioFormat target{};
};

// AudioSystem::make_output_sink options.
enum class OutputSinkFlags : std::uint8_t {
    none = 0,
    // Open the device at its native rate / channel count and convert internally. Off: open it at
    // the input format and let the backend / OS convert.
    convert_format = 1u << 0,
    // Track the producer / device clock drift through the ring fill level (adjusts the resampler
    // ratio). Requires convert_format. Off: fixed ratio; the ring slowly drifts, under/overruns
    // are absorbed by pre-roll and dropping.
    drift_compensation = 1u << 1,
};

template <>
inline constexpr bool enable_flag_operators<OutputSinkFlags> = true;

struct OutputSinkConfig
{
    OutputSinkFlags flags = OutputSinkFlags::convert_format | OutputSinkFlags::drift_compensation;
};

}  // namespace dth::audio
