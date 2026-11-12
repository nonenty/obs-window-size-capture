/* Window Size Capture for OBS Studio.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <wchar.h>
#include <wctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <obs-module.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#include <callback/signal.h>
#include <graphics/vec4.h>
#include "process-audio.hpp"
#include "capture-host.h"
#include "plugin-version.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("window-size-capture", "en-US")

const char *obs_module_name(void)
{
    return "Window Size Capture";
}

const char *obs_module_description(void)
{
    return "Window Size Capture " WINDOW_SIZE_CAPTURE_VERSION ": match windows by executable and size.";
}

struct winrt_capture;
typedef BOOL (*capture_supported_fn)(void);
typedef struct winrt_capture *(*capture_init_fn)(BOOL, HWND, BOOL, BOOL);
typedef void (*capture_free_fn)(struct winrt_capture *);
typedef BOOL (*capture_active_fn)(const struct winrt_capture *);
typedef void (*capture_render_fn)(struct winrt_capture *);
typedef uint32_t (*capture_size_fn)(const struct winrt_capture *);
typedef enum gs_color_space (*capture_color_fn)(const struct winrt_capture *);

struct capture_api {
    HMODULE module;
    capture_supported_fn supported;
    capture_init_fn init;
    capture_free_fn free;
    capture_active_fn active;
    capture_render_fn render;
    capture_size_fn width;
    capture_size_fn height;
    capture_color_fn color_space;
};

struct process_window {
    HWND window;
    RECT rect;
    struct winrt_capture *capture;
    float retry_elapsed;
    struct process_window *next;
};

struct size_capture {
    obs_source_t *source;
    struct pid_audio_capture *audio_capture;
    struct capture_host *host;
    CRITICAL_SECTION lock;
    wchar_t *exe;
    int target_width;
    int target_height;
    int tolerance;
    bool client_area;
    bool capture_cursor;
    bool capture_audio;
    bool include_children;
    bool dirty;
    float scan_elapsed;
    float audio_retry_elapsed;
    HWND window;
    DWORD pid;
    struct capture_api api;
    struct winrt_capture *capture;
    struct process_window *process_windows;
    RECT canvas_rect;
    gs_texrender_t *composite;
    DWORD audio_pid;
};

static char *utf8_copy(const wchar_t *value);

static wchar_t *wide_copy(const char *value)
{
    if (!value || !*value)
        return NULL;
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (length <= 0)
        return NULL;
    wchar_t *result = calloc((size_t)length, sizeof(wchar_t));
    if (result && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result, length)) {
        free(result);
        result = NULL;
    }
    return result;
}

static const char *source_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("SourceName");
}

static void free_process_windows(struct size_capture *self, struct process_window *windows)
{
    while (windows) {
        struct process_window *next = windows->next;
        if (windows->capture)
            self->api.free(windows->capture);
        free(windows);
        windows = next;
    }
}

static void release_process_windows(struct size_capture *self)
{
    free_process_windows(self, self->process_windows);
    self->process_windows = NULL;
}

static void release_capture(struct size_capture *self)
{
    release_process_windows(self);
    if (self->capture) {
        self->api.free(self->capture);
        self->capture = NULL;
    }
    self->window = NULL;
    self->pid = 0;
}

static void disable_audio(struct size_capture *self)
{
    struct pid_audio_capture *capture = self->audio_capture;
    self->audio_capture = NULL;
    self->audio_pid = 0;
    /* Stop only schedules retirement; it never joins on the video thread or
     * while holding this source's lock. */
    pid_audio_stop(capture);
}

static bool configure_audio(struct size_capture *self, DWORD pid)
{
    if (!self->capture_audio || !pid) {
        disable_audio(self);
        return true;
    }
    if (self->audio_capture && self->audio_pid == pid && !pid_audio_finished(self->audio_capture))
        return true;
    if (self->audio_capture && pid_audio_finished(self->audio_capture) && self->audio_retry_elapsed < 3.0f)
        return true;
    if (self->audio_capture && pid_audio_finished(self->audio_capture))
        blog(LOG_WARNING, "[window-size-capture: %s] process loopback stopped; retrying PID %lu",
             obs_source_get_name(self->source), (unsigned long)pid);
    disable_audio(self);
    self->audio_capture = pid_audio_start(self->source, pid);
    if (!self->audio_capture) {
        blog(LOG_WARNING, "[window-size-capture: %s] failed to create application audio capture",
             obs_source_get_name(self->source));
        return false;
    }
    self->audio_pid = pid;
    self->audio_retry_elapsed = 0.0f;
    return true;
}

