// End to end: speech audio -> AudioTranscriber stream -> TextStream -> translations. Runs the ASR,
// VAD and translation models from assets/models (label "model"; skipped if one is missing).
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
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

const std::string assets = DTH_TEST_ASSETS_DIR;
const std::string asr_model = assets + "/models/parakeet-tdt-0.6b-v3-q8_0.gguf";
const std::string vad_model = assets + "/models/silero_vad";
const std::string mt_model  = assets + "/models/plamo-3-translate-2b-Q4_K_M.gguf";
const std::string sentence_model = assets + "/models/sat-12l-sm";
const std::string clips     = DTH_TEST_LIBRISPEECH_DIR;
const std::string diarization_model = assets + "/models/nemotron-3-diarization-q8_0.gguf";
const std::string streaming_asr_model = assets + "/models/nemotron-3.5-asr-streaming-0.6b-q8_0.gguf";
// audio.cpp's four-speaker recording (24 kHz) and its transcript, next to the LibriSpeech clips.
const std::string four_speakers = clips + "/../../resources/four_speaker_short";

// 16-bit PCM WAV, mono 16 kHz, as float.
std::vector<float> read_wav(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    std::vector<char> bytes{std::istreambuf_iterator<char>{file}, {}};
    std::size_t       pos = 12;  // after "RIFF....WAVE"
    while (pos + 8 <= bytes.size()) {
        const std::string id{bytes.data() + pos, 4};
        std::uint32_t     size = 0;
        std::memcpy(&size, bytes.data() + pos + 4, 4);
        if (id == "data") {
            std::vector<float> samples(size / 2);
            for (std::size_t i = 0; i < samples.size(); ++i) {
                std::int16_t value = 0;
                std::memcpy(&value, bytes.data() + pos + 8 + (2 * i), 2);
                samples[i] = static_cast<float>(value) / 32768.0F;
            }
            return samples;
        }
        pos += 8 + size + (size % 2);
    }
    return {};
}

// Plays samples (mono 16 kHz) into the sink from its own thread, faster than real time.
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
            constexpr std::size_t block = 1024;
            const auto            begin = std::chrono::steady_clock::now();
            for (std::size_t pos = 0; pos < samples_.size() && !stop.stop_requested(); pos += block) {
                const std::size_t frames = std::min(block, samples_.size() - pos);
                std::this_thread::sleep_until(
                    begin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(static_cast<double>(pos) / 16000.0 / speed_)));
                if (sink.write({samples_.data() + pos, frames, 1}) !=
                    CallbackResult::continue_stream) {
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

struct Collected
{
    std::mutex               mutex;
    TextStream*              translation = nullptr;
    std::vector<std::string> transcripts;
    std::vector<AudioTranslator::TranslatedSegment> finals;

    static void on_transcript(void* user_data, const AudioTranscriber::TranscribedSegment& segment)
    {
        auto& self = *static_cast<Collected*>(user_data);
        EXPECT_TRUE(self.translation->push({.text      = segment.text,
                                            .start_sec = segment.start_sec,
                                            .end_sec   = segment.end_sec,
                                            .is_final  = segment.is_final}));
        if (segment.is_final) {
            const std::scoped_lock lock{self.mutex};
            self.transcripts.push_back(segment.text);
        }
    }

    std::vector<AudioTranscriber::SpeakerTurn> turns;

    static void on_speakers(void* user_data, const AudioTranscriber::SpeakerUpdate& update)
    {
        auto& self = *static_cast<Collected*>(user_data);
        std::vector<TextStream::SpeakerTurn> turns;
        for (const auto& turn : update.turns) {
            turns.push_back({.speaker = turn.speaker, .start_sec = turn.start_sec, .end_sec = turn.end_sec});
        }
        EXPECT_TRUE(self.translation->push(
            TextStream::Speakers{.turns = turns, .complete_until_sec = update.complete_until_sec}));
        const std::scoped_lock lock{self.mutex};
        self.turns.insert(self.turns.end(), update.turns.begin(), update.turns.end());
    }

    static void on_translation(void* user_data, const AudioTranslator::TranslatedSegment& segment)
    {
        auto& self = *static_cast<Collected*>(user_data);
        if (segment.is_final) {
            const std::scoped_lock lock{self.mutex};
            self.finals.push_back(segment);
        }
    }
};

// 24 kHz -> 16 kHz, linear interpolation (enough for diarization and ASR tests).
std::vector<float> to_16k_from_24k(const std::vector<float>& in)
{
    std::vector<float> out(in.size() * 2 / 3);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double      pos  = static_cast<double>(i) * 1.5;
        const std::size_t left = static_cast<std::size_t>(pos);
        const double      frac = pos - static_cast<double>(left);
        const float       next = left + 1 < in.size() ? in[left + 1] : in[left];
        out[i] = static_cast<float>(in[left] * (1.0 - frac) + next * frac);
    }
    return out;
}

// Lowercase letters and digits of each word.
std::vector<std::string> words_of(const std::string& text)
{
    std::vector<std::string> words;
    std::string              word;
    for (const char c : text + " ") {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        else if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    }
    return words;
}

std::string without_spaces(const std::string& text)
{
    std::string out;
    std::ranges::copy_if(text, std::back_inserter(out), [](char c) { return c != ' '; });
    return out;
}

}  // namespace

