/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Use OBS's real video and destruction queues, rather than manually tearing
 * down captures on the test's main thread. A watchdog in run.ps1 bounds hangs. */
#define CAPTURE_WORKER
#include "../window-size-capture.c"

static LONG failures, wrong_thread_frees, capture_frees;
static capture_free_fn real_capture_free;
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        InterlockedIncrement(&failures); \
    } \
} while (0)

static void checked_capture_free(struct winrt_capture *capture)
{
    if (!obs_in_task_thread(OBS_TASK_GRAPHICS))
        InterlockedIncrement(&wrong_thread_frees);
    InterlockedIncrement(&capture_frees);
    real_capture_free(capture);
}

struct probe { obs_source_t *source; int captures; bool audio; };
static void pump_messages(void);
struct queued_call { obs_task_t task; void *data; HANDLE finished; };
static void run_queued_call(void *data)
{
    struct queued_call *call = data;
    call->task(call->data);
    SetEvent(call->finished);
}

static void graphics_call(obs_task_t task, void *data)
{
    struct queued_call call = {.task = task, .data = data,
                               .finished = CreateEventW(NULL, TRUE, FALSE, NULL)};
    obs_queue_task(OBS_TASK_GRAPHICS, run_queued_call, &call, false);
    // GetWindowText on this test's own windows sends WM_GETTEXT to their
    // owner thread. Never wait synchronously without pumping those messages.
    while (WaitForSingleObject(call.finished, 0) == WAIT_TIMEOUT) {
        pump_messages();
        Sleep(1);
    }
    CloseHandle(call.finished);
}

static void inspect_source(void *data)
{
    struct probe *probe = data;
    struct size_capture *self = obs_obj_get_data(probe->source);
    probe->captures = 0;
    probe->audio = false;
    if (!self) return;
    if (self->api.free != checked_capture_free) {
        real_capture_free = self->api.free;
        self->api.free = checked_capture_free;
    }
    if (self->capture && self->api.active(self->capture))
        ++probe->captures;
    for (struct process_window *item = self->process_windows; item; item = item->next) {
        if (item->capture && self->api.active(item->capture))
            ++probe->captures;
    }
    probe->audio = self->audio_capture != NULL;
}

static void pump_messages(void)
{
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

static bool wait_for_source(obs_source_t *source, int captures, bool audio)
{
    ULONGLONG deadline = GetTickCount64() + 3000;
    do {
        pump_messages();
        struct probe probe = {.source = source};
        graphics_call(inspect_source, &probe);
        if (probe.captures == captures && probe.audio == audio)
            return true;
        Sleep(20);
    } while (GetTickCount64() < deadline);
    return false;
}

static void no_op(void *data) { UNUSED_PARAMETER(data); }

int main(void)
{
    CHECK(obs_startup("en-US", NULL, NULL));
    struct obs_audio_info audio = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
    CHECK(obs_reset_audio(&audio));
    struct obs_video_info video = {
        .graphics_module = "libobs-d3d11", .fps_num = 30, .fps_den = 1,
        .base_width = 400, .base_height = 320, .output_width = 400, .output_height = 320,
        .output_format = VIDEO_FORMAT_RGBA, .colorspace = VIDEO_CS_709,
        .range = VIDEO_RANGE_FULL, .scale_type = OBS_SCALE_DISABLE,
    };
    CHECK(obs_reset_video(&video) == OBS_VIDEO_SUCCESS);
    CHECK(obs_module_load());
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW window_class = {.lpfnWndProc = DefWindowProcW,
                             .hInstance = GetModuleHandleW(NULL),
                             .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
                             .lpszClassName = L"CPObsLifecycleTest"};
    CHECK(RegisterClassW(&window_class) != 0);
    HWND root = CreateWindowExW(0, window_class.lpszClassName, L"CP OBS lifecycle root",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 120, 120, 400, 320,
                                NULL, NULL, window_class.hInstance, NULL);
    CHECK(root != NULL);
    for (int i = 0; i < 6; ++i) {
        bool composite = i % 2 == 0;
        HWND child = CreateWindowExW(WS_EX_TOOLWINDOW, window_class.lpszClassName, L"Child title",
                                     WS_OVERLAPPEDWINDOW | WS_VISIBLE, 180, 180, 180, 140,
                                     root, NULL, window_class.hInstance, NULL);
        CHECK(child != NULL);
        RECT client;
        GetClientRect(root, &client);
        obs_data_t *settings = obs_data_create();
        source_defaults(settings);
        struct window_info info = {0};
        CHECK(inspect_window(root, true, composite, &info));
        char *exe = utf8_copy(info.exe);
        obs_data_set_string(settings, "exe", exe);
        free(exe);
        obs_data_set_string(settings, "title", "CP OBS lifecycle root");
        obs_data_set_int(settings, "width", client.right);
        obs_data_set_int(settings, "height", client.bottom);
        obs_data_set_bool(settings, "include_children", composite);
        obs_data_set_bool(settings, "capture_audio", true);
        obs_source_t *source = obs_source_create_private("window_size_capture", "Lifecycle capture", settings);
        CHECK(source != NULL);
        if (!source) { obs_data_release(settings); DestroyWindow(child); break; }
        obs_source_inc_showing(source);
        obs_set_output_source(0, source);
        CHECK(wait_for_source(source, composite ? 2 : 1, true));

        obs_data_set_bool(settings, "capture_audio", false);
        obs_source_update(source, settings);
        CHECK(wait_for_source(source, composite ? 2 : 1, false));
        obs_data_set_bool(settings, "capture_audio", true);
        obs_source_update(source, settings);
        CHECK(wait_for_source(source, composite ? 2 : 1, true));

        obs_set_output_source(0, NULL);
        obs_source_dec_showing(source);
        CHECK(wait_for_source(source, composite ? 0 : 1, false));
        obs_source_inc_showing(source);
        obs_set_output_source(0, source);
        CHECK(wait_for_source(source, composite ? 2 : 1, true));
        DestroyWindow(child);
        CHECK(wait_for_source(source, 1, true));

        obs_set_output_source(0, NULL);
        obs_source_dec_showing(source);
        obs_source_release(source); // OBS_TASK_DESTROY invokes source_destroy.
        obs_data_release(settings);
        obs_wait_for_destroy_queue();
        graphics_call(no_op, NULL);
        obs_queue_task(OBS_TASK_DESTROY, no_op, NULL, true);
    }
    CHECK(capture_frees > 0);
    CHECK(wrong_thread_frees == 0);
    DestroyWindow(root);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    obs_shutdown();
    printf("OBS source lifecycle tests: %s (%ld failures, %ld captures retired, %ld on wrong thread)\n",
           failures ? "FAIL" : "PASS", failures, capture_frees, wrong_thread_frees);
    return failures ? 1 : 0;
}
