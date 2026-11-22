#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "dth/audio/text_stream.hpp"

namespace dth::audio {

// Text translation with a llama.cpp model: one-shot (translate()) or continuous (make_stream(),
// e.g. fed by an AudioTranscriber stream).
class AudioTranslator
{
  public:
    struct Config
    {
        // GGUF model file.
        std::string model_path;
        // Model family: how prompts are written for the model and how its output ends.
        // Supported: "plamo_translate" (PLaMo 3 Translate). The constructor checks the model's
        // architecture against it.
        std::string model = "plamo_translate";

        // Layers offloaded to the GPU (Metal, CUDA, ... as llama.cpp was built): -1 all, 0 none
        // (CPU only).
        int gpu_layers = -1;
        int threads    = 4;
        // Context size of each llama.cpp context (translate()'s and every stream's): the prompt,
        // the stream's context pairs and the translation must fit. Range [256, 131072].
        int context_tokens = 4096;
        // Sentence boundary model (SaT, Segment any Text) for streams: a directory with model.onnx
        // and sentencepiece.bpe.model. Finds where sentences end in unpunctuated text (ASR output
        // often lacks punctuation), any language. Runs on the CPU (~10-45 ms per call). Empty:
        // streams cut at punctuation only. A relative path resolves against the working directory
        // (the default suits running from the repository root). The constructor throws
        // std::runtime_error if the files are missing or are not a SaT model.
        std::string sentence_model_path = "assets/models/sat-12l-sm";
    };

    // Language codes as the model family names them; for plamo_translate lowercase codes such
    // as en, ja, zh-hans, ko, ja-easy (no region subtags).
    struct Languages
    {
        // "auto": the model detects the source language. Giving it avoids misdetection of short
        // text; it costs no speed.
        std::string source = "auto";
        // Required; never "auto".
        std::string target;
    };

    struct TranslateResult
    {
        std::string text;
    };

    // Per-stream settings of make_stream(). Out-of-range values throw std::invalid_argument.
    //
    // Pushed text is cut into translation units, translated one at a time, each with the
    // translations of the units before it (same paragraph) as context. A unit ends at a sentence
    // end — inside an utterance as soon as the sentence is complete, without waiting for the
    // utterance's end. A sentence end is punctuation (. ! ? and the sentence marks of other
    // scripts: 。！？ । ؟ ...) or a boundary of the sentence model (Config::sentence_model_path),
    // whichever comes first. The model judges a boundary from the text after it, so a sentence
    // ends once sentence_lookahead_tokens of the next one are in. Text that an utterance leaves
    // without a sentence end (a pause or VAD cut) waits fragment_hold_sec for the next utterance.
    struct StreamConfig
    {
        Languages languages;

        // Longer pending text (also a long sentence) is cut at its last clause mark (, ; : 、 ，),
        // else at its last space; text without either (unpunctuated Japanese) runs on to twice
        // the limit for one or for its sentence end, then is cut at that length. Range [16, 4096]
        // bytes, and 6 * max_unit_chars + 96 <= Config::context_tokens so every unit fits.
        int max_unit_chars = 200;
        // How long a sentence fragment waits for its continuation, in seconds. Range [0, 60].
        double fragment_hold_sec = 0.8;
        // A gap between input utterances at least this long (input times) starts a new paragraph:
        // its units are translated without the earlier ones as context. Range [0, 3600].
        double paragraph_gap_sec = 3.0;
        // Earlier units (source and translation) given as context, at most this many and this many
        // tokens. 0 disables context. Ranges [0, 64] and [0, 65536].
        int context_units  = 6;
        int context_tokens = 1024;

        // Sentence model: a boundary counts at this probability or more, once this many tokens
        // follow it. Higher: fewer false cuts, later commits. Ranges (0, 1) and [1, 16]. Unused
        // without Config::sentence_model_path.
        double sentence_threshold        = 0.8;
        int    sentence_lookahead_tokens = 1;
        // Japanese only (languages.source "ja", else std::invalid_argument): also end a sentence
        // at a polite sentence-final form (ます です ました ...) when the utterance ends there or
        // kanji, katakana, Latin or a space follows. No model call, no lookahead: commits about one
        // ASR chunk earlier than the model where it applies.
        bool japanese_sentence_endings = false;
    };

    // Translation of one unit. Same contract as AudioTranscriber::TranscribedSegment:
    // - is_final false: new translated text of the unit as the model writes it (delta);
    //   concatenating a unit's deltas gives its final text.
    // - is_final true: the unit's whole translation, once per unit, after its deltas.
    // [start_sec, end_sec] is the unit's source span on the input's clock (approximate within an
    // utterance).
    struct TranslatedSegment
    {
        std::uint64_t unit_id = 0;  // increasing; a unit's deltas and final share it
        std::string   source_text;
        std::string   text;
        double        start_sec = 0.0;
        double        end_sec   = 0.0;
        bool          is_final  = true;
        // Who said the unit (TextStream::push(Speakers)); empty without speaker turns or while
        // the unit's turn had not ended yet when it was committed.
        std::string   speaker;
    };

    // Receives a stream's translations, in order, on that stream's worker thread (not real-time:
    // may allocate, lock and do IO; must not throw). `segment` is borrowed for the call. The last
    // calls can run inside the stream's stop() / destructor while the control thread waits for
    // them: `user_data` must outlive the stream, and the callback must not call into the stream.
    using StreamCallback = void (*)(void* user_data, const TranslatedSegment& segment);

    // Loads the model. Throws std::invalid_argument for an unknown family or an out-of-range
    // Config, std::runtime_error if the model cannot be loaded or is not of the family.
    explicit AudioTranslator(Config config);

    ~AudioTranslator();
    AudioTranslator(const AudioTranslator&)            = delete;
    AudioTranslator& operator=(const AudioTranslator&) = delete;
    AudioTranslator(AudioTranslator&&)                 = delete;
    AudioTranslator& operator=(AudioTranslator&&)      = delete;

    // Translates `text` (UTF-8) as one unit, without context. Empty or blank text gives empty
    // text. Throws std::invalid_argument for invalid languages, std::runtime_error if the prompt
    // does not fit Config::context_tokens or inference fails.
    TranslateResult translate(std::string_view text, const Languages& languages);

    // Starts a stream translating pushed text per `config` on its own worker thread and llama.cpp
    // context: it may outlive this translator and run alongside translate(). Call make_stream()
    // and translate() from one thread. Throws std::invalid_argument if `config` is invalid or
    // `callback` is null, std::runtime_error if the context cannot be created.
    [[nodiscard]]
    TextStream make_stream(const StreamConfig& config, StreamCallback callback, void* user_data);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dth::audio