static bool __attribute__((unused)) load_api(struct capture_api *api)
{
    api->module = LoadLibraryW(L"libobs-winrt.dll");
    if (!api->module)
        return false;
#define LOAD(field, export_name) do { \
    FARPROC address = GetProcAddress(api->module, export_name); \
    memcpy(&api->field, &address, sizeof(api->field)); \
} while (0)
    LOAD(supported, "winrt_capture_supported");
    LOAD(init, "winrt_capture_init_window");
    LOAD(free, "winrt_capture_free");
    LOAD(active, "winrt_capture_active");
    LOAD(render, "winrt_capture_render");
    LOAD(width, "winrt_capture_width");
    LOAD(height, "winrt_capture_height");
    LOAD(color_space, "winrt_capture_get_color_space");
#undef LOAD
    if (api->supported && api->init && api->free && api->active && api->render && api->width && api->height &&
        api->color_space && api->supported())
        return true;
    FreeLibrary(api->module);
    ZeroMemory(api, sizeof(*api));
    return false;
}

static void read_settings(struct size_capture *self, obs_data_t *settings)
{
    wchar_t *exe = wide_copy(obs_data_get_string(settings, "exe"));
    EnterCriticalSection(&self->lock);
    free(self->exe);
    self->exe = exe;
    int width = (int)obs_data_get_int(settings, "width");
    int height = (int)obs_data_get_int(settings, "height");
    int tolerance = (int)obs_data_get_int(settings, "tolerance");
    self->target_width = width >= 0 && width <= 16384 ? width : 0;
    self->target_height = height >= 0 && height <= 16384 ? height : 0;
    self->tolerance = tolerance >= 0 && tolerance <= 1000 ? tolerance : 0;
    self->client_area = obs_data_get_bool(settings, "client_area");
    self->capture_cursor = obs_data_get_bool(settings, "cursor");
    self->capture_audio = obs_data_get_bool(settings, "capture_audio");
    self->include_children = obs_data_get_bool(settings, "include_children");
    self->canvas_rect = (RECT){.right = self->target_width, .bottom = self->target_height};
    self->dirty = true;
    if (self->host) {
        struct capture_config config = {
            .width = (uint32_t)self->target_width, .height = (uint32_t)self->target_height,
            .tolerance = (uint32_t)self->tolerance, .client_area = self->client_area,
            .cursor = self->capture_cursor, .include_children = self->include_children,
        };
        if (exe) wcsncpy(config.exe, exe, CAPTURE_EXE_CHARS - 1);
        capture_host_configure(self->host, &config);
    }
    LeaveCriticalSection(&self->lock);
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
    struct size_capture *self = calloc(1, sizeof(*self));
    if (!self)
        return NULL;
    self->source = source;
    InitializeCriticalSection(&self->lock);
#ifdef CAPTURE_WORKER
    if (!load_api(&self->api)) {
#else
    self->host = capture_host_create(obs_source_get_name(source));
    if (!self->host) {
#endif
        blog(LOG_ERROR, "[window-size-capture] capture backend initialization failed");
        DeleteCriticalSection(&self->lock);
        free(self);
        return NULL;
    }
    read_settings(self, settings);
    return self;
}

static void source_actual_destroy(void *data)
{
    struct size_capture *self = data;
    if (!self)
        return;
    /* In OBS this only retires the external backend and its uploaded texture.
     * In the worker build, WGC teardown stays on that process's owning video/
     * dispatcher thread. A stuck teardown is handled by the parent's job. */
    disable_audio(self);
    if (self->host) capture_host_destroy(self->host);
    else release_capture(self);
    free(self->exe);
    obs_enter_graphics();
    gs_texrender_destroy(self->composite);
    obs_leave_graphics();
    DeleteCriticalSection(&self->lock);
    if (self->api.module) FreeLibrary(self->api.module);
    free(self);
}

static void source_destroy(void *data)
{
    struct size_capture *self = data;
    if (!self)
        return;
    pid_audio_request_stop(self->audio_capture);
    obs_queue_task(OBS_TASK_GRAPHICS, source_actual_destroy, self, false);
}

static void source_update(void *data, obs_data_t *settings)
{
    struct size_capture *self = data;
    read_settings(self, settings);

    /* Stop emitting audio from the previous PID as soon as OBS applies new
     * settings. The next video tick releases the old capture and resolves the
     * new HWND/PID from the updated executable and dimensions. */
    EnterCriticalSection(&self->lock);
    disable_audio(self);
    self->audio_retry_elapsed = 0.0f;
    blog(LOG_INFO, "[window-size-capture: %s] settings updated; audio will follow the next window match",
         obs_source_get_name(self->source));
    LeaveCriticalSection(&self->lock);
}

static void source_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "window_pick", "");
    obs_data_set_default_int(settings, "width", 0);
    obs_data_set_default_int(settings, "height", 0);
    obs_data_set_default_int(settings, "tolerance", 4);
    obs_data_set_default_bool(settings, "client_area", true);
    obs_data_set_default_bool(settings, "cursor", false);
    obs_data_set_default_bool(settings, "capture_audio", false);
    obs_data_set_default_bool(settings, "include_children", false);
}

