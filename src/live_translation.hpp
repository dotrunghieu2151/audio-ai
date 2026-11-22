#pragma once

// Shared by the transcribe* demos: command line, files, and the stdout view that shows the
// transcription and its translation live.

#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dth/audio/audio_transcriber.hpp"
#include "dth/audio/audio_translator.hpp"
#include "dth/audio/text_stream.hpp"
#include "spdlog/spdlog.h"

namespace demo {

struct FileCloser
{
    void operator()(std::FILE* file) const noexcept { std::fclose(file); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

struct Options
{
    std::string transcription_path = "transcription.txt";
    std::string translation_path   = "translation.txt";
    std::string translator_model   = "assets/models/plamo-3-translate-8b-Q4_K_M.gguf";
    // Speaker diarization (nemotron_3_diar GGUF); empty: off.
    std::string                            diarization_model;
    dth::audio::AudioTranslator::Languages languages;  // target required
};

inline constexpr const char* default_diarization_model =
    "assets/models/nemotron-3-diarization-q8_0.gguf";

// [transcription.txt] [translation.txt] --to <code> [--from <code|auto>] [--translator <gguf>]
// [--diarize | --diarizer <gguf>]
inline std::optional<Options> parse_options(int argc, char** argv)
{
    Options options;
    int     positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg       = argv[i];
        const bool             has_value = i + 1 < argc;
        if (arg == "--to" && has_value) {
            options.languages.target = argv[++i];
        }
        else if (arg == "--from" && has_value) {
            options.languages.source = argv[++i];
        }
        else if (arg == "--translator" && has_value) {
            options.translator_model = argv[++i];
        }
        else if (arg == "--diarize") {
            options.diarization_model = default_diarization_model;
        }
        else if (arg == "--diarizer" && has_value) {
            options.diarization_model = argv[++i];
        }
        else if (!arg.starts_with("--") && positional == 0) {
            options.transcription_path = arg;
            ++positional;
        }
        else if (!arg.starts_with("--") && positional == 1) {
            options.translation_path = arg;
            ++positional;
        }
        else {
            return std::nullopt;
        }
    }
    if (options.languages.target.empty()) {
        return std::nullopt;
    }
    return options;
}

inline void print_usage(const char* program)
{
    std::fprintf(stderr,
                 "usage: %s [transcription.txt] [translation.txt] --to <lang> [--from <lang|auto>] "
                 "[--translator <model.gguf>] [--diarize | --diarizer <model.gguf>]\n"
                 "  lang: lowercase code, e.g. en, ja, zh-hans\n",
                 program);
}

inline FilePtr open_append(const std::string& path)
{
    FilePtr file{std::fopen(path.c_str(), "a")};
    if (!file) {
        spdlog::error("Cannot open {}", path);
    }
    return file;
}

// Shows the transcription and the translation live on stdout, one line per utterance / unit,
// and appends finished lines to the two files. Transcripts are also pushed into the text stream
// to translate. Both callbacks run on worker threads (transcriber, translator): one mutex
// serializes the output. stdout has one live line: when the other side writes, the open line is
// ended and later resumes on a new line with its text so far.
class LiveTranslation
{
  public:
    LiveTranslation(std::FILE* transcription_file, std::FILE* translation_file, std::string target)
        : transcription_file_{transcription_file},
          translation_file_{translation_file},
          target_{std::move(target)}
    {
    }

    // Where transcripts are sent for translation; set before the transcriber stream starts.
    void translate_into(dth::audio::TextStream& stream) { stream_ = &stream; }

    static void on_transcript(void*                                                   user_data,
                              const dth::audio::AudioTranscriber::TranscribedSegment& segment)
    {
        auto& self = *static_cast<LiveTranslation*>(user_data);
        if (self.stream_ != nullptr) {
            if (auto pushed = self.stream_->push({.text      = segment.text,
                                                  .start_sec = segment.start_sec,
                                                  .end_sec   = segment.end_sec,
                                                  .is_final  = segment.is_final});
                !pushed) {
                spdlog::error("Translation input rejected: {}", pushed.error().message);
            }
        }

        self.write(Line::transcript, segment.start_sec, segment.end_sec, segment.text,
                   segment.is_final);
        if (segment.is_final) {
            std::fprintf(self.transcription_file_, "[%.2f - %.2f] %s\n", segment.start_sec,
                         segment.end_sec, segment.text.c_str());
            std::fflush(self.transcription_file_);
        }
    }

