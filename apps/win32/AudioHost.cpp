#include "AudioHost.h"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstring>

namespace nodsynth::app {
namespace {
const CLSID kEnumerator = __uuidof(MMDeviceEnumerator);
const IID kEnumeratorId = __uuidof(IMMDeviceEnumerator);
const IID kAudioClientId = __uuidof(IAudioClient);
const IID kRenderClientId = __uuidof(IAudioRenderClient);
const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

void copyError(std::string& error, const char* text) { error = text; }

std::wstring deviceName(IMMDevice* device) {
    std::wstring name = L"Output";
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return name;
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    store->Release();
    return name;
}
} // namespace

std::vector<AudioDevice> renderDevices() {
    std::vector<AudioDevice> devices;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(kEnumerator, nullptr, CLSCTX_ALL, kEnumeratorId, reinterpret_cast<void**>(&enumerator)))) return devices;
    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT index = 0; index < count; ++index) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(index, &device))) continue;
            LPWSTR id = nullptr;
            AudioDevice entry;
            if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
                entry.id = id;
                CoTaskMemFree(id);
            }
            entry.name = deviceName(device);
            devices.push_back(std::move(entry));
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();
    return devices;
}

std::vector<MidiDevice> midiDevices() {
    std::vector<MidiDevice> devices;
    const auto count = midiInGetNumDevs();
    for (unsigned index = 0; index < count; ++index) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        devices.push_back(MidiDevice{index, caps.szPname});
    }
    return devices;
}

WasapiOutput::WasapiOutput() {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] { threadMain(); });
}

WasapiOutput::~WasapiOutput() {
    command_.store(3, std::memory_order_release);
    if (wake_ != nullptr) SetEvent(static_cast<HANDLE>(wake_));
    if (event_ != nullptr) SetEvent(static_cast<HANDLE>(event_));
    if (thread_.joinable()) thread_.join();
    if (wake_ != nullptr) CloseHandle(static_cast<HANDLE>(wake_));
}

bool WasapiOutput::start(const std::wstring& deviceId, RenderCallback callback, void* context, std::string& error) {
    command_.store(6, std::memory_order_release);
    if (wake_ != nullptr) SetEvent(static_cast<HANDLE>(wake_));
    if (event_ != nullptr) SetEvent(static_cast<HANDLE>(event_));
    for (int spin = 0; spin < 1000 && command_.load(std::memory_order_acquire) == 6; ++spin) Sleep(2);
    callback_ = callback;
    context_ = context;
    requestedId_ = deviceId;
    command_.store(1, std::memory_order_release);
    SetEvent(static_cast<HANDLE>(wake_));
    for (int spin = 0; spin < 2000; ++spin) {
        const auto state = command_.load(std::memory_order_acquire);
        if (state == 5) return true;
        if (state == 4) {
            error = error_;
            return false;
        }
        Sleep(2);
    }
    copyError(error, "the audio device did not respond");
    return false;
}

void WasapiOutput::begin() {
    if (command_.load(std::memory_order_acquire) != 5) return;
    command_.store(2, std::memory_order_release);
    if (wake_ != nullptr) SetEvent(static_cast<HANDLE>(wake_));
}

void WasapiOutput::stop() {
    const auto state = command_.load(std::memory_order_acquire);
    if (state != 2 && state != 5) return;
    command_.store(6, std::memory_order_release);
    if (wake_ != nullptr) SetEvent(static_cast<HANDLE>(wake_));
    if (event_ != nullptr) SetEvent(static_cast<HANDLE>(event_));
    for (int spin = 0; spin < 1000 && command_.load(std::memory_order_acquire) == 6; ++spin) Sleep(2);
}

void WasapiOutput::threadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (command_.load(std::memory_order_acquire) != 3) {
        WaitForSingleObject(static_cast<HANDLE>(wake_), 200);
        const auto command = command_.load(std::memory_order_acquire);
        if (command == 3) break;
        if (command == 1) {
            std::string error;
            close();
            if (!open(requestedId_, error)) {
                error_ = std::move(error);
                command_.store(4, std::memory_order_release);
            } else {
                command_.store(5, std::memory_order_release);
            }
            continue;
        }
        if (command == 2 && client_ != nullptr) {
            auto* client = static_cast<IAudioClient*>(client_);
            DWORD taskIndex = 0;
            HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
            client->Start();
            while (command_.load(std::memory_order_acquire) == 2) {
                WaitForSingleObject(static_cast<HANDLE>(event_), 50);
                if (command_.load(std::memory_order_acquire) != 2) break;
                renderAvailable();
            }
            client->Stop();
            if (task != nullptr) AvRevertMmThreadCharacteristics(task);
            close();
            if (command_.load(std::memory_order_acquire) == 2) command_.store(0, std::memory_order_release);
            continue;
        }
        if (command == 6) {
            if (client_ != nullptr) static_cast<IAudioClient*>(client_)->Stop();
            close();
            command_.store(0, std::memory_order_release);
        }
    }
    close();
    CoUninitialize();
}