static bool get_window_size(HWND window, bool client_area, int *width, int *height)
{
    RECT rect;
    if (!(client_area ? GetClientRect(window, &rect) : GetWindowRect(window, &rect)))
        return false;
    *width = rect.right - rect.left;
    *height = rect.bottom - rect.top;
    return *width > 0 && *height > 0;
}

struct window_info {
    HWND window;
    DWORD pid;
    int width;
    int height;
    wchar_t title[1024];
    wchar_t class_name[1024];
    wchar_t exe[512];
};

static char *utf8_copy(const wchar_t *value)
{
    int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);
    if (length <= 0)
        return NULL;
    char *result = malloc((size_t)length);
    if (result && !WideCharToMultiByte(CP_UTF8, 0, value, -1, result, length, NULL, NULL)) {
        free(result);
        result = NULL;
    }
    return result;
}

static bool inspect_window(HWND window, bool client_area, bool include_children, struct window_info *info)
{
    if (!IsWindowVisible(window) || IsIconic(window))
        return false;
    /* Floating/owned windows are top-level HWNDs and are already returned by
     * EnumWindows. Do not enumerate WS_CHILD controls: WGC captures top-level
     * windows, and those controls include buttons and embedded panels. */
    if (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD)
        return false;
    if (!include_children && (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        return false;
    wchar_t window_class[64];
    if (GetClassNameW(window, window_class, (int)(sizeof(window_class) / sizeof(window_class[0]))) &&
        (wcscmp(window_class, L"Progman") == 0 || wcscmp(window_class, L"WorkerW") == 0))
        return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;
    info->title[0] = L'\0';
    GetWindowTextW(window, info->title, (int)(sizeof(info->title) / sizeof(info->title[0])));
    if (!GetClassNameW(window, info->class_name,
                       (int)(sizeof(info->class_name) / sizeof(info->class_name[0]))))
        info->class_name[0] = L'\0';
    if (!get_window_size(window, client_area, &info->width, &info->height))
        return false;
    if (info->width < 32 || info->height < 32)
        return false;
    info->window = window;
    info->exe[0] = L'\0';
    GetWindowThreadProcessId(window, &info->pid);
    if (!info->pid)
        return false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, info->pid);
    if (process) {
        wchar_t path[32768];
        DWORD path_size = (DWORD)(sizeof(path) / sizeof(path[0]));
        if (QueryFullProcessImageNameW(process, 0, path, &path_size)) {
            const wchar_t *filename = wcsrchr(path, L'\\');
            wcsncpy(info->exe, filename ? filename + 1 : path,
                    (sizeof(info->exe) / sizeof(info->exe[0])) - 1);
            info->exe[(sizeof(info->exe) / sizeof(info->exe[0])) - 1] = L'\0';
        }
        CloseHandle(process);
    }
    /* A title is only a display label; selection requires a known executable. */
    return *info->exe != L'\0';
}

static uint64_t hash_text(uint64_t hash, const wchar_t *text)
{
    while (*text) {
        hash ^= (uint16_t)towlower(*text++);
        hash *= UINT64_C(1099511628211);
    }
    hash ^= 0xFF;
    return hash * UINT64_C(1099511628211);
}

static void window_key(const struct window_info *info, char key[17])
{
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = hash_text(hash, info->exe);
    hash ^= (uint32_t)info->width;
    hash *= UINT64_C(1099511628211);
    hash ^= (uint32_t)info->height;
    hash *= UINT64_C(1099511628211);
    /* Titles (and native classes) can change during an application session. Keep
     * the selection stable using the same executable/size identity as capture. */
    snprintf(key, 17, "%016llX", (unsigned long long)hash);
}

struct list_context {
    obs_property_t *list;
    bool client_area;
    bool include_children;
};

static BOOL CALLBACK list_windows(HWND window, LPARAM param)
{
    struct list_context *context = (struct list_context *)param;
    struct window_info info = {0};
    if (!inspect_window(window, context->client_area, context->include_children, &info))
        return TRUE;
    char key[17];
    window_key(&info, key);
    for (size_t i = 0; i < obs_property_list_item_count(context->list); ++i) {
        const char *existing = obs_property_list_item_string(context->list, i);
        if (existing && strcmp(existing, key) == 0)
            return TRUE;
    }
    char *title = utf8_copy(*info.title ? info.title : info.class_name);
    char *exe = utf8_copy(info.exe);
    if (title && exe) {
        const char *kind = (!*info.title || GetWindow(window, GW_OWNER) ||
                            (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
                               ? obs_module_text("FloatingWindow") : "";
        size_t length = strlen(title) + strlen(exe) + strlen(kind) + 96;
        char *label = malloc(length);
        if (label) {
            snprintf(label, length, "%s%s [%s]  %d x %d  (PID %lu)", kind, title, exe,
                     info.width, info.height, (unsigned long)info.pid);
            obs_property_list_add_string(context->list, label, key);
            free(label);
        }
    }
    free(title);
    free(exe);
    return TRUE;
}

static void populate_window_list(obs_properties_t *props, obs_data_t *settings)
{
    obs_property_t *list = obs_properties_get(props, "window_pick");
    obs_property_list_clear(list);
    obs_property_list_add_string(list, obs_module_text("ChooseWindow"), "");
    struct list_context context = {
        .list = list, .client_area = obs_data_get_bool(settings, "client_area"),
        .include_children = obs_data_get_bool(settings, "include_children"),
    };
    EnumWindows(list_windows, (LPARAM)&context);
    /* Older scenes saved a title-dependent list key. Rebuild it from their
     * existing executable and dimensions, even while the target is closed. */
    if (*obs_data_get_string(settings, "window_pick")) {
        struct window_info saved = {0};
        wchar_t *exe = wide_copy(obs_data_get_string(settings, "exe"));
        saved.width = (int)obs_data_get_int(settings, "width");
        saved.height = (int)obs_data_get_int(settings, "height");
        if (exe && saved.width > 0 && saved.height > 0) {
            wcsncpy(saved.exe, exe, (sizeof(saved.exe) / sizeof(saved.exe[0])) - 1);
            char key[17];
            window_key(&saved, key);
            obs_data_set_string(settings, "window_pick", key);
        }
        free(exe);
    }
    const char *selected = obs_data_get_string(settings, "window_pick");
    if (selected && *selected) {
        bool found = false;
        for (size_t i = 0; i < obs_property_list_item_count(list); ++i) {
            const char *value = obs_property_list_item_string(list, i);
            if (value && strcmp(value, selected) == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            obs_property_list_add_string(list, obs_module_text("SavedWindowMissing"), selected);
    }
}

struct select_context {
    const char *key;
    bool client_area;
    bool include_children;
    struct window_info info;
    bool found;
};

static BOOL CALLBACK select_window(HWND window, LPARAM param)
{
    struct select_context *context = (struct select_context *)param;
    struct window_info info = {0};
    if (!inspect_window(window, context->client_area, context->include_children, &info))
        return TRUE;
    char key[17];
    window_key(&info, key);
    if (strcmp(key, context->key) != 0)
        return TRUE;
    context->info = info;
    context->found = true;
    return FALSE;
}

static bool window_selected(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(property);
    const char *key = obs_data_get_string(settings, "window_pick");
    if (!key || !*key)
        return false;
    struct select_context context = {
        .key = key, .client_area = obs_data_get_bool(settings, "client_area"),
        .include_children = obs_data_get_bool(settings, "include_children"),
    };
    EnumWindows(select_window, (LPARAM)&context);
    if (!context.found)
        return false;
    char *title = utf8_copy(context.info.title);
    char *exe = utf8_copy(context.info.exe);
    if (!title || !exe) {
        free(title);
        free(exe);
        return false;
    }
    obs_data_set_string(settings, "title", title);
    obs_data_set_string(settings, "exe", exe);
    obs_data_set_int(settings, "width", context.info.width);
    obs_data_set_int(settings, "height", context.info.height);
    /* Retain the title as scene metadata only, never as a matching condition. */
    obs_data_unset_user_value(settings, "window_class");
    free(title);
    free(exe);
    return true;
}

static bool refresh_windows(obs_properties_t *props, obs_property_t *property, void *unused)
{
    UNUSED_PARAMETER(property);
    UNUSED_PARAMETER(unused);
    struct size_capture *self = obs_properties_get_param(props);
    obs_data_t *settings = self ? obs_source_get_settings(self->source) : obs_data_create();
    source_defaults(settings);
    populate_window_list(props, settings);
    obs_data_release(settings);
    return true;
}

static bool window_options_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
    UNUSED_PARAMETER(property);
    populate_window_list(props, settings);
    return true;
}

static obs_properties_t *source_properties(void *data)
{
    struct size_capture *self = data;
    obs_properties_t *props = obs_properties_create();
    obs_properties_set_param(props, self, NULL);
    obs_property_t *children = obs_properties_add_bool(props, "include_children", obs_module_text("IncludeChildren"));
    obs_property_set_long_description(children, obs_module_text("IncludeChildren.TT"));
    obs_property_set_modified_callback(children, window_options_changed);
    obs_property_t *list = obs_properties_add_list(props, "window_pick", obs_module_text("Window"),
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_set_modified_callback(list, window_selected);
    obs_properties_add_button(props, "refresh", obs_module_text("Refresh"), refresh_windows);
    obs_properties_add_text(props, "exe", obs_module_text("Executable"), OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, "width", obs_module_text("Width"), 0, 16384, 1);
    obs_properties_add_int(props, "height", obs_module_text("Height"), 0, 16384, 1);
    obs_properties_add_int(props, "tolerance", obs_module_text("Tolerance"), 0, 1000, 1);
    obs_property_t *client = obs_properties_add_bool(props, "client_area", obs_module_text("ClientArea"));
    obs_property_set_long_description(client, obs_module_text("ClientArea.TT"));
    obs_property_set_modified_callback(client, window_options_changed);
    obs_properties_add_bool(props, "cursor", obs_module_text("Cursor"));
    obs_property_t *audio = obs_properties_add_bool(props, "capture_audio", obs_module_text("CaptureAudio"));
    obs_property_set_long_description(audio, obs_module_text("CaptureAudio.TT"));
    obs_properties_add_text(props, "hint", obs_module_text("Hint"), OBS_TEXT_INFO);
    refresh_windows(props, NULL, NULL);
    return props;
}

static bool matches_window(struct size_capture *self, HWND window, DWORD *pid_out, int *score_out)
{
    if (!self->exe)
        return false;
    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if ((style & WS_CHILD) || (!self->include_children &&
                             (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)))
        return false;
    if (!IsWindowVisible(window) || IsIconic(window))
        return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;

    int width, height;
    if (!get_window_size(window, self->client_area, &width, &height))
        return false;
    if (abs(width - self->target_width) > self->tolerance ||
        abs(height - self->target_height) > self->tolerance)
        return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid)
        return false;
    if (self->exe) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process)
            return false;
        wchar_t path[32768];
        DWORD path_size = (DWORD)(sizeof(path) / sizeof(path[0]));
        BOOL ok = QueryFullProcessImageNameW(process, 0, path, &path_size);
        CloseHandle(process);
        if (!ok)
            return false;
        const wchar_t *filename = wcsrchr(path, L'\\');
        if (_wcsicmp(filename ? filename + 1 : path, self->exe) != 0)
            return false;
    }
    *pid_out = pid;
    *score_out = abs(width - self->target_width) + abs(height - self->target_height);
    return true;
}

struct window_search {
    struct size_capture *self;
    HWND best;
    DWORD pid;
    int score;
};

static BOOL CALLBACK find_window(HWND window, LPARAM param)
{
    struct window_search *search = (struct window_search *)param;
    DWORD pid;
    int score;
    if (matches_window(search->self, window, &pid, &score) &&
        (score < search->score || (score == search->score && pid < search->pid))) {
        search->best = window;
        search->pid = pid;
        search->score = score;
    }
    return TRUE;
}

static bool process_window_rect(HWND window, DWORD process_id, bool client_area, RECT *rect)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!process_id || pid != process_id || !IsWindowVisible(window) || IsIconic(window) ||
        (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD))
        return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;
    if (client_area) {
        POINT origin = {0};
        if (!GetClientRect(window, rect) || !ClientToScreen(window, &origin))
            return false;
        OffsetRect(rect, origin.x, origin.y);
    } else if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, rect, sizeof(*rect))) &&
               !GetWindowRect(window, rect)) {
        return false;
    }
    return rect->right > rect->left && rect->bottom > rect->top;
}

