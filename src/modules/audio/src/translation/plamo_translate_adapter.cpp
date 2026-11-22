#include "translation/plamo_translate_adapter.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace dth::audio {

namespace {

constexpr std::string_view auto_language = "auto";
constexpr std::string_view marker_prefix = "<|plamo:";

// As the model's chat template checks codes: lowercase letters, a 2-3 letter language and an
// optional 4-letter subtag (zh-hans, ja-easy); no region subtags.
bool is_language_code(std::string_view code)
{
    const auto is_lower = [](std::string_view part) {
        return std::ranges::all_of(part, [](char c) { return c >= 'a' && c <= 'z'; });
    };
    const std::size_t dash     = code.find('-');
    const std::string_view language = code.substr(0, dash);
    if (language.size() < 2 || language.size() > 3 || !is_lower(language)) {
        return false;
    }
    if (dash == std::string_view::npos) {
        return true;
    }
    const std::string_view subtag = code.substr(dash + 1);
    return subtag.size() == 4 && is_lower(subtag);
}

std::string input_marker(std::string_view source)
{
    std::string marker = "<|plamo:tag|>input";
    if (source != auto_language) {
        marker += "<|plamo:key|>lang<|plamo:val|>";
        marker += source;
    }
    marker += "<|plamo:msg|>";
    return marker;
}

std::string output_marker(std::string_view target)
{
    std::string marker = "<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>";
    marker += target;
    marker += "<|plamo:msg|>";
    return marker;
}

}  // namespace

void PlamoTranslateAdapter::check_architecture(std::string_view architecture) const
{
    if (architecture != "plamo3") {
        throw std::runtime_error("plamo_translate needs a PLaMo 3 model (architecture plamo3), got '" +
                                 std::string{architecture} + "'");
    }
}

void PlamoTranslateAdapter::validate(const AudioTranslator::Languages& languages) const
{
    if (languages.source != auto_language && !is_language_code(languages.source)) {
        throw std::invalid_argument("Invalid source language '" + languages.source +
                                    "': use a lowercase code such as en, ja, zh-hans, or auto");
    }
    if (!is_language_code(languages.target)) {
        throw std::invalid_argument("Invalid target language '" + languages.target +
                                    "': use a lowercase code such as en, ja, zh-hans");
    }
}

std::vector<PromptSegment> PlamoTranslateAdapter::build_prompt(
    std::string_view text, const AudioTranslator::Languages& languages,
    std::span<const ContextPair> context) const
{
    const std::string input  = input_marker(languages.source);
    const std::string output = output_marker(languages.target);

    std::vector<PromptSegment> prompt;
    prompt.reserve(2 + (context.size() * 4));
    prompt.push_back({.text = "<|plamo:tag|>dataset<|plamo:msg|>translation", .special = true});
    for (const ContextPair& pair : context) {
        prompt.push_back({.text = input, .special = true});
        prompt.push_back({.text = pair.source, .special = false});
        prompt.push_back({.text = output, .special = true});
        prompt.push_back({.text = pair.translation, .special = false});
    }
    prompt.push_back({.text = input, .special = true});
    prompt.push_back({.text = std::string{text}, .special = false});
    prompt.push_back({.text = output, .special = true});
    return prompt;
}

bool PlamoTranslateAdapter::is_stop(std::string_view piece) const
{
    return piece.starts_with(marker_prefix);
}

std::unique_ptr<TranslationModelAdapter> make_translation_model_adapter(std::string_view family)
{
    if (family == "plamo_translate") {
        return std::make_unique<PlamoTranslateAdapter>();
    }
    throw std::invalid_argument("Unknown translation model family '" + std::string{family} +
                                "' (supported: plamo_translate)");
}

}  // namespace dth::audio