bool WasapiOutput::open(const std::wstring& deviceId, std::string& error) {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(kEnumerator, nullptr, CLSCTX_ALL, kEnumeratorId, reinterpret_cast<void**>(&enumerator)))) {
        copyError(error, "audio devices are unavailable");
        return false;
    }
    IMMDevice* device = nullptr;
    const HRESULT found = deviceId.empty()
        ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)
        : enumerator->GetDevice(deviceId.c_str(), &device);
    enumerator->Release();
    if (FAILED(found) || device == nullptr) {
        copyError(error, "the selected audio device is unavailable");
        return false;
    }
    IAudioClient* client = nullptr;
    HRESULT result = device->Activate(kAudioClientId, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    device->Release();
    if (FAILED(result) || client == nullptr) {
        copyError(error, "the audio device could not be opened");
        return false;
    }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr) {
        client->Release();
        copyError(error, "the audio device has no mix format");
        return false;
    }
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = 2;
    format.Format.nSamplesPerSec = mix->nSamplesPerSec;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = 8;
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = kFloat;
    CoTaskMemFree(mix);
    const REFERENCE_TIME requested = static_cast<REFERENCE_TIME>(10000000.0 * 128.0 / format.Format.nSamplesPerSec);
    const DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, requested, requested, &format.Format, nullptr);
    if (FAILED(result)) result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 0, 0, &format.Format, nullptr);
    if (FAILED(result)) {
        client->Release();
        copyError(error, "shared-mode float output was rejected");
        return false;
    }
    UINT32 frames = 0;
    client->GetBufferSize(&frames);
    if (frames == 0 || frames > 8192) {
        client->Release();
        copyError(error, "the audio buffer is outside the supported size");
        return false;
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(client->SetEventHandle(static_cast<HANDLE>(event_)))) {
        CloseHandle(static_cast<HANDLE>(event_));
        event_ = nullptr;
        client->Release();
        copyError(error, "the audio callback event could not be set");
        return false;
    }
    IAudioRenderClient* render = nullptr;
    if (FAILED(client->GetService(kRenderClientId, reinterpret_cast<void**>(&render)))) {
        CloseHandle(static_cast<HANDLE>(event_));
        event_ = nullptr;
        client->Release();
        copyError(error, "the audio render client is unavailable");
        return false;
    }
    client_ = client;
    render_ = render;
    left_.assign(frames, 0.f);
    right_.assign(frames, 0.f);
    sampleRate_.store(format.Format.nSamplesPerSec, std::memory_order_release);
    bufferFrames_.store(frames, std::memory_order_release);
    return true;
}

void WasapiOutput::close() noexcept {
    if (render_ != nullptr) {
        static_cast<IAudioRenderClient*>(render_)->Release();
        render_ = nullptr;
    }
    if (client_ != nullptr) {
        static_cast<IAudioClient*>(client_)->Release();
        client_ = nullptr;
    }
    if (event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(event_));
        event_ = nullptr;
    }
    left_.clear();
    right_.clear();
}

void WasapiOutput::renderAvailable() {
    if (client_ == nullptr || render_ == nullptr) return;
    auto* client = static_cast<IAudioClient*>(client_);
    auto* render = static_cast<IAudioRenderClient*>(render_);
    UINT32 padding = 0;
    if (FAILED(client->GetCurrentPadding(&padding))) return;
    const auto capacity = bufferFrames_.load(std::memory_order_relaxed);
    if (padding >= capacity) return;
    const UINT32 frames = capacity - padding;
    BYTE* data = nullptr;
    if (FAILED(render->GetBuffer(frames, &data)) || data == nullptr) return;
    auto* interleaved = reinterpret_cast<float*>(data);
    for (UINT32 frame = 0; frame < frames; ++frame) {
        left_[frame] = interleaved[frame * 2];
        right_[frame] = interleaved[frame * 2 + 1];
    }
    float* channels[] = {left_.data(), right_.data()};
    if (callback_ != nullptr) callback_(context_, channels, 2, frames);
    else {
        std::fill_n(left_.data(), frames, 0.f);
        std::fill_n(right_.data(), frames, 0.f);
    }
    for (UINT32 frame = 0; frame < frames; ++frame) {
        interleaved[frame * 2] = left_[frame];
        interleaved[frame * 2 + 1] = right_[frame];
    }
    render->ReleaseBuffer(frames, 0);
}

void CALLBACK midiThunk(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param, DWORD_PTR) {
    if (message != MIM_DATA || instance == 0) return;
    const auto packed = static_cast<DWORD>(param);
    reinterpret_cast<MidiIn*>(instance)->dispatch(
        static_cast<std::uint8_t>(packed & 0xff),
        static_cast<std::uint8_t>((packed >> 8) & 0xff),
        static_cast<std::uint8_t>((packed >> 16) & 0xff));
}

bool MidiIn::open(unsigned index, Callback callback, void* context, std::string& error) {
    close();
    callback_ = callback;
    context_ = context;
    HMIDIIN handle = nullptr;
    const auto result = midiInOpen(&handle, index, reinterpret_cast<DWORD_PTR>(&midiThunk), reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        callback_ = nullptr;
        copyError(error, "the MIDI input could not be opened");
        return false;
    }
    handle_ = handle;
    midiInStart(handle);
    return true;
}

void MidiIn::dispatch(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept {
    if (callback_ != nullptr) callback_(context_, status, data1, data2);
}

void MidiIn::close() {
    if (handle_ == nullptr) return;
    auto* handle = static_cast<HMIDIIN>(handle_);
    midiInReset(handle);
    midiInStop(handle);
    midiInClose(handle);
    handle_ = nullptr;
    callback_ = nullptr;
    context_ = nullptr;
}
} // namespace nodsynth::app
