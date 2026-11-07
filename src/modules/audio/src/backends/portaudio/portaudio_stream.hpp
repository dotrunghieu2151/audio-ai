#pragma once

#include <portaudio.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "backends/portaudio/portaudio_backend_runtime.hpp"
#include "dth/audio/audio-types.hpp"
#include "dth/common/enum_mapping.hpp"

namespace dth::audio {

class PortAudioStreamHandle final
{
  public:
    struct StreamConfig
    {
        std::optional<PaStreamParameters> input{std::nullopt};
        std::optional<PaStreamParameters> output{std::nullopt};
        double                            sample_rate{0.0};
        unsigned long                     frames_per_buffer{0};
        PaStreamFlags                     stream_flags{paNoFlag};
    };

    using StreamCallback =
        std::function<CallbackResult(const void* input, void* output, unsigned long frameCount,
                                     const PaStreamCallbackTimeInfo* timeInfo,
                                     PaStreamCallbackFlags statusFlags, void* userData)>;

  private:
    using CallBackToPaStreamResultMapping = dth::common::EnumMap<
        CallbackResult, PaStreamCallbackResult,
        std::pair{CallbackResult::continue_stream, PaStreamCallbackResult::paContinue},
        std::pair{CallbackResult::stop_stream, PaStreamCallbackResult::paAbort},
        std::pair{CallbackResult::complete_stream, PaStreamCallbackResult::paComplete}>;

  public:
    ~PortAudioStreamHandle();

    PortAudioStreamHandle(std::shared_ptr<PortAudioBackendRuntime> runtime, StreamConfig config,
                          StreamCallback callback, void* userData);

    PortAudioStreamHandle(const PortAudioStreamHandle&)            = delete;
    PortAudioStreamHandle& operator=(const PortAudioStreamHandle&) = delete;

    PortAudioStreamHandle(PortAudioStreamHandle&&)            = default;
    PortAudioStreamHandle& operator=(PortAudioStreamHandle&&) = default;

    StreamResult open();
    StreamResult start();
    StreamResult stop();
    StreamResult close();

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_stopped() const noexcept;
    [[nodiscard]] bool is_closed() const noexcept;

  private:
    [[nodiscard]] static StreamResult validate_stream_config(const StreamConfig& config);
    [[nodiscard]] static StreamResult validate_pa_stream_input_params(
        const PaStreamParameters& paParams);
    [[nodiscard]] static StreamResult validate_pa_stream_output_params(
        const PaStreamParameters& paParams);

    StreamConfig                              _config;
    std::shared_ptr<PortAudioBackendRuntime>  _runtime;
    std::unique_ptr<std::atomic<StreamState>> _state{
        std::make_unique<std::atomic<StreamState>>(StreamState::closed)};
    PaStream*      _stream{nullptr};
    void*          _userData{nullptr};
    StreamCallback _callback;

    static int portaudio_callback(const void* input, void* output, unsigned long frame_count,
                                  const PaStreamCallbackTimeInfo* time_info,
                                  PaStreamCallbackFlags status_flags, void* user_data) noexcept;
};
}  // namespace dth::audio
