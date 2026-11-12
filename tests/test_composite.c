/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the real WGC backend and OBS/D3D11 compositor with painted windows. */
#include "../window-size-capture.c"

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static LRESULT CALLBACK paint_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        HBRUSH brush = CreateSolidBrush((COLORREF)GetWindowLongPtrW(window, GWLP_USERDATA));
        FillRect(dc, &paint.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static HWND color_window(HWND owner, int x, int y, int width, int height, COLORREF color)
{
    HWND window = CreateWindowExW(owner ? WS_EX_TOOLWINDOW : 0, L"CPObsCompositeTest", L"Composite test",
                                  WS_POPUP, x, y, width, height, owner, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(window != NULL);
    SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)color);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    return window;
}

static bool pixel_is(uint8_t *pixels, uint32_t stride, int x, int y, int red, int green, int blue)
{
    uint8_t *pixel = pixels + y * stride + x * 4;
    return abs((int)pixel[0] - red) < 20 && abs((int)pixel[1] - green) < 20 &&
           abs((int)pixel[2] - blue) < 20 && pixel[3] > 235;
}

/* Sample inside the main window, in the overlay and beyond the source bounds.
 * Blue is the test background and must survive outside the main-window canvas. */
static bool render_matches(struct size_capture *source, gs_texrender_t *output, gs_stagesurf_t *stage,
                           int overlay_x, int overlay_y, bool overlay_visible, const POINT *caption)
{
    bool matches = false;
    uint8_t *pixels = NULL;
    uint32_t stride = 0;
    obs_enter_graphics();
    gs_texrender_reset(output);
    if (gs_texrender_begin(output, 400, 320)) {
        struct vec4 clear;
        vec4_set(&clear, 0.0f, 0.0f, 1.0f, 1.0f);
        gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
        gs_ortho(0.0f, 400.0f, 0.0f, 320.0f, -100.0f, 100.0f);
        source_render(source, NULL);
        gs_texrender_end(output);
        gs_stage_texture(stage, gs_texrender_get_texture(output));
        if (gs_stagesurface_map(stage, &pixels, &stride)) {
            matches = pixel_is(pixels, stride, 280, 120, 255, 0, 0) &&
                      pixel_is(pixels, stride, overlay_x, overlay_y,
                               overlay_visible ? 0 : 255, overlay_visible ? 255 : 0, 0) &&
                      pixel_is(pixels, stride, 330, 200, 0, 0, 255) &&
                      pixel_is(pixels, stride, 210, 250, 0, 0, 255);
            if (caption) {
                /* Native caption pixels must be present above the green
                 * client area instead of exposing the red main window. */
                matches = matches &&
                          !pixel_is(pixels, stride, caption->x, caption->y, 255, 0, 0) &&
                          !pixel_is(pixels, stride, caption->x, caption->y, 0, 255, 0) &&
                          pixels[caption->y * stride + caption->x * 4 + 3] > 235;
            }
            gs_stagesurface_unmap(stage);
        }
    }
    obs_leave_graphics();
    return matches;
}

static bool await_frame(struct size_capture *source, gs_texrender_t *output, gs_stagesurf_t *stage,
                        int overlay_x, int overlay_y, bool overlay_visible, const POINT *caption)
{
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        collect_process_windows(source);
        update_process_captures(source, 0.03f);
        if (render_matches(source, output, stage, overlay_x, overlay_y, overlay_visible, caption))
            return true;
        Sleep(30);
    }
    return false;
}

