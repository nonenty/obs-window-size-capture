/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <stdint.h>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <new>
#include <obs-module.h>
#include "process-audio.hpp"

#ifndef KSAUDIO_SPEAKER_2POINT1
#define KSAUDIO_SPEAKER_2POINT1 (KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY)
#endif

/* These structures are part of the Process Loopback API (Windows build 20348+).
 * Some MinGW SDK packages do not ship audioclientactivationparams.h. */
enum AUDIOCLIENT_ACTIVATION_TYPE_LOCAL { AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK_LOCAL = 1 };
enum PROCESS_LOOPBACK_MODE_LOCAL { PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE_LOCAL = 0 };
struct PROCESS_LOOPBACK_PARAMS_LOCAL { DWORD TargetProcessId; DWORD ProcessLoopbackMode; };
struct AUDIOCLIENT_ACTIVATION_PARAMS_LOCAL {
    AUDIOCLIENT_ACTIVATION_TYPE_LOCAL ActivationType;
    PROCESS_LOOPBACK_PARAMS_LOCAL ProcessLoopbackParams;
};
typedef HRESULT (STDAPICALLTYPE *activate_audio_fn)(LPCWSTR, REFIID, PROPVARIANT *,
    IActivateAudioInterfaceCompletionHandler *, IActivateAudioInterfaceAsyncOperation **);

static const GUID iid_audio_client_local = {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const GUID subtype_float_local = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const DWORD speaker_4point1_local = KSAUDIO_SPEAKER_SURROUND | SPEAKER_LOW_FREQUENCY;
static const wchar_t process_loopback_device[] = L"VAD\\Process_Loopback";

class activation_handler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
    std::atomic<ULONG> refs{1};
    HANDLE event_;
    std::mutex mutex_;
    bool cancelled_ = false;
    IAudioClient *client_ = nullptr;
    HRESULT result_ = E_FAIL;
public:
    activation_handler() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~activation_handler() {
        if (client_) client_->Release();
        if (event_) CloseHandle(event_);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, __uuidof(IActivateAudioInterfaceCompletionHandler))) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler *>(this); AddRef(); return S_OK;
        }
        if (IsEqualIID(iid, __uuidof(IAgileObject))) {
            *out = static_cast<IAgileObject *>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --refs; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override {
        IUnknown *object = nullptr;
        HRESULT activation = E_FAIL;
        HRESULT hr = op->GetActivateResult(&activation, &object);
        IAudioClient *client = nullptr;
        HRESULT result = FAILED(hr) ? hr : activation;
        if (SUCCEEDED(result) && object) {
            result = object->QueryInterface(iid_audio_client_local, reinterpret_cast<void **>(&client));
        }
        if (object) object->Release();
        std::lock_guard<std::mutex> guard(mutex_);
        result_ = result;
        if (cancelled_) {
            if (client) client->Release();
        } else {
            client_ = client;
        }
        SetEvent(event_);
        return S_OK;
    }
    HANDLE event() const { return event_; }
    HRESULT result() { std::lock_guard<std::mutex> guard(mutex_); return result_; }
    IAudioClient *take_client() {
        std::lock_guard<std::mutex> guard(mutex_);
        IAudioClient *p = client_; client_ = nullptr; return p;
    }
    void cancel() {
        std::lock_guard<std::mutex> guard(mutex_);
        cancelled_ = true;
        if (client_) { client_->Release(); client_ = nullptr; }
        if (WaitForSingleObject(event_, 0) == WAIT_TIMEOUT) {
            /* A cancelled OS activation may call back after OBS unloads its
             * modules. That callback owns its event/client and no OBS state.
             * Keep its code mapped until process exit as well. */
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&pid_audio_start), &module);
        }
    }
};

struct pid_audio_capture {
    obs_weak_source_t *source;
    DWORD pid;
    HANDLE stop_event;
    std::thread worker;
    std::atomic<bool> finished{false};
};

static bool stopping(const pid_audio_capture *capture)
{
    return WaitForSingleObject(capture->stop_event, 0) != WAIT_TIMEOUT;
}

static speaker_layout layout_for_channels(WORD channels, DWORD mask)
{
    if (mask == KSAUDIO_SPEAKER_2POINT1) return SPEAKERS_2POINT1;
    if (mask == KSAUDIO_SPEAKER_SURROUND) return SPEAKERS_4POINT0;
    if (mask == speaker_4point1_local) return SPEAKERS_4POINT1;
    if (mask == KSAUDIO_SPEAKER_5POINT1_SURROUND) return SPEAKERS_5POINT1;
    if (mask == KSAUDIO_SPEAKER_7POINT1_SURROUND) return SPEAKERS_7POINT1;
    if (channels == 1) return SPEAKERS_MONO;
    if (channels == 2) return SPEAKERS_STEREO;
    return (speaker_layout)channels;
}

