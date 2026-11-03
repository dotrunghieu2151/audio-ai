#include <memory>

#include "dth/audio/audio-types.hpp"

namespace dth::audio {

class AudioStream
{
  public:
    AudioStream() noexcept = default;
    ~AudioStream();

    AudioStream(const AudioStream&)            = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    AudioStream(AudioStream&&) noexcept;
    AudioStream& operator=(AudioStream&&) noexcept;

    [[nodiscard]]
    bool is_open() const noexcept;

    [[nodiscard]]
    bool is_running() const noexcept;

    [[nodiscard]]
    StreamState state() const noexcept;

    [[nodiscard]]
    StreamResult open();

    [[nodiscard]]
    StreamResult start();

    [[nodiscard]]
    StreamResult stop();

    [[nodiscard]]
    StreamResult close();

    // [[nodiscard]]
    // StreamInfo info() const;

  private:
    class Impl;
    std::unique_ptr<Impl> _impl;

    explicit AudioStream(std::unique_ptr<Impl> impl);

    friend class AudioSystem;
};
}  // namespace dth::audio