static bool process_capture_client_area(const struct size_capture *self, HWND window)
{
    /* Only the selected main window follows the client-area setting. Floating
     * windows retain their native title bar and frame at desktop coordinates. */
    return window == self->window && self->client_area;
}

struct process_collection {
    struct size_capture *self;
    struct process_window *remaining;
};

static BOOL CALLBACK collect_process_window(HWND window, LPARAM param)
{
    struct process_collection *context = (struct process_collection *)param;
    struct size_capture *self = context->self;
    RECT rect, visible;
    if (!process_window_rect(window, self->pid, process_capture_client_area(self, window), &rect) ||
        !IntersectRect(&visible, &rect, &self->canvas_rect))
        return TRUE;

    struct process_window **previous = &context->remaining;
    while (*previous && (*previous)->window != window)
        previous = &(*previous)->next;
    struct process_window *item = *previous;
    if (item) {
        *previous = item->next;
    } else {
        item = calloc(1, sizeof(*item));
        if (!item)
            return TRUE;
        item->window = window;
        item->retry_elapsed = 1.0f;
    }
    item->rect = rect;
    /* EnumWindows visits front to back. Prepending produces paint order. */
    item->next = self->process_windows;
    self->process_windows = item;
    return TRUE;
}

static void collect_process_windows(struct size_capture *self)
{
    struct process_collection context = {.self = self, .remaining = self->process_windows};
    self->process_windows = NULL;
    RECT canvas;
    if (!process_window_rect(self->window, self->pid, self->client_area, &canvas)) {
        free_process_windows(self, context.remaining);
        return;
    }
    self->canvas_rect = canvas;
    EnumWindows(collect_process_window, (LPARAM)&context);
    free_process_windows(self, context.remaining);
}

