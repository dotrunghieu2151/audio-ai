#include "backends/cubeb/cubeb_backend.hpp"

#include <cubeb/cubeb.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace dth::audio {

namespace {
const char* or_empty(const char* str) { return str != nullptr ? str : ""; }
}  // namespace

CubebBackend::CubebBackend(std::shared_ptr<CubebBackendRuntime> runtime)
    : _runtime{std::move(runtime)}
{
    _runtime->initialize();
}

void CubebBackend::refresh_devices() const
{
    _devices.clear();
    if (_runtime->context() == nullptr) {
        return;
    }
    const std::string host_api = or_empty(cubeb_get_backend_id(_runtime->context()));

    for (const cubeb_device_type type : {CUBEB_DEVICE_TYPE_INPUT, CUBEB_DEVICE_TYPE_OUTPUT}) {
        const auto collection = CubebDeviceCollection::enumerate(_runtime, type);
        if (collection == nullptr) {
            continue;
        }
        for (const cubeb_device_info& info : collection->devices()) {
            if (info.state != CUBEB_DEVICE_STATE_ENABLED) {
                continue;
            }
            const std::string key =
                std::to_string(static_cast<int>(type)) + ':' + or_empty(info.device_id);
            const auto id = _ids.try_emplace(key, static_cast<std::uint32_t>(_ids.size()))
                                .first->second;
            const bool is_input = type == CUBEB_DEVICE_TYPE_INPUT;

            _devices.push_back(DeviceEntry{
                .device =
                    AudioDevice{
                        .id   = id,
                        .name = info.friendly_name != nullptr ? info.friendly_name
                                                              : or_empty(info.device_id),
                        .host_api            = host_api,
                        .max_input_channels  = is_input ? info.max_channels : 0,
                        .max_output_channels = is_input ? 0 : info.max_channels,
                        .default_sample_rate = static_cast<double>(info.default_rate),
                    },
                .type       = type,
                .devid      = info.devid,
                .preferred  = info.preferred != CUBEB_DEVICE_PREF_NONE,
                .latency_lo = info.latency_lo,
                .owner      = collection,
            });
        }
    }
}

std::vector<AudioDevice> CubebBackend::enumerate_devices() const
{
    refresh_devices();
    std::vector<AudioDevice> devices;
    devices.reserve(_devices.size());
    for (const auto& entry : _devices) {
        devices.push_back(entry.device);
    }
    return devices;
}

std::optional<AudioDevice> CubebBackend::default_device(cubeb_device_type type) const
{
    refresh_devices();
    const auto it = std::ranges::find_if(
        _devices, [type](const DeviceEntry& e) { return e.type == type && e.preferred; });
    if (it == _devices.end()) {
        return std::nullopt;
    }
    return it->device;
}

std::optional<AudioDevice> CubebBackend::default_input_device() const
{
    return default_device(CUBEB_DEVICE_TYPE_INPUT);
}

std::optional<AudioDevice> CubebBackend::default_output_device() const
{
    return default_device(CUBEB_DEVICE_TYPE_OUTPUT);
}

CubebStreamHandle::Endpoint CubebBackend::make_endpoint(const AudioDevice& device,
                                                        cubeb_device_type  type,
                                                        std::uint32_t      channels) const
{
    const auto find = [&] {
        return std::ranges::find_if(_devices, [&](const DeviceEntry& e) {
            return e.type == type && e.device.id == device.id;
        });
    };
    auto it = find();
    if (it == _devices.end()) {
        // Device may come from an enumeration newer than our snapshot.
        refresh_devices();
        it = find();
    }
    if (it == _devices.end()) {
        // owner == nullptr: open() reports device_not_found.
        return CubebStreamHandle::Endpoint{.channels = channels};
    }
    return CubebStreamHandle::Endpoint{
        .devid        = it->devid,
        .channels     = channels,
        .max_channels = it->type == CUBEB_DEVICE_TYPE_INPUT ? it->device.max_input_channels
                                                            : it->device.max_output_channels,
        .latency_lo   = it->latency_lo,
        .owner        = it->owner,
    };
}

CubebStreamHandle CubebBackend::make_stream(const AudioDevice&       device,
                                            const InputStreamConfig& config,
                                            InputCallback callback, void* user_data)
{
    auto streamConfig  = make_common_config(config.common);
    streamConfig.input = make_endpoint(device, CUBEB_DEVICE_TYPE_INPUT, config.channels);
    return {_runtime, std::move(streamConfig), make_stream_callback(config, callback), user_data};
}

CubebStreamHandle CubebBackend::make_stream(const AudioDevice&        device,
                                            const OutputStreamConfig& config,
                                            OutputCallback callback, void* user_data)
{
    auto streamConfig   = make_common_config(config.common);
    streamConfig.output = make_endpoint(device, CUBEB_DEVICE_TYPE_OUTPUT, config.channels);
    return {_runtime, std::move(streamConfig), make_stream_callback(config, callback), user_data};
}

CubebStreamHandle CubebBackend::make_stream(const AudioDevice& input, const AudioDevice& output,
                                            const DuplexStreamConfig& config,
                                            DuplexCallback callback, void* user_data)
{
    auto streamConfig   = make_common_config(config.common);
    streamConfig.input  = make_endpoint(input, CUBEB_DEVICE_TYPE_INPUT, config.input_channels);
    streamConfig.output = make_endpoint(output, CUBEB_DEVICE_TYPE_OUTPUT, config.output_channels);
    return {_runtime, std::move(streamConfig), make_stream_callback(config, callback), user_data};
}

}  // namespace dth::audio
