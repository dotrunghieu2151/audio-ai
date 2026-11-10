#include "system_audio/system_audio_source.hpp"

#import <CoreAudio/AudioHardware.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>

#include <unistd.h>

#include <algorithm>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "dth/audio/audio_error.hpp"

namespace dth::audio {
namespace {

AudioError make_error(AudioErrorCode code, std::string message, OSStatus status = noErr)
{
    std::optional<int> native;
    if (status != noErr) {
        native = static_cast<int>(status);
    }
    return AudioError(code, "SystemAudioSource: " + std::move(message), native);
}

AudioObjectPropertyAddress global_address(AudioObjectPropertySelector selector)
{
    return {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}

}  // namespace

struct SystemAudioSource::IoProc
{
    // Device IO thread.
    static OSStatus callback(AudioObjectID /*device*/, const AudioTimeStamp* /*now*/,
                             const AudioBufferList* input, const AudioTimeStamp* /*input_time*/,
                             AudioBufferList* /*output*/, const AudioTimeStamp* /*output_time*/,
                             void* client) noexcept
    {
        auto& source = *static_cast<SystemAudioSource*>(client);
        source._in_callback.fetch_add(1);
        if (AudioSink* sink = source._sink.load(); sink != nullptr && input != nullptr) {
            if (deliver(source, *sink, *input) != CallbackResult::continue_stream) {
                // The sink is done: no further write() until start().
                source._sink.store(nullptr);
            }
        }
        source._in_callback.fetch_sub(1);
        return noErr;
    }

    // Buffers whose layout does not match format() are dropped.
    static CallbackResult deliver(SystemAudioSource& source, AudioSink& sink,
                        const AudioBufferList& input) noexcept
    {
        std::uint32_t const channels = source._format.channels;

        if (source._interleaved) {
            if (input.mNumberBuffers < 1) {
                return CallbackResult::continue_stream;
            }
            AudioBuffer const& buffer = input.mBuffers[0];
            if (buffer.mData == nullptr || buffer.mNumberChannels != channels) {
                return CallbackResult::continue_stream;
            }
            return sink.write({static_cast<const float*>(buffer.mData),
                        buffer.mDataByteSize / (sizeof(float) * channels), channels});
        }

        if (input.mNumberBuffers != channels) {
            return CallbackResult::continue_stream;
        }
        std::size_t frames = input.mBuffers[0].mDataByteSize / sizeof(float);
        for (std::uint32_t c = 0; c < channels; ++c) {
            AudioBuffer const& buffer = input.mBuffers[c];
            if (buffer.mData == nullptr || buffer.mNumberChannels != 1) {
                return CallbackResult::continue_stream;
            }
            frames = std::min<std::size_t>(frames, buffer.mDataByteSize / sizeof(float));
        }

        float* const scratch = source._scratch.data();
        for (std::size_t offset = 0; offset < frames;) {
            std::size_t const n = std::min(scratch_frames, frames - offset);
            for (std::uint32_t c = 0; c < channels; ++c) {
                auto const* src = static_cast<const float*>(input.mBuffers[c].mData) + offset;
                for (std::size_t f = 0; f < n; ++f) {
                    scratch[f * channels + c] = src[f];
                }
            }
            if (CallbackResult const result = sink.write({scratch, n, channels});
                result != CallbackResult::continue_stream) {
                return result;
            }
            offset += n;
        }
        return CallbackResult::continue_stream;
    }
};

SystemAudioSource::~SystemAudioSource()
{
    (void)close();
}

StreamResult SystemAudioSource::open()
{
    if (is_open()) {
        return {};
    }

    @autoreleasepool {
        // Own process object, excluded from the tap (no feedback when playing the capture).
        AudioObjectPropertyAddress const translate =
            global_address(kAudioHardwarePropertyTranslatePIDToProcessObject);
        pid_t const   pid  = getpid();
        AudioObjectID own  = kAudioObjectUnknown;
        UInt32        size = sizeof(own);
        OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &translate,
                                                     sizeof(pid), &pid, &size, &own);
        if (status != noErr || own == kAudioObjectUnknown) {
            return std::unexpected(make_error(AudioErrorCode::stream_open_failed,
                                              "own process audio object not found", status));
        }

        CATapDescription* description =
            [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[ @(own) ]];
        description.name         = @"dth system audio";
        description.UUID         = [NSUUID UUID];
        description.privateTap   = YES;
        description.muteBehavior = CATapUnmuted;

        AudioObjectID tap = kAudioObjectUnknown;
        status            = AudioHardwareCreateProcessTap(description, &tap);
        if (status != noErr) {
            return std::unexpected(make_error(AudioErrorCode::stream_open_failed,
                                              "AudioHardwareCreateProcessTap failed", status));
        }
        _tap = tap;

        NSDictionary* aggregate_description = @{
            @kAudioAggregateDeviceNameKey : @"dth system audio",
            @kAudioAggregateDeviceUIDKey : [[NSUUID UUID] UUIDString],
            @kAudioAggregateDeviceIsPrivateKey : @YES,
            @kAudioAggregateDeviceIsStackedKey : @NO,
            // NO: with YES, AudioDeviceStart waits until a tapped process plays (no IO, no
            // silence, while the system is idle).
            @kAudioAggregateDeviceTapAutoStartKey : @NO,
            @kAudioAggregateDeviceTapListKey : @[ @{
                @kAudioSubTapUIDKey : [description.UUID UUIDString],
                @kAudioSubTapDriftCompensationKey : @YES,
            } ],
        };
        AudioObjectID aggregate = kAudioObjectUnknown;
        status = AudioHardwareCreateAggregateDevice(
            (__bridge CFDictionaryRef)aggregate_description, &aggregate);
        if (status != noErr) {
            (void)close();
            return std::unexpected(make_error(AudioErrorCode::stream_open_failed,
                                              "AudioHardwareCreateAggregateDevice failed",
                                              status));
        }
        _aggregate = aggregate;
    }

