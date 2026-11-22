#include "translation/llama_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace dth::audio::llama {

void initialize_backend()
{
    static std::once_flag once;
    std::call_once(once, [] {
        llama_log_set(
            [](ggml_log_level level, const char* text, void* /*user_data*/) {
                if (level == GGML_LOG_LEVEL_ERROR) {
                    std::fputs(text, stderr);
                }
            },
            nullptr);
        llama_backend_init();
    });
}

ModelPtr load_model(const std::string& path, int gpu_layers)
{
    initialize_backend();
    llama_model_params params = llama_model_default_params();
    params.n_gpu_layers       = gpu_layers;
    ModelPtr model{llama_model_load_from_file(path.c_str(), params)};
    if (!model) {
        throw std::runtime_error("Failed to load llama.cpp model '" + path + "'");
    }
    return model;
}

std::string model_metadata(const llama_model& model, const char* key)
{
    std::array<char, 256> value{};
    const std::int32_t length = llama_model_meta_val_str(&model, key, value.data(), value.size());
    return length < 0 ? std::string{} : std::string{value.data()};
}

void tokenize(const llama_vocab& vocab, std::string_view text, bool parse_special,
              std::vector<llama_token>& tokens)
{
    if (text.empty()) {
        return;
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max() / 2)) {
        throw std::runtime_error("Text too long to tokenize");
    }
    const auto        length = static_cast<std::int32_t>(text.size());
    const std::size_t offset = tokens.size();
    // Usually at most one token per byte; llama_tokenize reports the count needed otherwise.
    tokens.resize(offset + text.size() + 1);
    std::int32_t count =
        llama_tokenize(&vocab, text.data(), length, tokens.data() + offset,
                       static_cast<std::int32_t>(tokens.size() - offset), false, parse_special);
    if (count < 0) {
        tokens.resize(offset + static_cast<std::size_t>(-count));
        count = llama_tokenize(&vocab, text.data(), length, tokens.data() + offset, -count, false,
                               parse_special);
    }
    if (count < 0) {
        throw std::runtime_error("llama.cpp tokenization failed");
    }
    tokens.resize(offset + static_cast<std::size_t>(count));
}

std::string token_piece(const llama_vocab& vocab, llama_token token)
{
    std::string  piece(32, '\0');
    std::int32_t length = llama_token_to_piece(&vocab, token, piece.data(),
                                               static_cast<std::int32_t>(piece.size()), 0, true);
    if (length < 0) {  // too small: -length bytes needed
        piece.resize(static_cast<std::size_t>(-length));
        length = llama_token_to_piece(&vocab, token, piece.data(),
                                      static_cast<std::int32_t>(piece.size()), 0, true);
    }
    if (length < 0) {
        throw std::runtime_error("llama.cpp token piece failed");
    }
    piece.resize(static_cast<std::size_t>(length));
    return piece;
}

}  // namespace dth::audio::llama