static void update_process_captures(struct size_capture *self, float seconds)
{
    for (struct process_window *item = self->process_windows; item; item = item->next) {
        item->retry_elapsed += seconds;
        if (item->capture && !self->api.active(item->capture)) {
            self->api.free(item->capture);
            item->capture = NULL;
            item->retry_elapsed = 0.0f;
        }
        if (!item->capture && item->retry_elapsed >= 1.0f) {
            item->retry_elapsed = 0.0f;
            obs_enter_graphics();
            item->capture = self->api.init(self->capture_cursor, item->window,
                                            process_capture_client_area(self, item->window), FALSE);
            obs_leave_graphics();
            if (!item->capture)
                blog(LOG_WARNING, "[window-size-capture: %s] process window capture failed for HWND %p",
                     obs_source_get_name(self->source), item->window);
        }
    }
}

static bool anchor_valid(struct size_capture *self)
{
    DWORD pid = 0;
    int score;
    if (!IsWindow(self->window))
        return false;
    if (self->include_children) {
        /* Once a process is selected, resizing or retitling its main window
         * must not interrupt the other windows. Size is used to reacquire it. */
        GetWindowThreadProcessId(self->window, &pid);
        return pid != 0 && pid == self->pid;
    }
    return matches_window(self, self->window, &pid, &score) && pid == self->pid;
}

