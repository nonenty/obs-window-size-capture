/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Cancellation, runtime failure and retirement tests use the real audio worker.
 * Only activation lookup is replaced for the deterministic fault cases. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static FARPROC WINAPI test_get_proc_address(HMODULE module, LPCSTR name);
#define GetProcAddress test_get_proc_address
#include "../process-audio.cpp"
#undef GetProcAddress
#include <util/platform.h>
#include <cstdio>

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

enum class packet_failure { next_packet, get_buffer, release_buffer };

class fault_capture_client final : public IAudioCaptureClient {
public:
    packet_failure failure;
    ULONG refs = 1;
    int next_calls = 0, buffer_calls = 0, release_calls = 0;
    explicit fault_capture_client(packet_failure failure) : failure(failure) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE GetNextPacketSize(UINT32 *frames) override {
        ++next_calls;
        *frames = 1;
        return failure == packet_failure::next_packet ? AUDCLNT_E_DEVICE_INVALIDATED : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(BYTE **data, UINT32 *frames, DWORD *flags,
                                         UINT64 *device, UINT64 *qpc) override {
        ++buffer_calls;
        if (failure == packet_failure::get_buffer) return AUDCLNT_E_DEVICE_INVALIDATED;
        *data = nullptr; *frames = 1; *flags = AUDCLNT_BUFFERFLAGS_SILENT;
        *device = 0; *qpc = os_gettime_ns() / 100;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32) override {
        ++release_calls;
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }
};

class fault_audio_client final : public IAudioClient {
    HANDLE packet = nullptr;
public:
    fault_capture_client capture;
    ULONG refs = 1;
    int stops = 0;
    explicit fault_audio_client(packet_failure failure) : capture(failure) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        *out = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, iid_audio_client_local)) return E_NOINTERFACE;
        *out = static_cast<IAudioClient *>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME,
                                          const WAVEFORMATEX *, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX *, WAVEFORMATEX **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME *, REFERENCE_TIME *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Start() override { SetEvent(packet); return S_OK; }
    HRESULT STDMETHODCALLTYPE Stop() override { ++stops; return S_OK; }
    HRESULT STDMETHODCALLTYPE Reset() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE event) override { packet = event; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetService(REFIID iid, void **out) override {
        *out = nullptr;
        if (!IsEqualIID(iid, __uuidof(IAudioCaptureClient))) return E_NOINTERFACE;
        *out = static_cast<IAudioCaptureClient *>(&capture); capture.AddRef(); return S_OK;
    }
};

static fault_audio_client *injected_client;

class fault_activation final : public IActivateAudioInterfaceAsyncOperation {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetActivateResult(HRESULT *result, IUnknown **object) override {
        *result = S_OK; *object = injected_client; injected_client->AddRef(); return S_OK;
    }
};

static HRESULT STDAPICALLTYPE activate_fault_client(LPCWSTR, REFIID, PROPVARIANT *,
    IActivateAudioInterfaceCompletionHandler *handler, IActivateAudioInterfaceAsyncOperation **operation)
{
    *operation = nullptr;
    fault_activation result;
    return handler->ActivateCompleted(&result);
}

static FARPROC WINAPI test_get_proc_address(HMODULE module, LPCSTR name)
{
    if (injected_client && strcmp(name, "ActivateAudioInterfaceAsync") == 0) {
        activate_audio_fn activate = activate_fault_client;
        FARPROC address;
        memcpy(&address, &activate, sizeof(address));
        return address;
    }
    return GetProcAddress(module, name);
}

static void check_runtime_failure(obs_source_t *source, packet_failure failure)
{
    fault_audio_client client(failure);
    injected_client = &client;
    // A second start with the same PID must reach the driver again, rather
    // than leaving an unfinished, silent worker attached to the source.
    for (int attempt = 0; attempt < 2; ++attempt) {
        pid_audio_capture *capture = pid_audio_start(source, GetCurrentProcessId());
        CHECK(capture != nullptr);
        if (!capture) break;
        ULONGLONG deadline = GetTickCount64() + 1500;
        while (!pid_audio_finished(capture) && GetTickCount64() < deadline) Sleep(1);
        CHECK(pid_audio_finished(capture));
        CHECK(!stopping(capture));
        // Also bound cleanup if a regression leaves the worker waiting.
        pid_audio_request_stop(capture);
        capture->worker.join();
        CHECK(client.stops == attempt + 1);
        CHECK(client.refs == 1 && client.capture.refs == 1);
        CHECK(client.capture.next_calls == attempt + 1);
        CHECK(client.capture.buffer_calls == (failure == packet_failure::next_packet ? 0 : attempt + 1));
        CHECK(client.capture.release_calls == (failure == packet_failure::release_buffer ? attempt + 1 : 0));
        pid_audio_stop(capture);
        obs_queue_task(OBS_TASK_DESTROY, [](void *) {}, nullptr, true);
    }
    injected_client = nullptr;
}

