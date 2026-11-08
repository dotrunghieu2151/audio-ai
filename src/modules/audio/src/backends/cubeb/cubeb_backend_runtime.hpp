#pragma once

#include <cubeb/cubeb.h>

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "dth/audio/audio_error.hpp"

namespace dth::audio {

// Owns the cubeb context. Shared by the backend, device collections and streams so the
// context outlives everything created from it (cubeb_destroy must run last).
class CubebBackendRuntime final
{
  public:
    CubebBackendRuntime() = default;

    ~CubebBackendRuntime()
    {
        if (_context != nullptr) {
            cubeb_destroy(_context);
        }
    }

    CubebBackendRuntime(const CubebBackendRuntime&)            = delete;
    CubebBackendRuntime& operator=(const CubebBackendRuntime&) = delete;

    CubebBackendRuntime(CubebBackendRuntime&&)            = delete;
    CubebBackendRuntime& operator=(CubebBackendRuntime&&) = delete;

    std::expected<void, AudioError> initialize()
    {
        if (_context != nullptr) {
            return {};
        }
        const int error = cubeb_init(&_context, "dth_audio", nullptr);

        if (error != CUBEB_OK) {
            _context = nullptr;
            return std::unexpected(AudioError(AudioErrorCode::initialization_failed,
                                              "cubeb initialization failed",
                                              std::make_optional<int>(error)));
        }
        return {};
    }

    [[nodiscard]] cubeb* context() const noexcept { return _context; }

  private:
    cubeb* _context{nullptr};
};

// RAII owner of a cubeb_device_collection. The cubeb_devid handles inside it are only valid
// while the collection is alive, so streams keep a shared_ptr to it until they are destroyed.
class CubebDeviceCollection final
{
  public:
    [[nodiscard]] static std::shared_ptr<const CubebDeviceCollection> enumerate(
        std::shared_ptr<CubebBackendRuntime> runtime, cubeb_device_type type)
    {
        cubeb_device_collection collection{};
        if (runtime->context() == nullptr ||
            cubeb_enumerate_devices(runtime->context(), type, &collection) != CUBEB_OK) {
            return nullptr;
        }
        return std::shared_ptr<const CubebDeviceCollection>(
            new CubebDeviceCollection(std::move(runtime), collection));
    }

    ~CubebDeviceCollection() { cubeb_device_collection_destroy(_runtime->context(), &_collection); }

    CubebDeviceCollection(const CubebDeviceCollection&)            = delete;
    CubebDeviceCollection& operator=(const CubebDeviceCollection&) = delete;

    CubebDeviceCollection(CubebDeviceCollection&&)            = delete;
    CubebDeviceCollection& operator=(CubebDeviceCollection&&) = delete;

    [[nodiscard]] std::span<const cubeb_device_info> devices() const noexcept
    {
        return {_collection.device, _collection.count};
    }

  private:
    CubebDeviceCollection(std::shared_ptr<CubebBackendRuntime> runtime,
                          cubeb_device_collection          collection)
        : _runtime{std::move(runtime)}, _collection{collection}
    {
    }

    std::shared_ptr<CubebBackendRuntime> _runtime;
    cubeb_device_collection              _collection;
};
}  // namespace dth::audio
