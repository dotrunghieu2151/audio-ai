#include "pipeline/drift_controller.hpp"

#include <algorithm>
#include <cmath>

#include "pipeline/format_converter.hpp"

namespace dth::audio {

StreamResult DriftController::prepare(const Config& config)
{
    *this = DriftController{};

    if (!(config.sample_rate >= 1.0) || config.callback_frames == 0) {
        return std::unexpected(AudioError{AudioErrorCode::unsupported_configuration,
                                          "DriftController: invalid sample rate or callback size",
                                          std::nullopt});
    }

    _base   = 2 * config.callback_frames;
    _margin = static_cast<std::size_t>(std::lround(margin_seconds * config.sample_rate));
    _target = _base + _margin;
    _gain   = 1.0 / (config.sample_rate * correction_seconds);

    double const callback_seconds = static_cast<double>(config.callback_frames) / config.sample_rate;
    _smoothing = 1.0 - std::exp(-callback_seconds / smoothing_seconds);
    _hold_updates = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::lround(apply_hold_seconds / callback_seconds)));
    _since_change = _hold_updates;
    return {};
}

void DriftController::reset() noexcept
{
    _smoothed     = 1.0;
    _ratio        = 1.0;
    _since_change = _hold_updates;
}

void DriftController::set_producer_block_frames(std::size_t frames) noexcept
{
    _target = _base + std::max(_margin, frames);
}

double DriftController::update(std::size_t fill_frames) noexcept
{
    double const error = static_cast<double>(fill_frames) - static_cast<double>(_target);
    double const raw   = std::clamp(1.0 + _gain * error, 1.0 - FormatConverter::max_ratio_deviation,
                                    1.0 + FormatConverter::max_ratio_deviation);
    _smoothed += _smoothing * (raw - _smoothed);

    if (_since_change < _hold_updates) {
        ++_since_change;
    }
    if (_since_change >= _hold_updates && std::abs(_smoothed - _ratio) >= apply_deadband) {
        _ratio        = _smoothed;
        _since_change = 0;
    }
    return _ratio;
}

double DriftController::ratio() const noexcept
{
    return _ratio;
}

std::size_t DriftController::target_fill() const noexcept
{
    return _target;
}

}  // namespace dth::audio
