#include "dth/audio/text_stream.hpp"

#include <utility>

#include "translation/translation_worker.hpp"

namespace dth::audio {

TextStream::TextStream() noexcept = default;

TextStream::TextStream(std::unique_ptr<TranslationWorker> worker) noexcept
    : worker_{std::move(worker)}
{
}

TextStream::~TextStream() = default;

TextStream::TextStream(TextStream&&) noexcept            = default;
TextStream& TextStream::operator=(TextStream&&) noexcept = default;

StreamResult TextStream::push(const Text& text)
{
    if (!worker_) {
        return std::unexpected(AudioError{.code    = AudioErrorCode::stream_stop_failed,
                                          .message = "Empty text stream"});
    }
    return worker_->push(text);
}

StreamResult TextStream::push(const Speakers& speakers)
{
    if (!worker_) {
        return std::unexpected(AudioError{.code    = AudioErrorCode::stream_stop_failed,
                                          .message = "Empty text stream"});
    }
    return worker_->push(speakers);
}

StreamResult TextStream::stop()
{
    return worker_ ? worker_->stop() : StreamResult{};
}

bool TextStream::is_running() const noexcept
{
    return worker_ && worker_->is_running();
}

}  // namespace dth::audio