    AudioObjectPropertyAddress const format_address = global_address(kAudioTapPropertyFormat);
    AudioStreamBasicDescription      asbd{};
    UInt32                           size = sizeof(asbd);
    OSStatus status = AudioObjectGetPropertyData(_tap, &format_address, 0, nullptr, &size, &asbd);
    if (status != noErr) {
        (void)close();
        return std::unexpected(
            make_error(AudioErrorCode::stream_open_failed, "tap format unavailable", status));
    }
    if (asbd.mFormatID != kAudioFormatLinearPCM ||
        (asbd.mFormatFlags & kAudioFormatFlagIsFloat) == 0 || asbd.mBitsPerChannel != 32 ||
        asbd.mChannelsPerFrame == 0 || !(asbd.mSampleRate >= 1.0)) {
        (void)close();
        return std::unexpected(make_error(AudioErrorCode::unsupported_configuration,
                                          "tap format is not float32 PCM"));
    }

    _format      = {asbd.mSampleRate, asbd.mChannelsPerFrame};
    _interleaved = (asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0;
    if (!_interleaved) {
        _scratch.assign(scratch_frames * _format.channels, 0.0f);
    }
    return {};
}

StreamResult SystemAudioSource::close()
{
    StreamResult result = stop();

    if (_aggregate != 0) {
        OSStatus const status = AudioHardwareDestroyAggregateDevice(_aggregate);
        _aggregate            = 0;
        if (status != noErr && result) {
            result = std::unexpected(make_error(AudioErrorCode::stream_close_failed,
                                                "AudioHardwareDestroyAggregateDevice failed",
                                                status));
        }
    }
    if (_tap != 0) {
        OSStatus const status = AudioHardwareDestroyProcessTap(_tap);
        _tap                  = 0;
        if (status != noErr && result) {
            result = std::unexpected(make_error(AudioErrorCode::stream_close_failed,
                                                "AudioHardwareDestroyProcessTap failed", status));
        }
    }

    _format = {};
    _scratch.clear();
    _scratch.shrink_to_fit();
    return result;
}

bool SystemAudioSource::is_open() const noexcept
{
    return _aggregate != 0;
}

bool SystemAudioSource::is_running() const noexcept
{
    return _io_proc != nullptr;
}

AudioFormat SystemAudioSource::format() const noexcept
{
    return _format;
}

StreamResult SystemAudioSource::start(AudioSink& sink)
{
    if (!is_open()) {
        return std::unexpected(make_error(AudioErrorCode::stream_start_failed, "not open"));
    }
    if (is_running()) {
        if (_sink.load() != nullptr) {
            return {};
        }
        // Ended by a write() result: the device still runs idle. Restart it.
        if (auto r = stop(); !r) {
            return r;
        }
    }

    _sink.store(&sink);

    AudioDeviceIOProcID proc   = nullptr;
    OSStatus            status = AudioDeviceCreateIOProcID(_aggregate, &IoProc::callback, this, &proc);
    if (status != noErr) {
        _sink.store(nullptr);
        return std::unexpected(
            make_error(AudioErrorCode::stream_start_failed, "AudioDeviceCreateIOProcID failed",
                       status));
    }
    status = AudioDeviceStart(_aggregate, proc);
    if (status != noErr) {
        (void)AudioDeviceDestroyIOProcID(_aggregate, proc);
        _sink.store(nullptr);
        return std::unexpected(
            make_error(AudioErrorCode::stream_start_failed, "AudioDeviceStart failed", status));
    }
    _io_proc = reinterpret_cast<void*>(proc);
    return {};
}

StreamResult SystemAudioSource::stop()
{
    if (!is_running()) {
        return {};
    }

    auto const proc = reinterpret_cast<AudioDeviceIOProcID>(_io_proc);
    _io_proc        = nullptr;

    OSStatus const stop_status    = AudioDeviceStop(_aggregate, proc);
    OSStatus const destroy_status = AudioDeviceDestroyIOProcID(_aggregate, proc);

    // Whatever the HAL guarantees: no callback sees the sink after this.
    _sink.store(nullptr);
    while (_in_callback.load() != 0) {
        std::this_thread::yield();
    }

    if (stop_status != noErr) {
        return std::unexpected(
            make_error(AudioErrorCode::stream_stop_failed, "AudioDeviceStop failed", stop_status));
    }
    if (destroy_status != noErr) {
        return std::unexpected(make_error(AudioErrorCode::stream_stop_failed,
                                          "AudioDeviceDestroyIOProcID failed", destroy_status));
    }
    return {};
}

}  // namespace dth::audio