static void source_tick(void *data, float seconds)
{
    struct size_capture *self = data;
    EnterCriticalSection(&self->lock);
    if (self->host) {
        if (self->dirty) {
            capture_host_clear(self->host);
            self->dirty = false;
        }
        bool showing = obs_source_showing(self->source);
        capture_host_tick(self->host, showing);
        self->audio_retry_elapsed += seconds;
        DWORD pid = showing ? capture_host_target_pid(self->host) : 0;
        if (pid && self->capture_audio) configure_audio(self, pid);
        else disable_audio(self);
        LeaveCriticalSection(&self->lock);
        return;
    }
    if (self->dirty) {
        disable_audio(self);
        release_capture(self);
        self->scan_elapsed = 1.0f;
        self->dirty = false;
    }
    if (!self->capture_audio)
        disable_audio(self);
    if (!obs_source_showing(self->source)) {
        disable_audio(self);
        release_process_windows(self);
        LeaveCriticalSection(&self->lock);
        return;
    }
    self->audio_retry_elapsed += seconds;
    self->scan_elapsed += seconds;
    if (self->window && self->scan_elapsed >= 0.5f) {
        self->scan_elapsed = 0.0f;
        if (!anchor_valid(self)) {
            blog(LOG_INFO, "[window-size-capture: %s] target lost", obs_source_get_name(self->source));
            disable_audio(self);
            release_capture(self);
            self->scan_elapsed = 1.0f;
        }
    }
    if (!self->window && self->scan_elapsed >= 1.0f && self->target_width > 0 && self->target_height > 0) {
        struct window_search search = {.self = self, .score = INT32_MAX, .pid = UINT32_MAX};
        self->scan_elapsed = 0.0f;
        EnumWindows(find_window, (LPARAM)&search);
        if (search.best) {
            self->window = search.best;
            self->pid = search.pid;
            blog(LOG_INFO, "[window-size-capture: %s] matched PID %lu, HWND %p",
                 obs_source_get_name(self->source), (unsigned long)self->pid, self->window);
        }
    }
    if (self->include_children) {
        if (self->window) {
            collect_process_windows(self);
            update_process_captures(self, seconds);
        }
        if (self->process_windows && self->capture_audio)
            configure_audio(self, self->pid);
        else
            disable_audio(self);
    } else if (self->window && !self->capture) {
        obs_enter_graphics();
        self->capture = self->api.init(self->capture_cursor, self->window, self->client_area, FALSE);
        obs_leave_graphics();
        if (!self->capture) {
            blog(LOG_WARNING, "[window-size-capture: %s] capture initialization failed",
                 obs_source_get_name(self->source));
            release_capture(self);
        }
    }
    if (!self->include_children && self->window && self->capture_audio)
        configure_audio(self, self->pid);
    LeaveCriticalSection(&self->lock);
}

