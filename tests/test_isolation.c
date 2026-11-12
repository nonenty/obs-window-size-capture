/* SPDX-License-Identifier: GPL-2.0-or-later */
/* End-to-end isolation tests: actual WGC pixels cross a process boundary;
 * injected capture-close hangs must neither stop OBS frames nor survive teardown. */
#include "../capture-host.c"
#include "../window-size-capture.c"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static void check_heartbeat_timeout(void)
{
    const ULONGLONG started = 1000, heartbeat = 2000;
    CHECK(!worker_timed_out(heartbeat - 1, heartbeat, started));
    CHECK(!worker_timed_out(heartbeat, heartbeat, started));
    CHECK(!worker_timed_out(heartbeat + CAPTURE_HEARTBEAT_TIMEOUT_MS, heartbeat, started));
    CHECK(worker_timed_out(heartbeat + CAPTURE_HEARTBEAT_TIMEOUT_MS + 1, heartbeat, started));
    CHECK(!worker_timed_out(started - 1, 0, started));
    CHECK(!worker_timed_out(started + CAPTURE_STARTUP_TIMEOUT_MS, 0, started));
    CHECK(worker_timed_out(started + CAPTURE_STARTUP_TIMEOUT_MS + 1, 0, started));
}
static void pump(void)
{
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
struct call { obs_task_t task; void *data; HANDLE done; };
static void called(void *data)
{
    struct call *call = data; call->task(call->data); SetEvent(call->done);
}
static void on_graphics(obs_task_t task, void *data)
{
    struct call call = {.task = task, .data = data, .done = CreateEventW(NULL, TRUE, FALSE, NULL)};
    obs_queue_task(OBS_TASK_GRAPHICS, called, &call, false);
    ULONGLONG deadline = GetTickCount64() + 2000;
    while (WaitForSingleObject(call.done, 0) != WAIT_OBJECT_0) {
        pump(); Sleep(2);
        if (GetTickCount64() > deadline) {
            fprintf(stderr, "FAIL: OBS graphics queue blocked\n"); ExitProcess(1);
        }
    }
    CloseHandle(call.done);
}
static LRESULT CALLBACK paint(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(window, &ps);
        HBRUSH brush = CreateSolidBrush((COLORREF)GetWindowLongPtrW(window, GWLP_USERDATA));
        FillRect(dc, &ps.rcPaint, brush); DeleteObject(brush); EndPaint(window, &ps); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
static HWND make_window(HWND owner, int x, int y, int width, int height, COLORREF color)
{
    HWND window = CreateWindowExW(owner ? WS_EX_TOOLWINDOW : 0, L"CPObsIsolationTest", L"Process isolation test",
                                  WS_POPUP, x, y, width, height, owner, NULL, GetModuleHandleW(NULL), NULL);
    SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)color);
    ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window); return window;
}
struct probe {
    obs_source_t *source;
    bool red, overlay, frame, parent_has_no_wgc;
    uint32_t width, height;
    LONG launches, worker_pid, stage;
    DWORD target_pid;
    int command;
};
static void inspect(void *data)
{
    struct probe *p = data;
    struct size_capture *self = obs_obj_get_data(p->source);
    struct capture_host *host = self->host;
    CHECK(host != NULL);
    if (!host) return;
    p->parent_has_no_wgc = self->api.module == NULL && self->process_windows == NULL && self->capture == NULL;
    p->launches = InterlockedCompareExchange(&host->launches, 0, 0);
    p->worker_pid = InterlockedCompareExchange(&host->worker_pid, 0, 0);
    p->target_pid = capture_host_target_pid(host);
    p->width = source_width(self); p->height = source_height(self);
    p->frame = host->have_frame;
    EnterCriticalSection(&host->frame_lock);
    if (host->session) {
        p->stage = InterlockedCompareExchange(&host->session->ipc->test_stage, 0, 0);
        if (p->command) InterlockedExchange(&host->session->ipc->test_command, p->command);
    }
    LeaveCriticalSection(&host->frame_lock);
    if (!host->texture || !host->have_frame) return;
    obs_enter_graphics();
    gs_stagesurf_t *stage = gs_stagesurface_create(p->width, p->height, GS_BGRA);
    gs_texrender_t *render = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    if (stage && render && gs_texrender_begin(render, p->width, p->height)) {
        struct vec4 clear; vec4_zero(&clear); gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
        gs_ortho(0.0f, (float)p->width, 0.0f, (float)p->height, -100.0f, 100.0f);
        source_render(self, NULL);
        gs_texrender_end(render);
        gs_stage_texture(stage, gs_texrender_get_texture(render));
        uint8_t *pixels; uint32_t stride;
        if (gs_stagesurface_map(stage, &pixels, &stride)) {
            const uint8_t *red = pixels + 20 * stride + 20 * 4;
            const uint8_t *blue = pixels + 80 * stride + 80 * 4;
            p->red = red[0] < 20 && red[1] < 20 && red[2] > 235 && red[3] > 235;
            p->overlay = blue[0] > 235 && blue[1] < 20 && blue[2] < 20 && blue[3] > 235;
            gs_stagesurface_unmap(stage);
        }
    }
    gs_stagesurface_destroy(stage);
    gs_texrender_destroy(render);
    obs_leave_graphics();
}
static bool await_image(obs_source_t *source, bool overlay, uint32_t width, uint32_t height, DWORD timeout)
{
    ULONGLONG deadline = GetTickCount64() + timeout;
    do {
        struct probe p = {.source = source}; on_graphics(inspect, &p);
        if (p.red && p.overlay == overlay && p.width == width && p.height == height && p.target_pid == GetCurrentProcessId()) {
            CHECK(p.parent_has_no_wgc); return true;
        }
        pump(); Sleep(20);
    } while (GetTickCount64() < deadline);
    return false;
}
static void no_op(void *unused) { UNUSED_PARAMETER(unused); }
static bool await_stage(obs_source_t *source, LONG expected)
{
    ULONGLONG end = GetTickCount64() + 3000;
    do {
        struct probe p = {.source = source}; on_graphics(inspect, &p);
        if (p.stage == expected) return true;
        Sleep(20); pump();
    } while (GetTickCount64() < end);
    return false;
}
static void freeze_next_close(obs_source_t *source, HWND popup)
{
    struct probe p = {.source = source, .command = 1}; on_graphics(inspect, &p);
    DestroyWindow(popup);
    CHECK(await_stage(source, 1));
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    check_heartbeat_timeout();
    CHECK(obs_startup("en-US", NULL, NULL));
    struct obs_audio_info audio = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
    CHECK(obs_reset_audio(&audio));
    struct obs_video_info video = {.graphics_module = "libobs-d3d11", .fps_num = 30, .fps_den = 1,
        .base_width = 640, .base_height = 480, .output_width = 640, .output_height = 480,
        .output_format = VIDEO_FORMAT_RGBA, .colorspace = VIDEO_CS_709, .range = VIDEO_RANGE_FULL,
        .scale_type = OBS_SCALE_DISABLE};
    CHECK(obs_reset_video(&video) == OBS_VIDEO_SUCCESS); CHECK(obs_module_load());
    /* The test runner needs OBS in PATH to load this test EXE. Remove that
     * convenience before launching children, as in a normal OBS deployment.
     * A parent DLL directory also removes the child's current directory from
     * dependency lookup. The worker must find OBS DLLs beside its own EXE. */
    wchar_t system_directory[MAX_PATH];
    CHECK(GetSystemDirectoryW(system_directory, MAX_PATH) > 0);
    CHECK(SetEnvironmentVariableW(L"PATH", system_directory));
    CHECK(SetDllDirectoryW(system_directory));
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc = {.lpfnWndProc = paint, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"CPObsIsolationTest"};
    CHECK(RegisterClassW(&wc) != 0);
    HWND root = make_window(NULL, 200, 200, 400, 300, RGB(255, 0, 0));
    HWND popup = make_window(root, 250, 250, 100, 90, RGB(0, 0, 255));
    CHECK(root && popup);
    struct window_info info = {0}; CHECK(inspect_window(root, false, true, &info));
    char *exe = utf8_copy(info.exe);
    obs_data_t *settings = obs_data_create(); source_defaults(settings);
    obs_data_set_string(settings, "exe", exe); free(exe);
    obs_data_set_int(settings, "width", 400); obs_data_set_int(settings, "height", 300);
    obs_data_set_bool(settings, "client_area", false);
    obs_data_set_bool(settings, "include_children", true);
    obs_source_t *source = obs_source_create_private("window_size_capture", "Isolation regression", settings);
    CHECK(source != NULL);
    if (!source) return 1;
    obs_source_inc_showing(source); obs_set_output_source(0, source);
    bool initial_image = await_image(source, true, 400, 300, 15000);
    CHECK(initial_image);
    if (!initial_image) {
        obs_set_output_source(0, NULL); obs_source_dec_showing(source); obs_source_release(source);
        obs_data_release(settings); obs_wait_for_destroy_queue();
        DestroyWindow(popup); DestroyWindow(root); obs_shutdown();
        fprintf(stderr, "FAIL: worker did not deliver pixels without inherited OBS PATH\n");
        return 1;
    }
    printf("Worker deployment environment: PASS (worker beside OBS DLLs, no OBS in parent PATH, inherited DLL directory, real pixels)\n");
    struct probe original = {.source = source}; on_graphics(inspect, &original);
    CHECK(original.worker_pid && (DWORD)original.worker_pid != GetCurrentProcessId());

    /* Check the actual worker video thread, even on a 100% scale desktop. */
    struct probe dpi = {.source = source, .command = 2}; on_graphics(inspect, &dpi);
    CHECK(await_stage(source, 2));

    /* Ordinary scene hiding must retain the same worker/capture sessions. */
    obs_set_output_source(0, NULL); obs_source_dec_showing(source);
    ULONGLONG until = GetTickCount64() + 500;
    while (GetTickCount64() < until) { pump(); Sleep(10); }
    obs_source_inc_showing(source); obs_set_output_source(0, source);
    CHECK(await_image(source, true, 400, 300, 3000));
    struct probe shown = {.source = source}; on_graphics(inspect, &shown);
    CHECK(shown.launches == original.launches && shown.worker_pid == original.worker_pid);

    /* Exact failure class from the real dump; leave one slot WRITING. */
    freeze_next_close(source, popup);
    uint32_t before = obs_get_total_frames();
    until = GetTickCount64() + 2200;
    while (GetTickCount64() < until) { on_graphics(no_op, NULL); pump(); Sleep(20); }
    CHECK(obs_get_total_frames() > before + 45);
    CHECK(await_image(source, false, 400, 300, 13000));
    struct probe recovered = {.source = source}; on_graphics(inspect, &recovered);
    /* await_image may initially see the cached old frame. Require a new PID. */
    until = GetTickCount64() + 13000;
    while (recovered.worker_pid == original.worker_pid && GetTickCount64() < until) {
        pump(); Sleep(25); on_graphics(inspect, &recovered);
    }
    CHECK(recovered.worker_pid && recovered.worker_pid != original.worker_pid);
    CHECK(recovered.launches > original.launches);
    CHECK(await_image(source, false, 400, 300, 5000));

    /* An audio-only update reuses video and starts process loopback in OBS. */
    obs_data_set_bool(settings, "capture_audio", true);
    obs_source_update(source, settings);
    CHECK(await_image(source, false, 400, 300, 3000));
    struct probe audio_only = {.source = source}; on_graphics(inspect, &audio_only);
    CHECK(audio_only.worker_pid == recovered.worker_pid);

    /* Frame transport grows and restores the resized anchor after restart. */
    SetWindowPos(root, NULL, 0, 0, 520, 360, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    CHECK(await_image(source, false, 520, 360, 12000));
    /* Recreated main HWND must reconnect using saved executable/dimensions. */
    DestroyWindow(root);
    root = make_window(NULL, 200, 200, 400, 300, RGB(255, 0, 0));
    CHECK(await_image(source, false, 400, 300, 5000));
    popup = make_window(root, 250, 250, 100, 90, RGB(0, 0, 255));
    CHECK(await_image(source, true, 400, 300, 4000));

    /* Destroying a source with a wedged worker must also leave OBS running. */
    freeze_next_close(source, popup);
    struct probe doomed = {.source = source}; on_graphics(inspect, &doomed);
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)doomed.worker_pid);
    CHECK(child != NULL);
    before = obs_get_total_frames();
    obs_set_output_source(0, NULL); obs_source_dec_showing(source); obs_source_release(source);
    obs_data_release(settings);
    until = GetTickCount64() + 4000;
    while (child && WaitForSingleObject(child, 0) != WAIT_OBJECT_0 && GetTickCount64() < until) {
        pump(); on_graphics(no_op, NULL); Sleep(20);
    }
    CHECK(child && WaitForSingleObject(child, 0) == WAIT_OBJECT_0);
    CHECK(obs_get_total_frames() > before + 10);
    if (child) CloseHandle(child);
    obs_wait_for_destroy_queue();
    DestroyWindow(root); obs_shutdown();
    printf("Isolated capture: %s (%d failures): pixels, scene reuse, hung close, heartbeat recovery, audio update, resize, HWND reconnect, hung teardown\n",
           failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
