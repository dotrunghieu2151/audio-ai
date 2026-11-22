#pragma once

#include "translation/translation_model_adapter.hpp"

namespace dth::audio {

// PLaMo 3 Translate (GGUF architecture "plamo3"). Prompts are PFML3 as the model's embedded chat
// template renders them:
//   <|plamo:tag|>dataset<|plamo:msg|>translation
//   (<|plamo:tag|>input[<|plamo:key|>lang<|plamo:val|>SRC]<|plamo:msg|>SOURCE
//    <|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>TGT<|plamo:msg|>TRANSLATION)*  context pairs
//   <|plamo:tag|>input[<|plamo:key|>lang<|plamo:val|>SRC]<|plamo:msg|>TEXT
//   <|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>TGT<|plamo:msg|>
// The source lang key is left out for "auto" (the model detects the language). The model ends
// its output with the next <|plamo:tag|>.
class PlamoTranslateAdapter final : public TranslationModelAdapter
{
  public:
    void check_architecture(std::string_view architecture) const override;
    void validate(const AudioTranslator::Languages& languages) const override;
    [[nodiscard]] std::vector<PromptSegment> build_prompt(
        std::string_view text, const AudioTranslator::Languages& languages,
        std::span<const ContextPair> context) const override;
    [[nodiscard]] bool is_stop(std::string_view piece) const override;
};

}  // namespace dth::audio