static uint32_t source_width(void *data)
{
    struct size_capture *self = data;
    EnterCriticalSection(&self->lock);
    uint32_t width = self->host ? capture_host_width(self->host) : self->include_children ? (uint32_t)(self->canvas_rect.right - self->canvas_rect.left)
                     : self->capture ? self->api.width(self->capture) : (uint32_t)self->target_width;
    if (!width && self->host) width = (uint32_t)self->target_width;
    LeaveCriticalSection(&self->lock);
    return width;
}

static uint32_t source_height(void *data)
{
    struct size_capture *self = data;
    EnterCriticalSection(&self->lock);
    uint32_t height = self->host ? capture_host_height(self->host) : self->include_children ? (uint32_t)(self->canvas_rect.bottom - self->canvas_rect.top)
                      : self->capture ? self->api.height(self->capture) : (uint32_t)self->target_height;
    if (!height && self->host) height = (uint32_t)self->target_height;
    LeaveCriticalSection(&self->lock);
    return height;
}

static void render_process_windows(struct size_capture *self)
{
    const uint32_t width = (uint32_t)(self->canvas_rect.right - self->canvas_rect.left);
    const uint32_t height = (uint32_t)(self->canvas_rect.bottom - self->canvas_rect.top);
    if (!width || !height || !self->process_windows)
        return;
    enum gs_color_space space = gs_get_color_space();
    enum gs_color_format format = gs_get_format_from_space(space);
    if (self->composite && gs_texrender_get_format(self->composite) != format) {
        gs_texrender_destroy(self->composite);
        self->composite = NULL;
    }
    if (!self->composite)
        self->composite = gs_texrender_create(format, GS_ZS_NONE);
    gs_texrender_reset(self->composite);
    if (!gs_texrender_begin_with_color_space(self->composite, width, height, space))
        return;
    struct vec4 clear;
    vec4_zero(&clear);
    gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
    gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
    for (struct process_window *item = self->process_windows; item; item = item->next) {
        RECT rect;
        if (!item->capture || !self->api.active(item->capture) ||
            !process_window_rect(item->window, self->pid,
                                 process_capture_client_area(self, item->window), &rect))
            continue;
        uint32_t capture_width = self->api.width(item->capture);
        uint32_t capture_height = self->api.height(item->capture);
        if (!capture_width || !capture_height)
            continue;
        gs_matrix_push();
        gs_matrix_translate3f((float)(rect.left - self->canvas_rect.left),
                              (float)(rect.top - self->canvas_rect.top), 0.0f);
        gs_matrix_scale3f((float)(rect.right - rect.left) / capture_width,
                          (float)(rect.bottom - rect.top) / capture_height, 1.0f);
        self->api.render(item->capture);
        gs_matrix_pop();
    }
    gs_texrender_end(self->composite);
    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    const bool previous_srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    gs_texture_t *texture = gs_texrender_get_texture(self->composite);
    gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), texture);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(texture, 0, width, height);
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(previous_srgb);
}

