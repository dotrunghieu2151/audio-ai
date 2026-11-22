#include "translation/translation_engine.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "translation/utf8.hpp"

namespace dth::audio {

namespace {

// Bound of a translation's length: generous for any language pair, and a stop for a model that
// never ends its output.
constexpr std::size_t max_output_tokens(std::size_t input_tokens)
{
    return (2 * input_tokens) + 32;
}

}  // namespace

TranslationEngine::TranslationEngine(std::shared_ptr<const TranslationModel> model,
                                     Settings                                settings)
    : model_{std::move(model)}, vocab_{llama_model_get_vocab(model_->model.get())}
{
    llama_context_params params = llama_context_default_params();
    params.n_ctx                = static_cast<std::uint32_t>(settings.context_tokens);
    params.n_seq_max            = 1;
    params.n_threads            = settings.threads;
    params.n_threads_batch      = settings.threads;
    // Sliding-window attention caches (PLaMo 3) can only drop a sequence's tail, which the prefix
    // reuse needs, when they are full size.
    params.swa_full = true;
    params.no_perf  = true;

    context_.reset(llama_init_from_model(model_->model.get(), params));
    if (!context_) {
        throw std::runtime_error("Failed to create llama.cpp context");
    }
    context_size_ = llama_n_ctx(context_.get());
    batch_size_   = llama_n_batch(context_.get());

    sampler_.reset(llama_sampler_chain_init(llama_sampler_chain_default_params()));
    if (!sampler_) {
        throw std::runtime_error("Failed to create llama.cpp sampler");
    }
    llama_sampler_chain_add(sampler_.get(), llama_sampler_init_greedy());
}

void TranslationEngine::warm_up(const AudioTranslator::Languages& languages)
{
    decode_prompt(tokenize_prompt({}, languages, {}));
}

std::string TranslationEngine::translate(const TranslationJob&             job,
                                         const AudioTranslator::Languages& languages,
                                         const TextSink&                   on_text)
{
    const std::size_t output_limit = max_output_tokens(count_tokens(job.text));

    std::span<const ContextPair> context = job.context;
    std::vector<llama_token>     prompt  = tokenize_prompt(job.text, languages, context);
    while (prompt.size() + output_limit > context_size_) {
        if (context.empty()) {
            throw std::runtime_error("Text too long to translate: " + std::to_string(prompt.size()) +
                                     " prompt tokens + " + std::to_string(output_limit) +
                                     " for the translation exceed context_tokens (" +
                                     std::to_string(context_size_) + ")");
        }
        context = context.subspan(1);
        prompt  = tokenize_prompt(job.text, languages, context);
    }

    decode_prompt(prompt);

    utf8::Assembler assembler;
    std::string     translation;
    const auto      emit = [&](const std::string& text) {
        if (!text.empty()) {
            translation += text;
            on_text(text);
        }
    };
    for (std::size_t count = 0; count < output_limit; ++count) {
        const llama_token token = llama_sampler_sample(sampler_.get(), context_.get(), -1);
        if (llama_vocab_is_eog(vocab_, token)) {
            break;
        }
        const std::string piece = llama::token_piece(*vocab_, token);
        if (model_->adapter->is_stop(piece)) {
            break;
        }
        emit(assembler.append(piece));
        decode(std::span{&token, 1});
    }
    assembler.flush();
    return translation;
}

std::size_t TranslationEngine::count_tokens(std::string_view text)
{
    std::vector<llama_token> tokens;
    llama::tokenize(*vocab_, text, false, tokens);
    return tokens.size();
}

std::vector<llama_token> TranslationEngine::tokenize_prompt(
    std::string_view text, const AudioTranslator::Languages& languages,
    std::span<const ContextPair> context) const
{
    std::vector<llama_token> tokens;
    if (llama_vocab_get_add_bos(vocab_)) {
        tokens.push_back(llama_vocab_bos(vocab_));
    }
    for (const PromptSegment& segment : model_->adapter->build_prompt(text, languages, context)) {
        llama::tokenize(*vocab_, segment.text, segment.special, tokens);
    }
    return tokens;
}

void TranslationEngine::decode_prompt(const std::vector<llama_token>& prompt)
{
    if (prompt.empty()) {
        return;
    }
    auto common = static_cast<std::size_t>(
        std::ranges::mismatch(cached_, prompt).in2 - prompt.begin());
    // The last prompt token is decoded again: its logits give the first output token.
    common = std::min(common, prompt.size() - 1);

    llama_memory_t memory = llama_get_memory(context_.get());
    if (llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(common), -1)) {
        cached_.resize(common);
    }
    else {
        clear_cache();
    }
    decode(std::span{prompt}.subspan(cached_.size()));
}

void TranslationEngine::decode(std::span<const llama_token> tokens)
{
    while (!tokens.empty()) {
        const std::span<const llama_token> batch = tokens.first(std::min(tokens.size(), batch_size_));
        // llama_batch_get_one takes a mutable pointer but does not write through it.
        const std::int32_t status = llama_decode(
            context_.get(), llama_batch_get_one(const_cast<llama_token*>(batch.data()),
                                                static_cast<std::int32_t>(batch.size())));
        if (status != 0) {
            clear_cache();
            throw std::runtime_error("llama.cpp decode failed (status " + std::to_string(status) +
                                     ")");
        }
        cached_.insert(cached_.end(), batch.begin(), batch.end());
        tokens = tokens.subspan(batch.size());
    }
}

void TranslationEngine::clear_cache()
{
    llama_memory_clear(llama_get_memory(context_.get()), true);
    cached_.clear();
}

}  // namespace dth::audio
