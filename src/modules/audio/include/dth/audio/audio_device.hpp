#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dth_audio/export_dth_audio.hpp"

namespace dth::audio {
class DTH_AUDIO_EXPORT AudioDevice
{
  public:
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::string_view host_api() const noexcept;

    [[nodiscard]] bool supports_input() const noexcept;
    [[nodiscard]] bool supports_output() const noexcept;

    [[nodiscard]] std::uint32_t max_input_channels() const noexcept;
    [[nodiscard]] std::uint32_t max_output_channels() const noexcept;

    [[nodiscard]] std::vector<double> supported_sample_rates() const;

  private:
    friend class AudioSystem;

    std::uint32_t _id;
    std::string   _name;
    std::string   _host_api;
    // Additional metadata.
};
}  // namespace dth::audio