static void source_render(void *data, gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);
    struct size_capture *self = data;
    EnterCriticalSection(&self->lock);
    if (self->host) {
        capture_host_render(self->host);
    } else if (self->include_children) {
        render_process_windows(self);
    } else if (self->capture) {
        if (self->api.active(self->capture))
            self->api.render(self->capture);
        else
            self->dirty = true; /* Retire on the next tick, outside rendering. */
    }
    LeaveCriticalSection(&self->lock);
}

static enum gs_color_space source_color_space(void *data, size_t count, const enum gs_color_space *preferred)
{
    struct size_capture *self = data;
    EnterCriticalSection(&self->lock);
    enum gs_color_space actual = self->capture ? self->api.color_space(self->capture) : GS_CS_SRGB;
    if (self->include_children) {
        for (struct process_window *item = self->process_windows; item; item = item->next) {
            if (item->capture && self->api.color_space(item->capture) == GS_CS_709_EXTENDED) {
                actual = GS_CS_709_EXTENDED;
                break;
            }
        }
    }
    LeaveCriticalSection(&self->lock);
    enum gs_color_space choice = actual;
    for (size_t i = 0; i < count; ++i) {
        choice = preferred[i];
        if (choice == actual)
            break;
    }
    return choice;
}

bool obs_module_load(void)
{
#ifndef CAPTURE_WORKER
    blog(LOG_INFO, "[window-size-capture] version %s", WINDOW_SIZE_CAPTURE_VERSION);
#endif
    static struct obs_source_info info = {
        .id = "window_size_capture",
        .type = OBS_SOURCE_TYPE_INPUT,
        .output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
        .get_name = source_name,
        .create = source_create,
        .destroy = source_destroy,
        .get_width = source_width,
        .get_height = source_height,
        .get_defaults = source_defaults,
        .get_properties = source_properties,
        .update = source_update,
        .video_tick = source_tick,
        .video_render = source_render,
        .video_get_color_space = source_color_space,
        .icon_type = OBS_ICON_TYPE_WINDOW_CAPTURE,
    };
    obs_register_source(&info);
    return true;
}
