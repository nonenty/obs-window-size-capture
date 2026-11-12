/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Disposable libobs/WGC backend. All capture callbacks, shared capture lists,
 * COM dispatch and GPU readback belong to this process, never the OBS host. */
#define COBJMACROS
#define CAPTURE_WORKER
#include "window-size-capture.c"
#include <dxgi1_2.h>

__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

struct worker_output {
    struct capture_ipc *ipc;
    struct size_capture *source;
    gs_texrender_t *render;
    gs_stagesurf_t *stage;
    uint32_t width, height;
    uint64_t sequence;
};
#ifdef CAPTURE_TEST_HOOKS
static capture_free_fn original_free;
static struct capture_ipc *test_ipc;
static void test_capture_free(struct winrt_capture *capture)
{
    if (InterlockedCompareExchange(&test_ipc->test_command, 0, 1) == 1) {
        /* Simulate the exact incident: a capture close never returns and a
         * partially written frame is left behind. No host thread may wait. */
        InterlockedExchange(&test_ipc->slots[0].state, CAPTURE_WRITING);
        InterlockedExchange(&test_ipc->test_stage, 1);
        HANDLE never = CreateEventW(NULL, TRUE, FALSE, NULL);
        WaitForSingleObject(never, INFINITE);
    }
    original_free(capture);
}
#endif

static uint32_t preferred_adapter(void)
{
    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) return 0;
    uint32_t best = 0;
    SIZE_T memory = 0;
    IDXGIAdapter1 *adapter;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            desc.DedicatedVideoMemory > memory) {
            memory = desc.DedicatedVideoMemory; best = i;
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    return best;
}

