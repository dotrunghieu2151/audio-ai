#pragma once

#include <portaudio.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "backends/portaudio/portaudio_backend_runtime.hpp"
#include "dth/audio/audio-types.hpp"
#include "dth/common/enum_mapping.hpp"
#include "internal/stream_handle.hpp"
namespace dth::audio {

class PortAudioStreamHandle final : public IAudioStreamHandle
{
  private:
    using StreamCallback =
        std::function<CallbackResult(const void* input, void* output, unsigned long frameCount,
                                     const PaStreamCallbackTimeInfo* timeInfo,
                                     PaStreamCallbackFlags statusFlags, void* userData)>;

    using CallBackToPaStreamResultMapping = dth::common::EnumMap<
        CallbackResult, PaStreamCallbackResult,
        std::pair{CallbackResult::continue_stream, PaStreamCallbackResult::paContinue},
        std::pair{CallbackResult::stop_stream, PaStreamCallbackResult::paAbort},
        std::pair{CallbackResult::complete_stream, PaStreamCallbackResult::paComplete}>;

    struct StreamConfig
    {
        std::optional<PaStreamParameters> input;
        std::optional<PaStreamParameters> output;
        float                             sample_rate{0};
        unsigned long                     frames_per_buffer{0};
        PaStreamFlags                     stream_flags{paNoFlag};
    };

  public:
    ~PortAudioStreamHandle() override;

    PortAudioStreamHandle(std::shared_ptr<PortAudioBackendRuntime> runtime, StreamConfig config,
                          StreamCallback callback, void* userData);

    PortAudioStreamHandle(const PortAudioStreamHandle&)            = delete;
    PortAudioStreamHandle& operator=(const PortAudioStreamHandle&) = delete;

    PortAudioStreamHandle(PortAudioStreamHandle&&)            = delete;
    PortAudioStreamHandle& operator=(PortAudioStreamHandle&&) = delete;

    StreamResult open() override;
    StreamResult start() override;
    StreamResult stop() override;
    StreamResult close() override;

    [[nodiscard]] bool is_open() const noexcept override;
    [[nodiscard]] bool is_running() const noexcept override;
    [[nodiscard]] bool is_stopped() const noexcept override;

  private:
    [[nodiscard]] static StreamResult validate_stream_config(const StreamConfig& config);
    [[nodiscard]] static StreamResult validate_pa_stream_input_params(
        const PaStreamParameters& paParams);
    [[nodiscard]] static StreamResult validate_pa_stream_output_params(
        const PaStreamParameters& paParams);

    StreamConfig                             _config;
    std::shared_ptr<PortAudioBackendRuntime> _runtime;
    std::atomic<StreamState>                 _state{StreamState::closed};
    PaStream*                                _stream{nullptr};
    void*                                    _userData{nullptr};
    StreamCallback                           _callback;

    static int portaudio_callback(const void* input, void* output, unsigned long frame_count,
                                  const PaStreamCallbackTimeInfo* time_info,
                                  PaStreamCallbackFlags status_flags, void* user_data) noexcept;
};
}  // namespace dth::audio
