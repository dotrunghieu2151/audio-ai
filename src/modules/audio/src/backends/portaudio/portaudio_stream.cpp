#include "backends/portaudio/portaudio_stream.hpp"

#include <portaudio.h>

#include <atomic>
#include <iostream>

#include "dth/audio/audio_error.hpp"

namespace dth::audio {

PortAudioStreamHandle::PortAudioStreamHandle(std::shared_ptr<PortAudioBackendRuntime> runtime,
                                             StreamConfig config, StreamCallback callback,
                                             void* userData)
    : _config{config},
      _runtime{std::move(runtime)},
      _userData{userData},
      _callback{std::move(callback)}
{
}

PortAudioStreamHandle::~PortAudioStreamHandle()
{
    if (_state && !is_closed()) {
        const auto result = close();
        if (!result.has_value()) {
            // log error here
            std::cout << "failed to destruct stream";
        }
    }
}

StreamResult PortAudioStreamHandle::validate_pa_stream_input_params(
    const PaStreamParameters& paParams)
{
    if (paParams.device == paNoDevice) {
        return std::unexpected(
            AudioError(AudioErrorCode::device_not_found, "No input device speciified"));
    }
    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(paParams.device);
    if ((deviceInfo == nullptr) || paParams.channelCount > deviceInfo->maxInputChannels) {
        return std::unexpected(AudioError(AudioErrorCode::device_not_found,
                                          "Input device does not support requested channels"));
    }
    return {};
}
StreamResult PortAudioStreamHandle::validate_pa_stream_output_params(
    const PaStreamParameters& paParams)
{
    if (paParams.device == paNoDevice) {
        return std::unexpected(
            AudioError(AudioErrorCode::device_not_found, "No input device speciified"));
    }
    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(paParams.device);
    if ((deviceInfo == nullptr) || paParams.channelCount > deviceInfo->maxOutputChannels) {
        return std::unexpected(AudioError(AudioErrorCode::device_not_found,
                                          "Output device does not support requested channels"));
    }
    return {};
}

StreamResult PortAudioStreamHandle::validate_stream_config(const StreamConfig& config)
{
    if (config.sample_rate <= 0) {
        return std::unexpected(
            AudioError(AudioErrorCode::unsupported_configuration, "Invalid sample rate  config"));
    }

    if (config.input.has_value()) {
        const auto inputValidationResult = validate_pa_stream_input_params(config.input.value());
        if (!inputValidationResult.has_value()) {
            return inputValidationResult;
        }
    }

    if (config.output.has_value()) {
        const auto outputValidationResult = validate_pa_stream_output_params(config.output.value());
        if (!outputValidationResult.has_value()) {
            return outputValidationResult;
        }
    }

    return {};
}

StreamResult PortAudioStreamHandle::open()
{
    if (_stream != nullptr) {
        return {};
    }
    // validate the configs
    const auto validationResult = validate_stream_config(_config);
    if (!validationResult.has_value()) {
        return validationResult;
    }

    const PaError error = Pa_OpenStream(
        &_stream, _config.input.has_value() ? &_config.input.value() : nullptr,
        _config.output.has_value() ? &_config.output.value() : nullptr, _config.sample_rate,
        _config.frames_per_buffer == 0 ? paFramesPerBufferUnspecified : _config.frames_per_buffer,
        _config.stream_flags, &PortAudioStreamHandle::portaudio_callback, this);
    if (error != paNoError) {
        _stream = nullptr;

        return std::unexpected(
            AudioError(AudioErrorCode::stream_open_failed, "Pa_OpenStream failed", error));
    }

    _state->store(StreamState::open, std::memory_order_release);

    return {};
}

StreamResult PortAudioStreamHandle::start()
{
    if (_stream == nullptr) {
        return std::unexpected(AudioError(AudioErrorCode::stream_start_failed,
                                          "Cannot start an uninitialized stream. Open() first"));
    }
    if (is_running()) {
        return {};
    }
    const PaError error = Pa_StartStream(_stream);

    if (error != paNoError) {
        return std::unexpected(
            AudioError(AudioErrorCode::stream_start_failed, "Pa_StartStream failed", error));
    }

    _state->store(StreamState::running, std::memory_order_release);
    return {};
}
StreamResult PortAudioStreamHandle::stop()
{
    if (_stream == nullptr) {
        return {};
    }

    if (is_stopped()) {
        return {};
    }

    const PaError error = Pa_StopStream(_stream);

    if (error != paNoError) {
        return std::unexpected(
            AudioError(AudioErrorCode::stream_stop_failed, "Pa_StopStream failed", error));
    }

    _state->store(StreamState::stopped, std::memory_order_release);

    return {};
}
StreamResult PortAudioStreamHandle::close()
{
    if (_stream == nullptr) {
        _state->store(StreamState::closed, std::memory_order_release);
        return {};
    }

    if (is_running()) {
        const PaError stop_error = Pa_StopStream(_stream);

        if (stop_error != paNoError) {
            // Still attempt to close the stream.
        }
    }

    const PaError error = Pa_CloseStream(_stream);

    if (error != paNoError) {
        return std::unexpected(
            AudioError(AudioErrorCode::stream_close_failed, "Pa_CloseStream failed", error));
    }

    _stream = nullptr;

    _state->store(StreamState::closed, std::memory_order_release);

    return {};
}

bool PortAudioStreamHandle::is_open() const noexcept
{
    return _state->load(std::memory_order_acquire) == StreamState::open;
}
bool PortAudioStreamHandle::is_running() const noexcept
{
    return _state->load(std::memory_order_acquire) == StreamState::running;
}
bool PortAudioStreamHandle::is_stopped() const noexcept
{
    return _state->load(std::memory_order_acquire) == StreamState::stopped;
}

bool PortAudioStreamHandle::is_closed() const noexcept
{
    return _state->load(std::memory_order_acquire) == StreamState::closed;
}

int PortAudioStreamHandle::portaudio_callback(const void* input, void* output,
                                              unsigned long                   frame_count,
                                              const PaStreamCallbackTimeInfo* time_info,
                                              PaStreamCallbackFlags           status_flags,
                                              void*                           user_data) noexcept
{
    auto* self = static_cast<PortAudioStreamHandle*>(user_data);
    return CallBackToPaStreamResultMapping::map(
        self->_callback(input, output, frame_count, time_info, status_flags, self->_userData));
}
}  // namespace dth::audio
