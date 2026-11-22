#pragma once

#include <llama.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Shared helpers for the llama.cpp C API: owning handles, process-wide setup, tokens.
namespace dth::audio::llama {

template <auto Free>
struct HandleDeleter
{
    template <typename T>
    void operator()(T* handle) const noexcept
    {
        Free(handle);
    }
};

using ModelPtr   = std::unique_ptr<llama_model, HandleDeleter<&llama_model_free>>;
using ContextPtr = std::unique_ptr<llama_context, HandleDeleter<&llama_free>>;
using SamplerPtr = std::unique_ptr<llama_sampler, HandleDeleter<&llama_sampler_free>>;

// Initializes the llama.cpp backends once per process and routes its log to stderr, errors only
// (it logs every load step otherwise). Thread-safe.
void initialize_backend();

// Throws std::runtime_error if the model cannot be loaded.
[[nodiscard]] ModelPtr load_model(const std::string& path, int gpu_layers);

// GGUF metadata value; empty if the key is missing.
[[nodiscard]] std::string model_metadata(const llama_model& model, const char* key);

// `parse_special`: control tokens written as text (e.g. <|plamo:tag|>) become those tokens.
// Plain text is tokenized literally. Appends to `tokens`.
void tokenize(const llama_vocab& vocab, std::string_view text, bool parse_special,
              std::vector<llama_token>& tokens);

// The text of `token`, control tokens rendered as their text.
[[nodiscard]] std::string token_piece(const llama_vocab& vocab, llama_token token);

}  // namespace dth::audio::llama