    // Speaker turns go to the translation, which keeps each unit to one speaker.
    static void on_speakers(void*                                              user_data,
                            const dth::audio::AudioTranscriber::SpeakerUpdate& update)
    {
        auto& self = *static_cast<LiveTranslation*>(user_data);
        if (self.stream_ == nullptr) {
            return;
        }
        std::vector<dth::audio::TextStream::SpeakerTurn> turns;
        turns.reserve(update.turns.size());
        for (const auto& turn : update.turns) {
            turns.push_back(
                {.speaker = turn.speaker, .start_sec = turn.start_sec, .end_sec = turn.end_sec});
        }
        if (auto pushed = self.stream_->push(dth::audio::TextStream::Speakers{
                .turns = turns, .complete_until_sec = update.complete_until_sec});
            !pushed) {
            spdlog::error("Speaker update rejected: {}", pushed.error().message);
        }
    }

    static void on_translation(void*                                                 user_data,
                               const dth::audio::AudioTranslator::TranslatedSegment& segment)
    {
        auto& self = *static_cast<LiveTranslation*>(user_data);
        self.write(Line::translation, segment.start_sec, segment.end_sec, segment.text,
                   segment.is_final, segment.speaker);
        if (segment.is_final) {
            const std::string who = segment.speaker.empty() ? "" : segment.speaker + ": ";
            std::fprintf(self.translation_file_, "[%.2f - %.2f] %s%s\n", segment.start_sec,
                         segment.end_sec, who.c_str(), segment.text.c_str());
            std::fflush(self.translation_file_);
        }
    }

  private:
    enum class Line { none, transcript, translation };

    // State of one side's current line: an utterance (transcript) or a unit (translation).
    struct Side
    {
        std::string text;  // shown so far
        std::string speaker;
        double      start_sec = 0.0;
        bool        started   = false;
    };

    // Deltas (is_final false) are appended live; a final ends the line, or prints the whole line
    // if no delta of it was shown (offline transcription).
    void write(Line kind, double start_sec, double end_sec, const std::string& text, bool is_final,
               const std::string& speaker = {})
    {
        Side& side = kind == Line::transcript ? transcript_ : translation_;
        if (!is_final) {
            if (!side.started) {
                side = {.text = {}, .speaker = speaker, .start_sec = start_sec, .started = true};
            }
            if (open_ != kind) {
                end_line();
                std::fputs(prefix(kind, side.start_sec, std::nullopt, side.speaker).c_str(),
                           stdout);
                std::fputs(side.text.c_str(), stdout);
                open_ = kind;
            }
            side.text += text;
            std::fputs(text.c_str(), stdout);
        }
        else {
            if (open_ == kind) {
                end_line();
            }
            else if (!side.started && !text.empty()) {
                end_line();
                std::printf("%s%s\n", prefix(kind, start_sec, end_sec, speaker).c_str(),
                            text.c_str());
            }
            side = {};
        }
        std::fflush(stdout);
    }

    void end_line()
    {
        if (open_ != Line::none) {
            std::fputc('\n', stdout);
            open_ = Line::none;
        }
    }

    // "[start] " or "[start - end] "; translations indented and tagged with the target language
    // and, with diarization, the speaker.
    [[nodiscard]] std::string prefix(Line kind, double start_sec,
                                     std::optional<double> end_sec = std::nullopt,
                                     const std::string&    speaker = {}) const
    {
        std::array<char, 64> time{};
        if (end_sec) {
            std::snprintf(time.data(), time.size(), "[%.2f - %.2f] ", start_sec, *end_sec);
        }
        else {
            std::snprintf(time.data(), time.size(), "[%.2f] ", start_sec);
        }
        if (kind == Line::transcript) {
            return time.data();
        }
        const std::string who = speaker.empty() ? "" : speaker + ": ";
        return std::string{"    "} + time.data() + "(" + target_ + ") " + who;
    }

    std::FILE*              transcription_file_;
    std::FILE*              translation_file_;
    std::string             target_;
    dth::audio::TextStream* stream_ = nullptr;

    Line open_ = Line::none;
    Side transcript_;
    Side translation_;
};

}  // namespace demo
