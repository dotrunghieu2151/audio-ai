#include "dth/audio/audio_translator.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "translation/llama_support.hpp"
#include "translation/sentence_boundary_model.hpp"
#include "translation/translation_engine.hpp"
#include "translation/translation_model_adapter.hpp"
#include "translation/translation_scheduler.hpp"
#include "translation/translation_worker.hpp"

namespace dth::audio {

namespace {

// Bounds of the Config / StreamConfig values (see audio_translator.hpp).
constexpr int    min_context_tokens        = 256;
constexpr int    max_context_tokens        = 131072;
constexpr int    min_unit_chars            = 16;
constexpr int    max_unit_chars            = 4096;
constexpr int    longest_unit_factor       = 2;  // of max_unit_chars (unpunctuated text)
constexpr double max_fragment_hold_sec     = 60.0;
constexpr double max_paragraph_gap_sec     = 3600.0;
constexpr int    max_context_units         = 64;
constexpr int    max_stream_context_tokens = 65536;
constexpr int    max_lookahead_tokens      = 16;
// Prompt markup and the translation's slack around a unit (TranslationEngine's 2x + 32 bound).
constexpr int prompt_overhead_tokens = 96;

void validate(const AudioTranslator::Config& config)
{
    if (config.model_path.empty()) {
        throw std::invalid_argument("model_path cannot be empty");
    }
    if (config.threads < 1) {
        throw std::invalid_argument("threads must be at least 1");
    }
    if (config.context_tokens < min_context_tokens || config.context_tokens > max_context_tokens) {
        throw std::invalid_argument("context_tokens must be within [256, 131072]");
    }
}

TranslationScheduler::Config scheduler_config(const AudioTranslator::StreamConfig& config)
{
    if (config.max_unit_chars < min_unit_chars || config.max_unit_chars > max_unit_chars) {
        throw std::invalid_argument("max_unit_chars must be within [16, 4096]");
    }
    if (!(config.fragment_hold_sec >= 0.0 && config.fragment_hold_sec <= max_fragment_hold_sec)) {
        throw std::invalid_argument("fragment_hold_sec must be within [0, 60]");
    }
    if (!(config.paragraph_gap_sec >= 0.0 && config.paragraph_gap_sec <= max_paragraph_gap_sec)) {
        throw std::invalid_argument("paragraph_gap_sec must be within [0, 3600]");
    }
    if (config.context_units < 0 || config.context_units > max_context_units) {
        throw std::invalid_argument("context_units must be within [0, 64]");
    }
    if (config.context_tokens < 0 || config.context_tokens > max_stream_context_tokens) {
        throw std::invalid_argument("context_tokens must be within [0, 65536]");
    }
    if (!(config.sentence_threshold > 0.0 && config.sentence_threshold < 1.0)) {
        throw std::invalid_argument("sentence_threshold must be within (0, 1)");
    }
    if (config.sentence_lookahead_tokens < 1 ||
        config.sentence_lookahead_tokens > max_lookahead_tokens) {
        throw std::invalid_argument("sentence_lookahead_tokens must be within [1, 16]");
    }
    if (config.japanese_sentence_endings && config.languages.source != "ja") {
        throw std::invalid_argument("japanese_sentence_endings needs languages.source \"ja\"");
    }
    return {.max_unit_chars            = static_cast<std::size_t>(config.max_unit_chars),
            .fragment_hold_sec         = config.fragment_hold_sec,
            .paragraph_gap_sec         = config.paragraph_gap_sec,
            .context_units             = static_cast<std::size_t>(config.context_units),
            .context_tokens            = static_cast<std::size_t>(config.context_tokens),
            .japanese_sentence_endings = config.japanese_sentence_endings};
}

// The scheduler's boundary finder over the shared sentence model (kept alive by the stream).
TranslationScheduler::BoundaryFinder
boundary_finder(std::shared_ptr<const SentenceBoundaryModel> model,
                const AudioTranslator::StreamConfig& config)
{
    if (!model) {
        return {};
    }
    const auto threshold = static_cast<float>(config.sentence_threshold);
    const auto lookahead = static_cast<std::size_t>(config.sentence_lookahead_tokens);
    return [model = std::move(model), threshold, lookahead](std::string_view text, std::size_t from) {
        return model->first_boundary(text, threshold, lookahead, from);
    };
}

bool is_blank(std::string_view text)
{
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

}  // namespace

class AudioTranslator::Impl
{
  public:
    explicit Impl(Config config) : config_{std::move(config)}
    {
        validate(config_);
        auto adapter = make_translation_model_adapter(config_.model);

        auto model = llama::load_model(config_.model_path, config_.gpu_layers);
        adapter->check_architecture(llama::model_metadata(*model, "general.architecture"));

        model_ = std::make_shared<const TranslationModel>(
            TranslationModel{.model = std::move(model), .adapter = std::move(adapter)});
        if (!config_.sentence_model_path.empty()) {
            sentences_ = std::make_shared<const SentenceBoundaryModel>(config_.sentence_model_path);
        }
    }

