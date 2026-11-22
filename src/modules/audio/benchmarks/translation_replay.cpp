// Diagnostic: plays a mono 16 kHz float32 WAV through the live pipeline of
// transcribe-streaming (nemotron continuous_stream -> TextStream) and prints the transcript finals,
// speaker turns and every translation unit with its source text.
//   translation_replay <wav> <from> <to> [--speed 2] [--no-diarize] [--no-sat]
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dth/audio/audio_sink.hpp"
#include "dth/audio/audio_source.hpp"
#include "dth/audio/audio_system.hpp"
#include "dth/audio/audio_transcriber.hpp"
#include "dth/audio/audio_translator.hpp"

using namespace dth::audio;

namespace {

std::vector<float> read_float_wav(const std::string& path)
{
    std::ifstream     file{path, std::ios::binary};
    std::vector<char> bytes{std::istreambuf_iterator<char>{file}, {}};
    std::size_t       pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::string id{bytes.data() + pos, 4};
        std::uint32_t     size = 0;
        std::memcpy(&size, bytes.data() + pos + 4, 4);
        if (id == "data") {
            size = std::min<std::uint32_t>(size, bytes.size() - pos - 8);
            std::vector<float> samples(size / 4);
            std::memcpy(samples.data(), bytes.data() + pos + 8, samples.size() * 4);
            return samples;
        }
        pos += 8 + size + (size % 2);
    }
    return {};
}

class FileSource final : public AudioSource
{
  public:
    FileSource(std::vector<float> samples, double speed) : samples_{std::move(samples)}, speed_{speed} {}
    ~FileSource() override { (void)stop(); }
    StreamResult open() override { return {}; }
    StreamResult close() override { return stop(); }
    [[nodiscard]] AudioFormat format() const noexcept override { return {16000, 1}; }
    StreamResult start(AudioSink& sink) override
    {
        thread_ = std::jthread{[this, &sink](const std::stop_token& stop) {
            constexpr std::size_t block = 512;
            const auto            begin = std::chrono::steady_clock::now();
            for (std::size_t pos = 0; pos < samples_.size() && !stop.stop_requested(); pos += block) {
                const std::size_t frames = std::min(block, samples_.size() - pos);
                std::this_thread::sleep_until(
                    begin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(static_cast<double>(pos) / 16000.0 / speed_)));
                if (sink.write({samples_.data() + pos, frames, 1}) != CallbackResult::continue_stream) {
                    break;
                }
            }
            finished_ = true;
        }};
        return {};
    }
    StreamResult stop() override
    {
        thread_ = {};
        return {};
    }
    [[nodiscard]] bool finished() const { return finished_; }

  private:
    std::vector<float> samples_;
    double             speed_;
    std::jthread       thread_;
    std::atomic<bool>  finished_{false};
};

std::mutex  out_mutex;
TextStream* translation = nullptr;

void on_transcript(void*, const AudioTranscriber::TranscribedSegment& segment)
{
    (void)translation->push({.text = segment.text, .start_sec = segment.start_sec,
                             .end_sec = segment.end_sec, .is_final = segment.is_final});
    if (segment.is_final) {
        const std::scoped_lock lock{out_mutex};
        std::printf("ASR  [%7.2f - %7.2f] %s\n", segment.start_sec, segment.end_sec, segment.text.c_str());
    }
}

void on_speakers(void*, const AudioTranscriber::SpeakerUpdate& update)
{
    std::vector<TextStream::SpeakerTurn> turns;
    for (const auto& turn : update.turns) {
        turns.push_back({.speaker = turn.speaker, .start_sec = turn.start_sec, .end_sec = turn.end_sec});
        const std::scoped_lock lock{out_mutex};
        std::printf("TURN [%7.2f - %7.2f] %s\n", turn.start_sec, turn.end_sec, turn.speaker.c_str());
    }
    (void)translation->push(TextStream::Speakers{.turns = turns, .complete_until_sec = update.complete_until_sec});
}

void on_translation(void*, const AudioTranslator::TranslatedSegment& segment)
{
    if (segment.is_final) {
        const std::scoped_lock lock{out_mutex};
        std::printf("UNIT [%7.2f - %7.2f] %s | %s\n     => %s\n", segment.start_sec, segment.end_sec,
                    segment.speaker.c_str(), segment.source_text.c_str(), segment.text.c_str());
        std::fflush(stdout);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: translation_replay <wav> <from> <to> [--speed s] [--no-diarize] [--no-sat]\n");
        return 1;
    }
    double speed = 2.0;
    bool   diarize = true;
    bool   sat     = true;
    for (int i = 4; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--speed" && i + 1 < argc) speed = std::stod(argv[++i]);
        else if (arg == "--no-diarize") diarize = false;
        else if (arg == "--no-sat") sat = false;
    }
    AudioTranscriber asr({.model = "nemotron_asr",
                          .model_path = "assets/models/nemotron-3.5-asr-streaming-0.6b-q8_0.gguf",
                          .backend = AudioTranscriber::Config::Backend::Metal,
                          .threads = 8});
    AudioTranslator::Config tconfig{.model_path = "assets/models/plamo-3-translate-2b-Q4_K_M.gguf"};
    if (!sat) tconfig.sentence_model_path.clear();
    AudioTranslator translator(tconfig);
    AudioSystem     system;
    TextStream      stream_t = translator.make_stream({.languages = {.source = argv[2], .target = argv[3]}},
                                                      &on_translation, nullptr);
    translation = &stream_t;
    auto  source  = std::make_unique<FileSource>(read_float_wav(argv[1]), speed);
    auto* playing = source.get();
    AudioTranscriber::StreamConfig config{
        .strategy = AudioTranscriber::StreamConfig::Strategy::continuous_stream,
        .max_utterance_sec = 10.0,
        .text_latency_sec  = 0.6};
    if (diarize) {
        config.diarization_model_path = "assets/models/nemotron-3-diarization-q8_0.gguf";
        config.speaker_callback       = &on_speakers;
    }
    auto stream = asr.make_stream(system, std::move(source), config, &on_transcript, nullptr);
    if (!stream.open() || !stream.start()) return 1;
    while (!playing->finished()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    (void)stream.stop();
    (void)stream_t.stop();
}
