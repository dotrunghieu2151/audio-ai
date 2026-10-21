#include <spdlog/spdlog.h>

#include <chrono>

class Timer
{
  public:
    void start() { start_ = std::chrono::high_resolution_clock::now(); }

    double stop_ms()
    {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start_).count();
    }

    long long stop_us()
    {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
    }

  private:
    std::chrono::high_resolution_clock::time_point start_;
};

int main()
{
    spdlog::info("Hello, World!");
    return 0;
}