static void capture_thread(pid_audio_capture *capture)
{
    HRESULT init_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init_hr)) {
        blog(LOG_WARNING, "[window-size-capture] PID %lu: CoInitializeEx failed: 0x%08lX",
             (unsigned long)capture->pid, (unsigned long)init_hr);
        capture->finished = true;
        return;
    }
    HMODULE mmdevapi = LoadLibraryW(L"Mmdevapi.dll");
    activate_audio_fn activate = nullptr;
    if (mmdevapi) { FARPROC address = GetProcAddress(mmdevapi, "ActivateAudioInterfaceAsync"); memcpy(&activate, &address, sizeof(activate)); }
    IAudioClient *client = nullptr;
    IAudioCaptureClient *capture_client = nullptr;
    HANDLE packet = nullptr;
    if (stopping(capture)) goto done;
    if (!activate) {
        blog(LOG_WARNING, "[window-size-capture] PID %lu: process loopback API/event unavailable",
             (unsigned long)capture->pid);
        goto done;
    }

    {
        AUDIOCLIENT_ACTIVATION_PARAMS_LOCAL params{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK_LOCAL;
        params.ProcessLoopbackParams.TargetProcessId = capture->pid;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE_LOCAL;
        PROPVARIANT value{};
        value.vt = VT_BLOB;
        value.blob.cbSize = sizeof(params);
        value.blob.pBlobData = reinterpret_cast<BYTE *>(&params);
        auto *handler = new activation_handler();
        if (!handler->event()) { handler->Release(); goto done; }
        IActivateAudioInterfaceAsyncOperation *operation = nullptr;
        HRESULT hr = activate(process_loopback_device, iid_audio_client_local, &value, handler, &operation);
        HANDLE activation_events[] = {capture->stop_event, handler->event()};
        DWORD wait_result = SUCCEEDED(hr) ? WaitForMultipleObjects(2, activation_events, FALSE, 10000) : WAIT_FAILED;
        if (SUCCEEDED(hr) && wait_result == WAIT_OBJECT_0 + 1 && !stopping(capture) && SUCCEEDED(handler->result()))
            client = handler->take_client();
        if (!client && !stopping(capture)) {
            HRESULT activation_result = FAILED(hr) ? hr : wait_result == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : handler->result();
            blog(LOG_WARNING, "[window-size-capture] PID %lu: process loopback activation failed: 0x%08lX",
                 (unsigned long)capture->pid, (unsigned long)activation_result);
        }
        handler->cancel();
        if (operation) operation->Release();
        handler->Release();
        if (!client) goto done;
    }

    {
        if (stopping(capture)) goto done;
        obs_audio_info audio{};
        if (!obs_get_audio_info(&audio)) goto done;
        const WORD channels = audio.speakers == SPEAKERS_MONO ? 1 : audio.speakers == SPEAKERS_STEREO ? 2 :
            audio.speakers == SPEAKERS_2POINT1 ? 3 : audio.speakers == SPEAKERS_4POINT0 ? 4 :
            audio.speakers == SPEAKERS_4POINT1 ? 5 : audio.speakers == SPEAKERS_5POINT1 ? 6 : 8;
        WAVEFORMATEXTENSIBLE format{};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = channels;
        format.Format.nSamplesPerSec = audio.samples_per_sec;
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = channels * sizeof(float);
        format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
        format.Format.cbSize = sizeof(format) - sizeof(format.Format);
        format.Samples.wValidBitsPerSample = 32;
        format.SubFormat = subtype_float_local;
        switch (audio.speakers) {
        case SPEAKERS_MONO: format.dwChannelMask = KSAUDIO_SPEAKER_MONO; break;
        case SPEAKERS_STEREO: format.dwChannelMask = KSAUDIO_SPEAKER_STEREO; break;
        case SPEAKERS_2POINT1: format.dwChannelMask = KSAUDIO_SPEAKER_2POINT1; break;
        case SPEAKERS_4POINT0: format.dwChannelMask = KSAUDIO_SPEAKER_SURROUND; break;
        case SPEAKERS_4POINT1: format.dwChannelMask = speaker_4point1_local; break;
        case SPEAKERS_5POINT1: format.dwChannelMask = KSAUDIO_SPEAKER_5POINT1_SURROUND; break;
        default: format.dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND; break;
        }
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_LOOPBACK,
                                10000000, 0, &format.Format, nullptr);
        if (FAILED(hr)) {
            blog(LOG_WARNING, "[window-size-capture] PID %lu: IAudioClient::Initialize failed: 0x%08lX",
                 (unsigned long)capture->pid, (unsigned long)hr);
            goto done;
        }
        packet = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = packet ? client->SetEventHandle(packet) : HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture_client));
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr)) {
            blog(LOG_WARNING, "[window-size-capture] PID %lu: audio capture client start failed: 0x%08lX",
                 (unsigned long)capture->pid, (unsigned long)hr);
            goto done;
        }
        blog(LOG_INFO, "[window-size-capture] PID %lu: process loopback started", (unsigned long)capture->pid);

        HANDLE events[] = {capture->stop_event, packet};
        while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            UINT32 frames = 0;
            BYTE *buffer = nullptr;
            DWORD flags = 0;
            UINT64 device_pos = 0, qpc_pos = 0;
            while (!stopping(capture)) {
                hr = capture_client->GetNextPacketSize(&frames);
                if (FAILED(hr)) goto audio_failed;
                if (!frames) break;
                hr = capture_client->GetBuffer(&buffer, &frames, &flags, &device_pos, &qpc_pos);
                if (FAILED(hr)) goto audio_failed;
                std::vector<float> silence;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silence.resize((size_t)frames * channels, 0.0f);
                    buffer = reinterpret_cast<BYTE *>(silence.data());
                }
                obs_source_audio out{};
                out.data[0] = buffer;
                out.frames = frames;
                out.speakers = layout_for_channels(channels, format.dwChannelMask);
                out.format = AUDIO_FORMAT_FLOAT;
                out.samples_per_sec = format.Format.nSamplesPerSec;
                out.timestamp = qpc_pos * 100;
                /* A packet holds a source reference only while delivering it.
                 * Retirement may outlive the source, without a dangling pointer
                 * or a permanent reference cycle that prevents destruction. */
                obs_source_t *source = !stopping(capture) ? obs_weak_source_get_source(capture->source) : nullptr;
                if (source) {
                    obs_source_output_audio(source, &out);
                    obs_source_release(source);
                }
                hr = capture_client->ReleaseBuffer(frames);
                if (FAILED(hr)) goto audio_failed;
                if (!source) { SetEvent(capture->stop_event); break; }
            }
        }
        goto done;
