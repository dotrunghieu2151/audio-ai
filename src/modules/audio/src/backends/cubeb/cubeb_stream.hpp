#pragma once

#include <cubeb/cubeb.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "backends/cubeb/cubeb_backend_runtime.hpp"
#include "dth/audio/audio-types.hpp"

namespace dth::audio {

class CubebStreamHandle final
{
  public:
    struct Endpoint
    {
        cubeb_devid   devid{nullptr};
        std::uint32_t channels{0};
        std::uint32_t max_channels{0};
        std::uint32_t latency_lo{0};
        // Keeps `devid` valid. nullptr means the requested device was not found.
        std::shared_ptr<const CubebDeviceCollection> owner;
    };

    struct StreamConfig
    {
        std::optional<Endpoint> input{std::nullopt};
        std::optional<Endpoint> output{std::nullopt};
        double                  sample_rate{0.0};
        std::uint32_t           frames_per_buffer{0};
        std::optional<double>   suggested_latency{std::nullopt};
    };

    using StreamCallback = std::function<CallbackResult(const void* input, void* output,
                                                        long frameCount, void* userData)>;

    CubebStreamHandle(std::shared_ptr<CubebBackendRuntime> runtime, StreamConfig config,
                      StreamCallback callback, void* userData);

    ~CubebStreamHandle() = default;

    CubebStreamHandle(const CubebStreamHandle&)            = delete;
    CubebStreamHandle& operator=(const CubebStreamHandle&) = delete;

    CubebStreamHandle(CubebStreamHandle&&) = default;
    // Deleted: member-wise move assignment would release the old runtime before the old stream.
    CubebStreamHandle& operator=(CubebStreamHandle&&) = delete;

    StreamResult open();
    StreamResult start();
    StreamResult stop();
    StreamResult close();

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_stopped() const noexcept;
    [[nodiscard]] bool is_closed() const noexcept;

  private:
    // State touched by the cubeb callback threads. Heap allocated so its address (passed to
    // cubeb as user_ptr) stays stable when the handle is moved.
    struct CallbackContext
    {
        StreamCallback           callback;
        void*                    user_data{nullptr};
        std::uint32_t            output_channels{0};
        std::atomic<StreamState> state{StreamState::closed};
        // Set after the user returns complete_stream: the next callback drains the stream.
        std::atomic<bool> draining{false};
    };

    struct StreamDeleter
    {
        void operator()(cubeb_stream* stream) const noexcept
        {
            // cubeb requires stop before destroy; stopping a stopped stream is a no-op.
            cubeb_stream_stop(stream);
            cubeb_stream_destroy(stream);
        }
    };

    [[nodiscard]] static StreamResult validate_stream_config(const StreamConfig& config);
    [[nodiscard]] static StreamResult validate_endpoint(const Endpoint& endpoint,
                                                        const char*     direction);
    [[nodiscard]] std::uint32_t latency_frames(cubeb_stream_params& params) const;

    static long data_callback(cubeb_stream* stream, void* user_ptr, const void* input_buffer,
                              void* output_buffer, long nframes) noexcept;
    static void state_callback(cubeb_stream* stream, void* user_ptr, cubeb_state state) noexcept;

    // Declaration order matters: _stream is destroyed first, then the callback context,
    // then the device collections (in _config), then the runtime.
    std::shared_ptr<CubebBackendRuntime>         _runtime;
    StreamConfig                                 _config;
    std::unique_ptr<CallbackContext>             _context;
    std::unique_ptr<cubeb_stream, StreamDeleter> _stream;
};
}  // namespace dth::audio
