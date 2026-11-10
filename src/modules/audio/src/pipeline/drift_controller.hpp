#pragma once

#include <cstddef>

#include "dth/audio/audio-types.hpp"

namespace dth::audio {

// Clock drift compensation for a producer / consumer pair bridged by a ring buffer. Maps the ring
// fill level, sampled once per consumer callback, to a ratio multiplier for
// FormatConverter::set_ratio() (> 1 consumes input faster):
//
//   target = 2 * callback_frames + max(margin_seconds * sample_rate, producer_block_frames)
//            (the margin must absorb one producer write burst, see set_producer_block_frames())
//   raw    = 1 + k * (fill - target) / target,  k = target / (sample_rate * correction_seconds)
//            (a fill error is drained in ~correction_seconds, independent of callback size)
//   raw is clamped to 1 +- FormatConverter::max_ratio_deviation, then one-pole smoothed
//   (time constant smoothing_seconds) to filter the fill sawtooth caused by block-wise writes.
//   The returned (applied) ratio follows the smoothed one only when it moved by at least
//   apply_deadband and apply_hold_seconds passed since the last change: every effective change
//   costs a resampler filter rebuild (FormatConverter::set_ratio()), so this bounds their rate.
//
// A pure proportional loop: with a constant clock offset d (relative), the fill settles at
// target + d * sample_rate * correction_seconds.
//
// Pure, deterministic logic, no threads. prepare()/reset() on the control thread; update() on the
// consumer real-time thread (no allocation, locks or IO).
class DriftController final
{
  public:
    struct Config
    {
        double      sample_rate     = 0.0;  // ring frames per second
        std::size_t callback_frames = 0;    // ring frames consumed per consumer callback (nominal)
    };

    static constexpr double margin_seconds     = 0.010;
    static constexpr double correction_seconds = 2.0;
    static constexpr double smoothing_seconds  = 1.0;
    static constexpr double apply_deadband     = 20e-6;
    static constexpr double apply_hold_seconds = 0.1;

    // Resets state; ratio back to 1.
    StreamResult prepare(const Config& config);

    // Ratio back to 1, keeps the configuration (including the producer block).
    void reset() noexcept;

    // Largest producer write seen so far (ring frames). Raises the target when a write burst
    // exceeds the margin. Real-time safe; call from the consumer thread before update().
    void set_producer_block_frames(std::size_t frames) noexcept;

    // Call once per consumer callback with the current ring fill (frames). Returns the ratio to
    // apply (held between changes, see apply_deadband / apply_hold_seconds).
    double update(std::size_t fill_frames) noexcept;

    [[nodiscard]]
    double ratio() const noexcept;

    [[nodiscard]]
    std::size_t target_fill() const noexcept;

  private:
    std::size_t _base{0};    // 2 * callback_frames
    std::size_t _margin{0};  // margin_seconds * sample_rate
    std::size_t _target{0};
    double      _gain{0.0};       // ratio change per frame of fill error
    double      _smoothing{0.0};  // one-pole coefficient per update()
    double      _smoothed{1.0};
    double      _ratio{1.0};          // applied
    std::size_t _hold_updates{1};     // apply_hold_seconds in update() calls
    std::size_t _since_change{0};     // update() calls since the applied ratio changed
};

}  // namespace dth::audio
