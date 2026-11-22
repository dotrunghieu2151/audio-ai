// AudioTranslator::Config validation: runs before any model is loaded.
#include <gtest/gtest.h>

#include <stdexcept>

#include "dth/audio/audio_translator.hpp"

using dth::audio::AudioTranslator;

TEST(AudioTranslatorConfig, RejectsInvalidConfigBeforeLoading)
{
    EXPECT_THROW(AudioTranslator({.model_path = ""}), std::invalid_argument);
    EXPECT_THROW(AudioTranslator({.model_path = "x.gguf", .model = "unknown"}), std::invalid_argument);
    EXPECT_THROW(AudioTranslator({.model_path = "x.gguf", .threads = 0}), std::invalid_argument);
    EXPECT_THROW(AudioTranslator({.model_path = "x.gguf", .context_tokens = 100}),
                 std::invalid_argument);
    EXPECT_THROW(AudioTranslator({.model_path = "/nonexistent/model.gguf"}), std::runtime_error);
}