class failed_activation final : public IActivateAudioInterfaceAsyncOperation {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetActivateResult(HRESULT *result, IUnknown **object) override {
        *result = E_FAIL;
        *object = nullptr;
        return S_OK;
    }
};

static void check_late_callback()
{
    auto *handler = new activation_handler();
    HANDLE event = handler->event();
    CHECK(event != nullptr);
    handler->AddRef(); // The activation API retains this reference until callback.
    handler->cancel();
    handler->Release(); // The cancelled capture worker has already exited.
    DWORD flags = 0;
    CHECK(GetHandleInformation(event, &flags));
    failed_activation operation;
    CHECK(SUCCEEDED(handler->ActivateCompleted(&operation)));
    CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
    CHECK(handler->take_client() == nullptr);
    handler->Release();
    CHECK(!GetHandleInformation(event, &flags) && GetLastError() == ERROR_INVALID_HANDLE);
}

static const char *test_name(void *) { return "Audio lifecycle test"; }
static void *test_create(obs_data_t *, obs_source_t *) { return calloc(1, 1); }
static void test_destroy(void *data) { free(data); }
static void no_op(void *) {}

struct stop_measurement { pid_audio_capture *capture; ULONGLONG elapsed; };
static void stop_on_video_thread(void *data)
{
    auto *measurement = static_cast<stop_measurement *>(data);
    ULONGLONG start = GetTickCount64();
    pid_audio_stop(measurement->capture);
    measurement->elapsed = GetTickCount64() - start;
}

static void check_nonblocking_stop(obs_source_t *source)
{
    HANDLE release_worker = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> exited{false};
    auto *capture = new pid_audio_capture{};
    capture->source = obs_source_get_weak_source(source);
    capture->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    capture->worker = std::thread([&exited, release_worker, capture] {
        // A slow driver must not hold OBS's video thread hostage.
        WaitForSingleObject(release_worker, 3000);
        exited = true;
        capture->finished = true;
    });
    stop_measurement measurement{capture, 0};
    obs_queue_task(OBS_TASK_GRAPHICS, stop_on_video_thread, &measurement, true);
    CHECK(measurement.elapsed < 250);
    CHECK(!exited.load());
    obs_queue_task(OBS_TASK_GRAPHICS, no_op, nullptr, true);
    SetEvent(release_worker);
    obs_queue_task(OBS_TASK_DESTROY, no_op, nullptr, true);
    CHECK(exited.load());
    CloseHandle(release_worker);
}

int main()
{
    check_late_callback();
    CHECK(obs_startup("en-US", nullptr, nullptr));
    obs_audio_info audio{};
    audio.samples_per_sec = 48000;
    audio.speakers = SPEAKERS_STEREO;
    CHECK(obs_reset_audio(&audio));
    obs_video_info video{};
    video.graphics_module = "libobs-d3d11";
    video.fps_num = 30; video.fps_den = 1;
    video.base_width = video.output_width = 320;
    video.base_height = video.output_height = 240;
    video.output_format = VIDEO_FORMAT_RGBA;
    video.colorspace = VIDEO_CS_709;
    video.range = VIDEO_RANGE_FULL;
    CHECK(obs_reset_video(&video) == OBS_VIDEO_SUCCESS);
    obs_source_info info{};
    info.id = "audio_retirement_test";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = test_name; info.create = test_create; info.destroy = test_destroy;
    obs_register_source(&info);
    obs_source_t *source = obs_source_create_private(info.id, "Audio retirement", nullptr);
    CHECK(source != nullptr);
    if (source) {
        check_runtime_failure(source, packet_failure::next_packet);
        check_runtime_failure(source, packet_failure::get_buffer);
        check_runtime_failure(source, packet_failure::release_buffer);
        check_nonblocking_stop(source);
        // Cover cancellation before initialization and after activation starts.
        for (int i = 0; i < 12; ++i) {
            pid_audio_capture *capture = pid_audio_start(source, GetCurrentProcessId());
            CHECK(capture != nullptr);
            if (i % 3 == 2) Sleep(120);
            stop_measurement measurement{capture, 0};
            obs_queue_task(OBS_TASK_GRAPHICS, stop_on_video_thread, &measurement, true);
            CHECK(measurement.elapsed < 250);
        }
        obs_source_release(source);
        obs_wait_for_destroy_queue();
    }
    obs_shutdown();
    printf("Audio runtime failure/cancellation/retirement tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
