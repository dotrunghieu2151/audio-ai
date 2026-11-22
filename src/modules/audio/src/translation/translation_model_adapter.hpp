#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dth/audio/audio_translator.hpp"

namespace dth::audio {

// A translated unit kept as context for the next ones.
struct ContextPair
{
    std::string source;
    std::string translation;
};

// One unit to translate, with the earlier units of its paragraph as context (oldest first).
struct TranslationJob
{
    std::uint64_t            unit_id = 0;
    std::string              text;
    double                   start_sec = 0.0;
    double                   end_sec   = 0.0;
    std::vector<ContextPair> context;
    std::string              speaker;  // who said it (diarization); empty when unknown
    // When the text was heard (on the clock of the input times), to name its speaker later.
    double heard_from_sec = 0.0;
    double heard_to_sec   = 0.0;
};

// Part of a prompt. `special` text holds the family's control markup and is tokenized with
// special tokens parsed; other text (user input) is tokenized as plain text, so it can never
// produce control tokens.
struct PromptSegment
{
    std::string text;
    bool        special = false;
};

// What a model family needs to be used for translation. Everything else (llama.cpp contexts, KV
// cache reuse, generation) is family-independent and lives in TranslationEngine.
class TranslationModelAdapter
{
  public:
    virtual ~TranslationModelAdapter() = default;

    // Throws std::runtime_error if a model of GGUF `general.architecture` cannot be used.
    virtual void check_architecture(std::string_view architecture) const = 0;

    // Throws std::invalid_argument for language codes the family does not accept.
    virtual void validate(const AudioTranslator::Languages& languages) const = 0;

    // The prompt that makes the model write the translation of `text`, after the BOS token.
    // Stable parts come first (instructions, then context pairs, then the input), so consecutive
    // prompts of a stream share a prefix whose KV cache is reused.
    [[nodiscard]] virtual std::vector<PromptSegment> build_prompt(
        std::string_view text, const AudioTranslator::Languages& languages,
        std::span<const ContextPair> context) const = 0;

    // Whether generating the token rendered as `piece` (control tokens included) ends the
    // translation. End-of-generation tokens always end it.
    [[nodiscard]] virtual bool is_stop(std::string_view piece) const = 0;
};

// The adapter of `family` (AudioTranslator::Config::model). Throws std::invalid_argument for an
// unknown family.
[[nodiscard]] std::unique_ptr<TranslationModelAdapter> make_translation_model_adapter(
    std::string_view family);

}  // namespace dth::audio