    TranslateResult translate(std::string_view text, const Languages& languages)
    {
        model_->adapter->validate(languages);
        if (is_blank(text)) {
            return {};
        }
        if (!engine_) {
            engine_ = std::make_unique<TranslationEngine>(model_, engine_settings());
        }
        const TranslationJob job{.text = std::string{text}};
        return {.text = engine_->translate(job, languages, [](std::string_view) {})};
    }

    std::unique_ptr<TranslationWorker> make_worker(const StreamConfig& config,
                                                   StreamCallback callback, void* user_data)
    {
        if (callback == nullptr) {
            throw std::invalid_argument("callback cannot be null");
        }
        model_->adapter->validate(config.languages);
        TranslationScheduler::Config scheduler = scheduler_config(config);
        scheduler.find_boundary                = boundary_finder(sentences_, config);
        // The longest unit (2 * max_unit_chars bytes) must fit with its translation, so no unit
        // fails the stream: at most a token per byte, twice that (+32) for the translation, plus
        // the prompt markup.
        if (3 * longest_unit_factor * config.max_unit_chars + prompt_overhead_tokens > config_.context_tokens) {
            throw std::invalid_argument(
                "max_unit_chars too large for Config::context_tokens: needs 6 * max_unit_chars + " +
                std::to_string(prompt_overhead_tokens) + " <= context_tokens");
        }

        auto engine = std::make_unique<TranslationEngine>(model_, engine_settings());
        engine->warm_up(config.languages);
        return std::make_unique<TranslationWorker>(
            std::move(engine), TranslationWorker::Settings{.languages = config.languages,
                                                           .scheduler = scheduler,
                                                           .callback  = callback,
                                                           .user_data = user_data});
    }

  private:
    [[nodiscard]] TranslationEngine::Settings engine_settings() const
    {
        return {.context_tokens = config_.context_tokens, .threads = config_.threads};
    }

    Config                                  config_;
    std::shared_ptr<const TranslationModel>      model_;
    std::shared_ptr<const SentenceBoundaryModel> sentences_;  // null: punctuation only
    std::unique_ptr<TranslationEngine>           engine_;  // translate()'s, created on first use
};

AudioTranslator::AudioTranslator(Config config) : impl_{std::make_unique<Impl>(std::move(config))}
{
}

AudioTranslator::~AudioTranslator() = default;

AudioTranslator::TranslateResult AudioTranslator::translate(std::string_view text,
                                                            const Languages& languages)
{
    return impl_->translate(text, languages);
}

TextStream AudioTranslator::make_stream(const StreamConfig& config, StreamCallback callback,
                                        void* user_data)
{
    return TextStream{impl_->make_worker(config, callback, user_data)};
}

}  // namespace dth::audio
