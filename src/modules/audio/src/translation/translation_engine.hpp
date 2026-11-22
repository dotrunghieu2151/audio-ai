#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dth/audio/audio_translator.hpp"
#include "translation/llama_support.hpp"
#include "translation/translation_model_adapter.hpp"

namespace dth::audio {

// A loaded model and its family's adapter; shared (read-only) by translate() and every stream,
// so a stream keeps it alive after its AudioTranslator is gone.
struct TranslationModel
{
    llama::ModelPtr                                model;
    std::unique_ptr<const TranslationModelAdapter> adapter;
};

// Translates one unit at a time. The stream worker depends on this seam, so it is tested
// without a model.
class UnitTranslator
{
  public:
    // Receives translated text as it is generated: complete UTF-8, in order.
    using TextSink = std::function<void(std::string_view text)>;

    virtual ~UnitTranslator() = default;

    // Translates `job` (its context pairs first); returns the whole translation, after passing it
    // to `on_text` in pieces.
    virtual std::string translate(const TranslationJob&             job,
                                  const AudioTranslator::Languages& languages,
                                  const TextSink&                   on_text) = 0;

    // Tokens `text` takes in a prompt (context budgets).
    [[nodiscard]] virtual std::size_t count_tokens(std::string_view text) = 0;
};

// One llama.cpp context translating with greedy decoding. Consecutive prompts share their start
// (instructions, context pairs), so the KV cache of the longest common token prefix with the
// previous prompt is kept and only the rest is decoded. Not thread-safe: one per thread.
class TranslationEngine final : public UnitTranslator
{
  public:
    struct Settings
    {
        int context_tokens = 4096;
        int threads        = 4;
    };

    // Throws std::runtime_error if the context cannot be created.
    TranslationEngine(std::shared_ptr<const TranslationModel> model, Settings settings);

    // Decodes the prompt start shared by every translation into `languages`, so the first
    // translation does not pay for it (and GPU pipelines are compiled before).
    void warm_up(const AudioTranslator::Languages& languages);

    // Drops oldest context pairs while the prompt plus the translation (at most 2 tokens per input
    // token + 32) does not fit the context; throws std::runtime_error if the text alone does not.
    std::string translate(const TranslationJob& job, const AudioTranslator::Languages& languages,
                          const TextSink& on_text) override;

    [[nodiscard]] std::size_t count_tokens(std::string_view text) override;

  private:
    [[nodiscard]] std::vector<llama_token> tokenize_prompt(
        std::string_view text, const AudioTranslator::Languages& languages,
        std::span<const ContextPair> context) const;
    // Makes the KV cache hold `prompt`, reusing the common prefix of what it holds.
    void decode_prompt(const std::vector<llama_token>& prompt);
    // Decodes `tokens` after the cached ones; on failure the cache is cleared and this throws.
    void decode(std::span<const llama_token> tokens);
    void clear_cache();

    // Declared first: freed after the context that uses it.
    std::shared_ptr<const TranslationModel> model_;
    const llama_vocab*                      vocab_ = nullptr;
    llama::ContextPtr                       context_;
    llama::SamplerPtr                       sampler_;
    std::size_t                             context_size_ = 0;
    std::size_t                             batch_size_   = 0;
    std::vector<llama_token>                cached_;  // tokens in sequence 0's KV cache, in order
};

}  // namespace dth::audio
