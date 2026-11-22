#pragma once

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dth/audio/audio_transcriber.hpp"
#include "transcription/audiocpp_support.hpp"

namespace dth::audio {

// Streaming speaker diarization (audio.cpp nemotron_3_diar, latency profile "low") for a
// transcriber stream: fed the stream's mono 16 kHz audio, reports the speaker turns that ended
// through a SpeakerCallback (AudioTranscriber::SpeakerUpdate contract). audio.cpp's streaming
// diarization reports a turn only once it ends; the turn still in progress arrives with
// finish(). Single-threaded: the transcriber's worker drives it.
class SpeakerDiarizer
{
  public:
    // Right context of the "low" latency profile: 4 encoder frames of 80 ms. The diarizer
    // decides chunk by chunk (the session's preferred chunk, 9 frames); a chunk is decided once
    // its right context has been pushed, so turns are reported through
    // floor((pushed - right context) / chunk) * chunk (measured: 0.8-1.4 s after a turn ends).
    static constexpr std::int64_t right_context_samples = 4 * 1280;

    // `session`: a streaming "diar" session of nemotron_3_diar with the "low" latency profile.
    SpeakerDiarizer(audiocpp::SessionPtr session, AudioTranscriber::SpeakerCallback callback,
                    void* user_data);

    void start();
    void push(std::span<const float> samples);
    // End of the stream: the remaining turns, including the one in progress.
    void finish();

  private:
    void push_chunk(std::span<const float> chunk);
    [[nodiscard]] double complete_until_sec() const;
    // Reports the turns of `result` not reported before.
    void report(const audiocpp_result* result, double complete_until_sec,
                AudioTranscriber::SpeakerUpdate& update);

    audiocpp::SessionPtr              session_;
    AudioTranscriber::SpeakerCallback callback_;
    void*                             user_data_;
    std::size_t                       chunk_samples_ = 0;

    std::vector<float> buffer_;  // audio of the next chunk
    std::int64_t       pushed_samples_ = 0;
    // (start, speaker) of the turns reported: finish() returns every turn again (the last ones
    // possibly with a refined end).
    std::set<std::pair<std::int64_t, std::string>> reported_;
};

}  // namespace dth::audio