static void export_frame(void *data, float seconds)
{
    UNUSED_PARAMETER(seconds);
    struct worker_output *out = data;
    struct capture_ipc *ipc = out->ipc;
    struct size_capture *self = out->source;
#ifdef CAPTURE_TEST_HOOKS
    if (InterlockedCompareExchange(&ipc->test_command, 0, 2) == 2)
        InterlockedExchange(&ipc->test_stage,
            AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ? 2 : -2);
#endif
    InterlockedExchange64(&ipc->heartbeat, (LONG64)GetTickCount64());
    InterlockedExchange(&ipc->target_pid, (LONG)self->pid);
    InterlockedExchange64(&ipc->target_window, (LONG64)(uintptr_t)self->window);
    InterlockedExchange(&ipc->status, 1);
    if (!InterlockedCompareExchange(&ipc->enabled, 0, 0)) return;
    uint32_t width = source_width(self), height = source_height(self);
    if (!width || !height || width > 16384 || height > 16384) return;
    uint64_t bytes = (uint64_t)width * height * 4;
    if (bytes > ipc->capacity) {
        InterlockedExchange64(&ipc->required_bytes, (LONG64)bytes);
        if (bytes > CAPTURE_MAX_FRAME_BYTES) InterlockedExchange(&ipc->status, -4);
        return;
    }
    /* Slot ownership is atomic, never a cross-process mutex. A dead writer
     * cannot hold up OBS. Reclaim an old complete frame if the reader is slow. */
    int chosen = -1;
    for (unsigned i = 0; i < CAPTURE_IPC_SLOTS; ++i)
        if (InterlockedCompareExchange(&ipc->slots[i].state, CAPTURE_WRITING, CAPTURE_FREE) == CAPTURE_FREE) {
            chosen = (int)i; break;
        }
    if (chosen < 0)
        for (unsigned i = 0; i < CAPTURE_IPC_SLOTS; ++i)
            if (InterlockedCompareExchange(&ipc->slots[i].state, CAPTURE_WRITING, CAPTURE_READY) == CAPTURE_READY) {
                chosen = (int)i; break;
            }
    if (chosen < 0) return;
    bool complete = false;
    obs_enter_graphics();
    if (!out->stage || out->width != width || out->height != height) {
        gs_stagesurface_destroy(out->stage);
        out->stage = gs_stagesurface_create(width, height, GS_BGRA);
        out->width = width; out->height = height;
    }
    if (out->stage) {
        gs_texrender_reset(out->render);
        if (gs_texrender_begin(out->render, width, height)) {
            struct vec4 clear;
            vec4_zero(&clear);
            gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
            gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
            source_render(self, NULL);
            gs_texrender_end(out->render);
            gs_stage_texture(out->stage, gs_texrender_get_texture(out->render));
            uint8_t *pixels = NULL;
            uint32_t stride = 0;
            if (gs_stagesurface_map(out->stage, &pixels, &stride)) {
                uint8_t *destination = capture_slot_pixels(ipc, (unsigned)chosen);
                for (uint32_t y = 0; y < height; ++y)
                    memcpy(destination + (size_t)y * width * 4, pixels + (size_t)y * stride, (size_t)width * 4);
                gs_stagesurface_unmap(out->stage);
                struct capture_slot *slot = &ipc->slots[chosen];
                slot->width = width; slot->height = height; slot->stride = width * 4;
                slot->sequence = ++out->sequence;
                complete = true;
            }
        }
    }
    obs_leave_graphics();
    InterlockedExchange(&ipc->slots[chosen].state, complete ? CAPTURE_READY : CAPTURE_FREE);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 4) return 2;
    HANDLE mapping = (HANDLE)(uintptr_t)wcstoull(argv[1], NULL, 10);
    HANDLE stop = (HANDLE)(uintptr_t)wcstoull(argv[2], NULL, 10);
    size_t size = (size_t)wcstoull(argv[3], NULL, 10);
    if (size < sizeof(struct capture_ipc) || size > capture_mapping_size(CAPTURE_MAX_FRAME_BYTES)) return 3;
    struct capture_ipc *ipc = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!ipc) return 4;
    if (ipc->version != CAPTURE_IPC_VERSION || !ipc->capacity || ipc->capacity > CAPTURE_MAX_FRAME_BYTES ||
        capture_mapping_size(ipc->capacity) != size) return 5;
    /* Set the process default before libobs creates its video thread. A main
     * thread override alone leaves window matching/compositing DPI virtualized. */
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                     DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        InterlockedExchange(&ipc->status, -5);
        return 10;
    }
    if (!obs_startup("en-US", NULL, NULL)) return 6;
    struct obs_audio_info audio = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
    obs_reset_audio(&audio);
    /* Only the private offscreen renderer exports pixels. The libobs output
     * canvas stays tiny; window resizing does not reset the video subsystem. */
    struct obs_video_info video = {.graphics_module = "libobs-d3d11", .fps_num = 30, .fps_den = 1,
        .base_width = 16, .base_height = 16, .output_width = 16, .output_height = 16,
        .output_format = VIDEO_FORMAT_RGBA, .colorspace = VIDEO_CS_709, .range = VIDEO_RANGE_FULL,
        .scale_type = OBS_SCALE_DISABLE, .adapter = preferred_adapter()};
    if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) { InterlockedExchange(&ipc->status, -1); return 7; }
    obs_module_load();
    struct capture_config config = ipc->config;
    char exe[CAPTURE_EXE_CHARS * 4] = {0};
    WideCharToMultiByte(CP_UTF8, 0, config.exe, -1, exe, sizeof(exe), NULL, NULL);
    obs_data_t *settings = obs_data_create();
    source_defaults(settings);
    obs_data_set_string(settings, "exe", exe);
    obs_data_set_int(settings, "width", config.width);
    obs_data_set_int(settings, "height", config.height);
    obs_data_set_int(settings, "tolerance", config.tolerance);
    obs_data_set_bool(settings, "client_area", config.client_area);
    obs_data_set_bool(settings, "cursor", config.cursor);
    obs_data_set_bool(settings, "include_children", config.include_children);
    obs_data_set_bool(settings, "capture_audio", false); /* Audio stays in OBS. */
    obs_source_t *source = obs_source_create_private("window_size_capture", "Isolated capture", settings);
    obs_data_release(settings);
    if (!source) { InterlockedExchange(&ipc->status, -2); return 8; }
    struct worker_output out = {.ipc = ipc, .source = obs_obj_get_data(source)};
    if (config.include_children && config.resume_window && config.resume_pid) {
        HWND anchor = (HWND)(uintptr_t)config.resume_window;
        DWORD pid = 0;
        GetWindowThreadProcessId(anchor, &pid);
        HANDLE process = pid == config.resume_pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
        if (process) {
            wchar_t path[32768]; DWORD length = sizeof(path) / sizeof(*path);
            if (QueryFullProcessImageNameW(process, 0, path, &length)) {
                const wchar_t *filename = wcsrchr(path, L'\\');
                if (_wcsicmp(filename ? filename + 1 : path, config.exe) == 0) {
                    EnterCriticalSection(&out.source->lock);
                    out.source->window = anchor; out.source->pid = pid;
                    out.source->dirty = false;
                    LeaveCriticalSection(&out.source->lock);
                }
            }
            CloseHandle(process);
        }
    }
#ifdef CAPTURE_TEST_HOOKS
    test_ipc = ipc;
    EnterCriticalSection(&out.source->lock);
    original_free = out.source->api.free;
    out.source->api.free = test_capture_free;
    LeaveCriticalSection(&out.source->lock);
#endif
    obs_enter_graphics();
    out.render = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    obs_leave_graphics();
    if (!out.render) { InterlockedExchange(&ipc->status, -3); return 9; }
    obs_source_inc_showing(source);
    obs_add_tick_callback(export_frame, &out);
    WaitForSingleObject(stop, INFINITE);
    obs_remove_tick_callback(export_frame, &out);
    obs_source_dec_showing(source);
    obs_source_release(source);
    obs_wait_for_destroy_queue();
    obs_enter_graphics();
    gs_stagesurface_destroy(out.stage);
    gs_texrender_destroy(out.render);
    obs_leave_graphics();
    obs_shutdown();
    UnmapViewOfFile(ipc); CloseHandle(mapping); CloseHandle(stop);
    return 0;
}