int main(void)
{
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CHECK(SUCCEEDED(com));
    if (!obs_startup("en-US", NULL, NULL)) {
        fprintf(stderr, "Unable to initialize libobs\n");
        return 1;
    }
    struct obs_video_info video = {
        .graphics_module = "libobs-d3d11", .fps_num = 30, .fps_den = 1,
        .base_width = 400, .base_height = 320, .output_width = 400, .output_height = 320,
        .output_format = VIDEO_FORMAT_RGBA, .colorspace = VIDEO_CS_709,
        .range = VIDEO_RANGE_FULL, .scale_type = OBS_SCALE_DISABLE,
    };
    int reset_result = obs_reset_video(&video);
    CHECK(reset_result == OBS_VIDEO_SUCCESS);
    if (reset_result != OBS_VIDEO_SUCCESS) {
        obs_shutdown();
        return 1;
    }
    WNDCLASSW window_class = {.lpfnWndProc = paint_window,
                             .hInstance = GetModuleHandleW(NULL),
                             .lpszClassName = L"CPObsCompositeTest"};
    CHECK(RegisterClassW(&window_class) != 0);
    /* Enable physical coordinates just as OBS does, including HiDPI desktops. */
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND root = color_window(NULL, 100, 100, 320, 240, RGB(255, 0, 0));
    HWND overlay = color_window(root, 300, 240, 180, 140, RGB(0, 255, 0));
    struct size_capture source = {.window = root, .pid = GetCurrentProcessId(),
                                 .include_children = true, .client_area = true};
    InitializeCriticalSection(&source.lock);
    bool loaded = load_api(&source.api);
    CHECK(loaded);
    if (loaded) {
        /* OBS supplies a WinRT dispatcher on its video thread. This test
         * captures on the message-pumping main thread and needs its own. */
        void *(*dispatcher_init)(void) = NULL;
        void (*dispatcher_free)(void *) = NULL;
        FARPROC address = GetProcAddress(source.api.module, "winrt_dispatcher_init");
        memcpy(&dispatcher_init, &address, sizeof(dispatcher_init));
        address = GetProcAddress(source.api.module, "winrt_dispatcher_free");
        memcpy(&dispatcher_free, &address, sizeof(dispatcher_free));
        CHECK(dispatcher_init != NULL && dispatcher_free != NULL);
        void *dispatcher = dispatcher_init ? dispatcher_init() : NULL;
        CHECK(dispatcher != NULL);
        obs_enter_graphics();
        gs_texrender_t *output = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        gs_stagesurf_t *stage = gs_stagesurface_create(400, 320, GS_RGBA);
        obs_leave_graphics();
        CHECK(output != NULL && stage != NULL);
        if (output && stage) {
            CHECK(await_frame(&source, output, stage, 210, 150, true, NULL));
            CHECK(source_width(&source) == 320 && source_height(&source) == 240);

            /* Moving both windows together must preserve the rendered layout. */
            SetWindowPos(root, NULL, 160, 140, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(overlay, NULL, 360, 280, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            CHECK(await_frame(&source, output, stage, 210, 150, true, NULL));

            /* Negative offsets clip left/top, without shifting the main image. */
            SetWindowPos(overlay, NULL, 130, 110, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            CHECK(await_frame(&source, output, stage, 10, 10, true, NULL));
            ShowWindow(overlay, SW_HIDE);
            CHECK(await_frame(&source, output, stage, 10, 10, false, NULL));
            CHECK(source_width(&source) == 320 && source_height(&source) == 240);

            /* A captioned floating window retains its title bar while the
             * selected main window still captures only its client area. */
            HWND titled = color_window(root, 200, 175, 180, 140, RGB(0, 255, 0));
            SetWindowLongPtrW(titled, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowTextW(titled, L"Child window title");
            SetWindowPos(titled, HWND_TOP, 200, 175, 180, 140, SWP_FRAMECHANGED | SWP_NOACTIVATE);
            RECT full, client;
            CHECK(process_window_rect(titled, source.pid, false, &full));
            CHECK(process_window_rect(titled, source.pid, true, &client));
            CHECK(full.top < client.top);
            POINT caption = {client.left - source.canvas_rect.left + 12,
                             client.top - source.canvas_rect.top - 5};
            int sample_x = client.left - source.canvas_rect.left + 20;
            int sample_y = client.top - source.canvas_rect.top + 20;
            CHECK(await_frame(&source, output, stage, sample_x, sample_y, true, &caption));
            CHECK(source_width(&source) == 320 && source_height(&source) == 240);
            DestroyWindow(titled);
        }
        release_capture(&source);
        obs_enter_graphics();
        gs_texrender_destroy(source.composite);
        gs_texrender_destroy(output);
        gs_stagesurface_destroy(stage);
        obs_leave_graphics();
        if (dispatcher && dispatcher_free)
            dispatcher_free(dispatcher);
        FreeLibrary(source.api.module);
    }
    DeleteCriticalSection(&source.lock);
    DestroyWindow(overlay);
    DestroyWindow(root);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    obs_shutdown();
    if (SUCCEEDED(com))
        CoUninitialize();
    printf("WGC composite pixel tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