TEST(TranslationEndToEnd, EveryTranscribedSentenceIsTranslatedOnceInOrder)
{
    for (const auto& path : {asr_model, vad_model, mt_model, sentence_model, clips}) {
        if (!std::filesystem::exists(path)) {
            GTEST_SKIP() << "missing " << path;
        }
    }
    // Two clips with a pause between: several utterances and sentences.
    std::vector<float> audio = read_wav(clips + "/librispeech_test_clean_6930-75918-0001.wav");
    audio.resize(audio.size() + 24000);
    const auto second = read_wav(clips + "/librispeech_test_clean_6930-75918-0000.wav");
    audio.insert(audio.end(), second.begin(), second.end());
    audio.resize(audio.size() + 16000);
    ASSERT_GT(audio.size(), 16000U * 5);

    AudioTranscriber asr({.model_path = asr_model,
                          .model      = "parakeet_tdt",
                          .backend    = AudioTranscriber::Config::Backend::Metal});
    AudioTranslator  translator({.model_path = mt_model, .sentence_model_path = sentence_model});
    AudioSystem      system;

    Collected  collected;
    TextStream translation = translator.make_stream(
        {.languages = {.source = "en", .target = "ja"}}, &Collected::on_translation, &collected);
    collected.translation = &translation;

    auto  source  = std::make_unique<FileSource>(std::move(audio), 4.0);
    auto* playing = source.get();
    auto  stream  = asr.make_stream(system, std::move(source), {.vad_model_path = vad_model},
                                    &Collected::on_transcript, &collected);
    ASSERT_TRUE(stream.open());
    ASSERT_TRUE(stream.start());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!playing->finished() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(stream.stop());
    ASSERT_TRUE(translation.stop());

    ASSERT_FALSE(collected.transcripts.empty());
    ASSERT_FALSE(collected.finals.empty());
    std::string transcribed;
    for (const auto& text : collected.transcripts) {
        transcribed += text;
    }
    std::string translated_sources;
    for (std::size_t i = 0; i < collected.finals.size(); ++i) {
        const auto& unit = collected.finals[i];
        translated_sources += unit.source_text;
        EXPECT_FALSE(unit.text.empty()) << unit.source_text;
        EXPECT_TRUE(std::ranges::any_of(unit.text, [](char c) { return static_cast<unsigned char>(c) >= 0x80; }))
            << "Japanese expected: " << unit.text;
        if (i > 0) {
            EXPECT_GT(unit.unit_id, collected.finals[i - 1].unit_id);
        }
    }
    // All transcribed text, and nothing else, went through translation units, in order.
    EXPECT_EQ(without_spaces(translated_sources), without_spaces(transcribed));
}

// Four speakers, one after another: every translated unit holds one speaker's words, and the
// diarization tells the speakers apart.
TEST(TranslationEndToEnd, UnitsNeverMixSpeakers)
{
    for (const auto& path : {streaming_asr_model, vad_model, mt_model, sentence_model,
                             diarization_model, four_speakers + ".wav"}) {
        if (!std::filesystem::exists(path)) {
            GTEST_SKIP() << "missing " << path;
        }
    }
    std::vector<float> audio = to_16k_from_24k(read_wav(four_speakers + ".wav"));
    audio.resize(audio.size() + 32000);
    std::vector<std::vector<std::string>> lines;  // words of each speaker's line
    std::ifstream                         transcript{four_speakers + ".txt"};
    for (std::string line; std::getline(transcript, line);) {
        if (const auto colon = line.find(':'); colon != std::string::npos) {
            lines.push_back(words_of(line.substr(colon + 1)));
        }
    }
    ASSERT_EQ(lines.size(), 4U);

    // Diarization matches turns to the partials' times: a streaming, cache-aware ASR.
    AudioTranscriber asr({.model_path = streaming_asr_model,
                          .model      = "nemotron_asr",
                          .backend    = AudioTranscriber::Config::Backend::Metal});
    AudioTranslator  translator({.model_path = mt_model, .sentence_model_path = sentence_model});
    AudioSystem      system;

    Collected  collected;
    TextStream translation = translator.make_stream(
        {.languages = {.source = "en", .target = "ja"}}, &Collected::on_translation, &collected);
    collected.translation = &translation;

    auto  source  = std::make_unique<FileSource>(std::move(audio), 2.0);
    auto* playing = source.get();
    auto  stream  = asr.make_stream(system, std::move(source),
                                    {.strategy = AudioTranscriber::StreamConfig::Strategy::continuous_stream,
                                     .vad_model_path         = vad_model,
                                     .max_utterance_sec      = 10.0,
                                     .text_latency_sec       = 0.6,
                                     .diarization_model_path = diarization_model,
                                     .speaker_callback       = &Collected::on_speakers},
                                    &Collected::on_transcript, &collected);
    ASSERT_TRUE(stream.open());
    ASSERT_TRUE(stream.start());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (!playing->finished() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(stream.stop());
    ASSERT_TRUE(translation.stop());

    std::set<std::string> speakers;
    for (const auto& turn : collected.turns) {
        speakers.insert(turn.speaker);
    }
    EXPECT_GE(speakers.size(), 3U) << "diarization merged speakers";

    ASSERT_FALSE(collected.finals.empty());
    std::set<std::string> labels;
    for (const auto& unit : collected.finals) {
        const auto words = words_of(unit.source_text);
        // Some line holds all of the unit's words but at most one (ASR errors).
        const bool one_speaker = std::ranges::any_of(lines, [&](const auto& line) {
            const auto missing = std::ranges::count_if(words, [&](const std::string& word) {
                return std::ranges::find(line, word) == line.end();
            });
            return missing <= 1;
        });
        EXPECT_TRUE(one_speaker) << "mixes speakers: " << unit.source_text;
        if (!unit.speaker.empty()) {
            labels.insert(unit.speaker);
        }
    }
    EXPECT_GE(labels.size(), 3U) << "units without their speaker";
}
