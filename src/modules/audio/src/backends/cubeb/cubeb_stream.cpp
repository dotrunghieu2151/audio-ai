#include "backends/cubeb/cubeb_stream.hpp"

#include <cubeb/cubeb.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "dth/audio/audio_error.hpp"

namespace dth::audio {

namespace {
// cubeb_stream_init accepts latency_frames in [1, 96000].
constexpr std::uint32_t kMinLatencyFrames = 1;
constexpr std::uint32_t kMaxLatencyFrames = 96000;

cubeb_stream_params make_params(std::uint32_t rate, std::uint32_t channels)
{
    return cubeb_stream_params{
        .format   = CUBEB_SAMPLE_FLOAT32NE,
        .rate     = rate,
        .channels = channels,
        .layout   = CUBEB_LAYOUT_UNDEFINED,
        .prefs    = CUBEB_STREAM_PREF_NONE,
    };
}
}  // namespace

CubebStreamHandle::CubebStreamHandle(std::shared_ptr<CubebBackendRuntime> runtime,
                                     StreamConfig config, StreamCallback callback,
                                     void* userData)
    : _runtime{std::move(runtime)},
      _config{std::move(config)},
      _context{std::make_unique<CallbackContext>()}
{
    _context->callback        = std::move(callback);
    _context->user_data       = userData;
    _context->output_channels = _config.output.has_value() ? _config.output->channels : 0;
}

StreamResult CubebStreamHandle::validate_endpoint(const Endpoint& endpoint, const char* direction)
{
    if (endpoint.owner == nullptr) {
        return std::unexpected(AudioError(AudioErrorCode::device_not_found,
                                          std::string(direction) + " device not found"));
    }
    if (endpoint.channels == 0 || endpoint.channels > endpoint.max_channels) {
        return std::unexpected(
            AudioError(AudioErrorCode::device_not_found,
                       std::string(direction) + " device does not support requested channels"));
    }
    return {};
}

StreamResult CubebStreamHandle::validate_stream_config(const StreamConfig& config)
{
    if (!(config.sample_rate >= 1.0 && config.sample_rate <= 384000.0)) {
        return std::unexpected(
            AudioError(AudioErrorCode::unsupported_configuration, "Invalid sample rate config"));
    }

    if (config.suggested_latency.has_value() && !(config.suggested_latency.value() > 0.0)) {
        return std::unexpected(AudioError(AudioErrorCode::unsupported_configuration,
                                          "Invalid suggested latency config"));
    }

    if (config.input.has_value()) {
        const auto inputValidationResult = validate_endpoint(config.input.value(), "Input");
        if (!inputValidationResult.has_value()) {
            return inputValidationResult;
        }
    }

    if (config.output.has_value()) {
        const auto outputValidationResult = validate_endpoint(config.output.value(), "Output");
        if (!outputValidationResult.has_value()) {
            return outputValidationResult;
        }
    }

    return {};
}

std::uint32_t CubebStreamHandle::latency_frames(cubeb_stream_params& params) const
{
    std::uint32_t frames = 0;

    if (_config.suggested_latency.has_value()) {
        frames = static_cast<std::uint32_t>(std::min(
            std::lround(_config.suggested_latency.value() * params.rate),
            static_cast<long>(kMaxLatencyFrames)));
    } else if (_config.frames_per_buffer != 0) {
        frames = _config.frames_per_buffer;
    } else if (cubeb_get_min_latency(_runtime->context(), &params, &frames) != CUBEB_OK) {
        // Fall back to the lowest latency the device itself reports.
        frames = std::max(_config.input.has_value() ? _config.input->latency_lo : 0,
                          _config.output.has_value() ? _config.output->latency_lo : 0);
    }

    return std::clamp(frames, kMinLatencyFrames, kMaxLatencyFrames);
}

StreamResult CubebStreamHandle::open()
{
    if (_stream != nullptr) {
        return {};
    }
    if (_runtime->context() == nullptr) {
        return std::unexpected(
            AudioError(AudioErrorCode::initialization_failed, "cubeb context is not initialized"));
    }
    // validate the configs
    const auto validationResult = validate_stream_config(_config);
    if (!validationResult.has_value()) {
        return validationResult;
    }

    const auto rate = static_cast<std::uint32_t>(std::lround(_config.sample_rate));

    std::optional<cubeb_stream_params> inputParams;
    std::optional<cubeb_stream_params> outputParams;
    if (_config.input.has_value()) {
        inputParams = make_params(rate, _config.input->channels);
    }
    if (_config.output.has_value()) {
        outputParams = make_params(rate, _config.output->channels);
    }

    const std::uint32_t latency =
        latency_frames(outputParams.has_value() ? *outputParams : *inputParams);

    cubeb_stream* stream = nullptr;
    const int     error  = cubeb_stream_init(
        _runtime->context(), &stream, "dth_audio_stream",
        _config.input.has_value() ? _config.input->devid : nullptr,
        inputParams.has_value() ? &inputParams.value() : nullptr,
        _config.output.has_value() ? _config.output->devid : nullptr,
        outputParams.has_value() ? &outputParams.value() : nullptr, latency,
        &CubebStreamHandle::data_callback, &CubebStreamHandle::state_callback, _context.get());

    if (error != CUBEB_OK) {
        return std::unexpected(
            AudioError(AudioErrorCode::stream_open_failed, "cubeb_stream_init failed", error));
    }

    _stream.reset(stream);
    _context->state.store(StreamState::open, std::memory_order_release);

    return {};
}

StreamResult CubebStreamHandle::start()
{
    if (_stream == nullptr) {
        return std::unexpected(AudioError(AudioErrorCode::stream_start_failed,
                                          "Cannot start an uninitialized stream. Open() first"));
    }
    if (is_running()) {
        return {};
    }

    // Mark running before starting so a DRAINED/ERROR state callback fired right after
    // cubeb_stream_start is not overwritten.
    const StreamState previous = _context->state.load(std::memory_order_acquire);
    _context->draining.store(false, std::memory_order_relaxed);
    _context->state.store(StreamState::running, std::memory_order_release);

    const int error = cubeb_stream_start(_stream.get());

    if (error != CUBEB_OK) {
        _context->state.store(previous, std::memory_order_release);
        return std::unexpected(
            AudioError(AudioErrorCode::stream_start_failed, "cubeb_stream_start failed", error));
    }

    return {};
}

StreamResult CubebStreamHandle::stop()
{
    if (_stream == nullptr) {
        return {};
    }

    if (is_stopped()) {
        return {};
    }

    const int error = cubeb_stream_stop(_stream.get());

    if (error != CUBEB_OK) {
        return std::unexpected(
            AudioError(AudioErrorCode::stream_stop_failed, "cubeb_stream_stop failed", error));
    }

    _context->state.store(StreamState::stopped, std::memory_order_release);

    return {};
}

StreamResult CubebStreamHandle::close()
{
    // StreamDeleter stops then destroys the stream; cubeb_stream_destroy cannot fail.
    _stream.reset();

    _context->state.store(StreamState::closed, std::memory_order_release);

    return {};
}

bool CubebStreamHandle::is_open() const noexcept
{
    return _context->state.load(std::memory_order_acquire) == StreamState::open;
}
bool CubebStreamHandle::is_running() const noexcept
{
    return _context->state.load(std::memory_order_acquire) == StreamState::running;
}
bool CubebStreamHandle::is_stopped() const noexcept
{
    return _context->state.load(std::memory_order_acquire) == StreamState::stopped;
}

bool CubebStreamHandle::is_closed() const noexcept
{
    return _context->state.load(std::memory_order_acquire) == StreamState::closed;
}

long CubebStreamHandle::data_callback(cubeb_stream* /*stream*/, void* user_ptr,
                                      const void* input_buffer, void* output_buffer,
                                      long nframes) noexcept
{
    auto* context = static_cast<CallbackContext*>(user_ptr);

    const auto silence_output = [&] {
        if (output_buffer != nullptr) {
            std::memset(output_buffer, 0,
                        static_cast<std::size_t>(nframes) * context->output_channels *
                            sizeof(float));
        }
    };

    // Returning fewer than nframes puts the stream into drain mode.
    if (context->draining.load(std::memory_order_relaxed)) {
        silence_output();
        return 0;
    }

    switch (context->callback(input_buffer, output_buffer, nframes, context->user_data)) {
        case CallbackResult::continue_stream:
            return nframes;
        case CallbackResult::complete_stream:
            // Play this buffer in full, drain on the next callback.
            context->draining.store(true, std::memory_order_relaxed);
            return nframes;
        case CallbackResult::stop_stream:
            silence_output();
            return 0;
    }
    return nframes;
}

void CubebStreamHandle::state_callback(cubeb_stream* /*stream*/, void* user_ptr,
                                       cubeb_state state) noexcept
{
    auto* context = static_cast<CallbackContext*>(user_ptr);
    switch (state) {
        case CUBEB_STATE_DRAINED:
            context->state.store(StreamState::stopped, std::memory_order_release);
            break;
        case CUBEB_STATE_ERROR:
            context->state.store(StreamState::stopped_by_error, std::memory_order_release);
            break;
        case CUBEB_STATE_STARTED:
        case CUBEB_STATE_STOPPED:
            // Tracked by start()/stop().
            break;
    }
}
}  // namespace dth::audio
