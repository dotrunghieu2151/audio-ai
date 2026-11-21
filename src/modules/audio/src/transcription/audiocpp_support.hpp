#pragma once

#include <audiocpp.h>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "dth/audio/audio_transcriber.hpp"

// Shared helpers for the audio.cpp C API: error translation, owning handles, result reading.
namespace dth::audio::audiocpp {

// audio.cpp streaming ASR and Silero VAD take contiguous mono 16 kHz audio.
inline constexpr int sample_rate = 16000;
inline constexpr int channels    = 1;

// Throws std::runtime_error with audio.cpp's detail for the failing call on this thread.
inline void check_status(audiocpp_status status)
{
    if (status == AUDIOCPP_OK) {
        return;
    }

    // Read this immediately after the failing call.
    const char* detail = audiocpp_last_error();

    std::string message = "audio.cpp error: ";

    if (detail && *detail) {
        message += detail;
    }
    else {
        message += audiocpp_status_string(status);
    }

    throw std::runtime_error(message);
}

template <auto Free>
struct HandleDeleter
{
    template <typename T>
    void operator()(T* handle) const noexcept
    {
        Free(handle);
    }
};

using RegistryPtr = std::unique_ptr<audiocpp_registry, HandleDeleter<&audiocpp_registry_free>>;
using ModelPtr   = std::unique_ptr<audiocpp_model, HandleDeleter<&audiocpp_model_free>>;
using SessionPtr = std::unique_ptr<audiocpp_session, HandleDeleter<&audiocpp_session_free>>;
using OptionsPtr = std::unique_ptr<audiocpp_options, HandleDeleter<&audiocpp_options_free>>;
using RequestPtr = std::unique_ptr<audiocpp_request, HandleDeleter<&audiocpp_request_free>>;
using ResultPtr  = std::unique_ptr<audiocpp_result, HandleDeleter<&audiocpp_result_free>>;
using EventPtr   = std::unique_ptr<audiocpp_event, HandleDeleter<&audiocpp_event_free>>;

// Copies the text out of `result`: the strings are borrowed from it.
// Empty when the task produced no text.
inline AudioTranscriber::TranscribeResult read_text(const audiocpp_result* result)
{
    const char* text     = nullptr;
    const char* language = nullptr;

    const audiocpp_status status = audiocpp_result_text(result, &text, &language);
    if (status == AUDIOCPP_ERR_NOT_AVAILABLE) {
        return {};
    }

    check_status(status);

    return text ? AudioTranscriber::TranscribeResult{.text{text},
                                                     .language{language ? language : ""}}
                : AudioTranscriber::TranscribeResult{};
}

using OptionMap = std::map<std::string, std::string>;

// Options for audiocpp_model_load / audiocpp_session_create.
inline OptionsPtr make_options(const OptionMap& values)
{
    OptionsPtr options{audiocpp_options_create()};
    if (!options) {
        throw std::runtime_error("Failed to create audio.cpp options");
    }
    for (const auto& [key, value] : values) {
        check_status(audiocpp_options_set(options.get(), key.c_str(), value.c_str()));
    }
    return options;
}

inline void set_request_options(audiocpp_request* request, const OptionMap& values)
{
    for (const auto& [key, value] : values) {
        check_status(audiocpp_request_set_option(request, key.c_str(), value.c_str()));
    }
}

inline RequestPtr make_request()
{
    RequestPtr request{audiocpp_request_create()};
    if (!request) {
        throw std::runtime_error("Failed to create audio.cpp request");
    }
    return request;
}

// Loads the model at `path` as `family` (e.g. "silero_vad").
inline ModelPtr load_model(audiocpp_registry* registry, const std::string& path,
                           const std::string& family)
{
    const audiocpp_model_config config{family.c_str(), nullptr, nullptr, nullptr};
    audiocpp_model*             model = nullptr;
    check_status(audiocpp_model_load(registry, path.c_str(), &config, nullptr, &model));
    return ModelPtr{model};
}

// `backend_config` and `options` may be NULL. The session keeps its model alive.
inline SessionPtr create_session(const audiocpp_model* model, const char* task, const char* mode,
                                 const audiocpp_backend_config* backend_config,
                                 const audiocpp_options*        options)
{
    audiocpp_session* session = nullptr;
    check_status(audiocpp_session_create(model, task, mode, backend_config, options, &session));
    return SessionPtr{session};
}

// Starts (or restarts) a stream on a "streaming" session for mono 16 kHz input. Families prepare
// from the request's audio format, so the request carries one silent sample; it is never processed.
inline void start_stream(audiocpp_session* session, const OptionMap& request_options = {})
{
    const RequestPtr request = make_request();
    const float silence = 0.0F;
    check_status(audiocpp_request_set_audio(request.get(), &silence, 1, sample_rate, channels));
    set_request_options(request.get(), request_options);
    check_status(audiocpp_stream_start(session, request.get()));
}

}  // namespace dth::audio::audiocpp
