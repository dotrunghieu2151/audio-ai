#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace dth::audio {

// Sentence boundaries of unpunctuated text with a SaT model (Segment any Text, wtpsplit; e.g.
// sat-12l-sm): ONNX Runtime on the CPU with the XLM-R sentencepiece tokenizer. The model gives
// every token the probability that a sentence ends after it. It needs the text after a boundary:
// at the end of the text the probability says nothing, so boundaries are only reported with
// `lookahead` tokens after them.
//
// Thread-safe: all members are const; one instance may serve several streams.
class SentenceBoundaryModel
{
  public:
    // At most this many tokens of a text are scored (the model's 512 positions minus <s> </s>);
    // only the first boundary is needed.
    static constexpr std::size_t max_tokens = 510;

    struct Tokens
    {
        std::vector<std::int64_t> ids;   // XLM-R ids (without <s> </s>)
        std::vector<std::size_t>  ends;  // byte offset just past each token in the text
    };

    struct ScoredToken
    {
        std::size_t end         = 0;  // byte offset just past the token
        float       probability = 0;  // that a sentence ends after the token
    };

    // `directory` holds model.onnx and sentencepiece.bpe.model. Throws std::runtime_error.
    explicit SentenceBoundaryModel(const std::filesystem::path& directory);
    ~SentenceBoundaryModel();

    SentenceBoundaryModel(const SentenceBoundaryModel&)            = delete;
    SentenceBoundaryModel& operator=(const SentenceBoundaryModel&) = delete;
    SentenceBoundaryModel(SentenceBoundaryModel&&)                 = delete;
    SentenceBoundaryModel& operator=(SentenceBoundaryModel&&)      = delete;

    [[nodiscard]] Tokens tokenize(std::string_view text) const;

    // The first max_tokens tokens of `text` with their boundary probability.
    [[nodiscard]] std::vector<ScoredToken> score(std::string_view text) const;

    // The end of the first token past byte `from` with probability >= threshold that has at
    // least `lookahead` tokens after it. text[0, from) is left context: the model judges a
    // boundary better knowing how the sentence began.
    [[nodiscard]] std::optional<std::size_t> first_boundary(std::string_view text, float threshold,
                                                            std::size_t lookahead,
                                                            std::size_t from = 0) const;

  private:
    struct Impl;
    std::unique_ptr<const Impl> impl_;
};

}  // namespace dth::audio
