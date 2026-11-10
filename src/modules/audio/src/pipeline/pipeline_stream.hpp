#pragma once

#include <atomic>
#include <memory>

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_sink.hpp"
#include "dth/audio/audio_source.hpp"
#include "dth/audio/pipeline_config.hpp"
#include "pipeline/converting_sink.hpp"

namespace dth::audio {

// Owns an AudioSource and an AudioSink and runs source -> [ConvertingSink] -> sink. Satisfies the
// AudioStreamImpl concept; AudioStream::Impl holds it as a variant alternative.
//
// Lifecycle mirrors CubebStreamHandle: open() is a no-op when already open, start() needs open(),
// stop() is a no-op unless running, close() always succeeds unless a teardown call fails.
// - open:  source.open -> chain.prepare(source.format())
// - start: chain.start -> source.start(chain)
// - stop:  source.stop -> chain.stop
// - close: stop -> source.close -> chain.close
//
// End of stream: once the sink returns stop_stream / complete_stream, no further write() reaches
// it and the stream reports stopped (is_running() false, is_stopped() true). The source itself is
// stopped by the next control call (stop / start / close / destruction): real-time threads never
// stop devices.
//
// Control thread only (is_* any thread). Movable (stages are heap-stable: RT callbacks hold their
// addresses); move assignment deleted, like CubebStreamHandle.
class PipelineStream final
{
  public:
    PipelineStream(std::unique_ptr<AudioSource> source, std::unique_ptr<AudioSink> sink,
                   PipelineConfig config = {});
    ~PipelineStream();

    PipelineStream(const PipelineStream&)            = delete;
    PipelineStream& operator=(const PipelineStream&) = delete;

    PipelineStream(PipelineStream&& other) noexcept;
    PipelineStream& operator=(PipelineStream&&) noexcept = delete;

    StreamResult open();
    StreamResult start();
    StreamResult stop();
    StreamResult close();

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_stopped() const noexcept;
    [[nodiscard]] bool is_closed() const noexcept;

  private:
    // First stage the source writes into: forwards until the sink ends the stream, then drops.
    class EndDetector final : public AudioSink
    {
      public:
        explicit EndDetector(AudioSink& next) noexcept : _next{next} {}

        StreamResult prepare(const AudioFormat& input) override { return _next.prepare(input); }
        StreamResult start() override;
        StreamResult stop() override { return _next.stop(); }
        StreamResult close() override { return _next.close(); }

        CallbackResult write(AudioBufferView buffer) noexcept override;

        [[nodiscard]] bool ended() const noexcept { return _ended.load(std::memory_order_acquire); }

      private:
        AudioSink&        _next;
        std::atomic<bool> _ended{false};
    };

    struct Chain
    {
        Chain(AudioSink& sink, PipelineConfig config) : converter{sink, config}, head{converter} {}

        ConvertingSink converter;
        EndDetector    head;
    };

    [[nodiscard]] bool ended() const noexcept;

    // Declaration order: chain refers to sink.
    std::unique_ptr<AudioSource> _source;
    std::unique_ptr<AudioSink>   _sink;
    std::unique_ptr<Chain>       _chain;
    StreamState                  _state{StreamState::closed};
};

}  // namespace dth::audio