audio_failed:
        blog(LOG_WARNING, "[window-size-capture] PID %lu: audio capture packet failed: 0x%08lX",
             (unsigned long)capture->pid, (unsigned long)hr);
    }
done:
    if (client) client->Stop();
    if (capture_client) capture_client->Release();
    if (client) client->Release();
    if (packet) CloseHandle(packet);
    if (mmdevapi) FreeLibrary(mmdevapi);
    CoUninitialize();
    capture->finished = true;
}

extern "C" pid_audio_capture *pid_audio_start(obs_source_t *source, DWORD pid)
{
    auto *capture = new (std::nothrow) pid_audio_capture{};
    if (!capture) return nullptr;
    capture->source = obs_source_get_weak_source(source);
    capture->pid = pid;
    capture->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!capture->stop_event || !capture->source) {
        if (capture->stop_event) CloseHandle(capture->stop_event);
        obs_weak_source_release(capture->source);
        delete capture;
        return nullptr;
    }
    try {
        capture->worker = std::thread(capture_thread, capture);
    } catch (...) {
        CloseHandle(capture->stop_event);
        obs_weak_source_release(capture->source);
        delete capture;
        return nullptr;
    }
    return capture;
}

static void dispose_audio_capture(void *data)
{
    auto *capture = static_cast<pid_audio_capture *>(data);
    if (capture->worker.joinable()) capture->worker.join();
    CloseHandle(capture->stop_event);
    obs_weak_source_release(capture->source);
    delete capture;
}

extern "C" void pid_audio_request_stop(pid_audio_capture *capture)
{
    if (capture) SetEvent(capture->stop_event);
}

extern "C" void pid_audio_stop(pid_audio_capture *capture)
{
    if (!capture) return;
    pid_audio_request_stop(capture);
    obs_queue_task(OBS_TASK_DESTROY, dispose_audio_capture, capture, false);
}

extern "C" bool pid_audio_finished(pid_audio_capture *capture)
{
    return !capture || capture->finished.load();
}
