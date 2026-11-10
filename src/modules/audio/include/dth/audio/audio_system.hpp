#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_device.hpp"
#include "dth/audio/audio_sink.hpp"
#include "dth/audio/audio_source.hpp"
#include "dth/audio/audio_stream.hpp"
#include "dth/audio/pipeline_config.hpp"
#include "dth/audio/stream_config.hpp"
#include "dth_audio/export_dth_audio.hpp"

namespace dth::audio {

class DTH_AUDIO_EXPORT AudioSystem
{
  public:
    AudioSystem();
    ~AudioSystem();

    AudioSystem(const AudioSystem&)            = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    AudioSystem(AudioSystem&&) noexcept;
    AudioSystem& operator=(AudioSystem&&) noexcept;

    [[nodiscard]]
    std::vector<AudioDevice> devices() const;

    [[nodiscard]]
    std::optional<AudioDevice> default_input_device() const;

    [[nodiscard]]
    std::optional<AudioDevice> default_output_device() const;

    [[nodiscard]]
    AudioStream make_stream(const AudioDevice& device, const InputStreamConfig& config,
                            InputCallback callback, void* user_data);

    [[nodiscard]]
    AudioStream make_stream(const AudioDevice& device, const OutputStreamConfig& config,
                            OutputCallback callback, void* user_data);

    [[nodiscard]]
    AudioStream make_stream(const AudioDevice& input, const AudioDevice& output,
                            const DuplexStreamConfig& config, DuplexCallback callback,
                            void* user_data);

    // ---- Pipeline streams: an AudioSource pushing into an AudioSink (or a SinkCallback) ----
    //
    // The stream owns its source and sink. Everything runs on the source's real-time thread;
    // `config` selects processing in between (ProcessingFlags::none = raw source format). The
    // sink / callback ends the stream by returning stop_stream or complete_stream (see
    // AudioSink::write). Never fails here; errors surface from open().

    [[nodiscard]]
    AudioStream make_stream(std::unique_ptr<AudioSource> source, std::unique_ptr<AudioSink> sink,
                            const PipelineConfig& config = {});

    // `callback` runs on the source's real-time thread: no allocation, locks, IO or syscalls.
    [[nodiscard]]
    AudioStream make_stream(std::unique_ptr<AudioSource> source, SinkCallback callback,
                            void* user_data, const PipelineConfig& config = {});

    // Captures everything the OS plays, excluding this process. make_stream with
    // make_system_audio_source(). macOS 14.2+ only: elsewhere open() returns
    // AudioErrorCode::unsupported_configuration. Needs the "System Audio Recording" permission
    // (NSAudioCaptureUsageDescription in the host app's Info.plist).
    [[nodiscard]]
    AudioStream make_system_audio_stream(std::unique_ptr<AudioSink> sink,
                                         const PipelineConfig& config = {});

    [[nodiscard]]
    AudioStream make_system_audio_stream(SinkCallback callback, void* user_data,
                                         const PipelineConfig& config = {});

    // Sink that plays its input on `output` (ring buffer bridging the source and device clocks).
    // Keeps this system's backend alive. Errors surface from the stream's open().
    [[nodiscard]]
    std::unique_ptr<AudioSink> make_output_sink(const AudioDevice&      output,
                                                const OutputSinkConfig& config = {});

    // Source capturing `input` at config.common.sample_rate / config.channels. A sink result other
    // than continue_stream ends the backend input stream like an input callback's would.
    [[nodiscard]]
    std::unique_ptr<AudioSource> make_input_source(const AudioDevice&       input,
                                                   const InputStreamConfig& config = {});

    // System audio (see make_system_audio_stream). Only one should run at a time per process.
    [[nodiscard]]
    std::unique_ptr<AudioSource> make_system_audio_source();

  private:
    class Impl;
    std::unique_ptr<Impl> _impl;

    friend struct AudioSystemImplValidator;
};

}  // namespace dth::audio
