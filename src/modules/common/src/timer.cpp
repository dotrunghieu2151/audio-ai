#include "dth/common/timer.hpp"

#include <chrono>

namespace dth::common {

void Timer::start() { start_ = std::chrono::high_resolution_clock::now(); }

double Timer::stop_ms()
{
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start_).count();
}

long long Timer::stop_microsecond()
{
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
}
}  // namespace dth::common
