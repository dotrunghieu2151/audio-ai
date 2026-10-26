#pragma once

#include <chrono>
#include <dth_common/export_dth_common.hpp>

namespace dth::common {
class DTH_COMMON_EXPORT Timer
{
  public:
    void start();

    double stop_ms();

    long long stop_microsecond();

  private:
    std::chrono::high_resolution_clock::time_point start_;
};
}  // namespace dth::common
