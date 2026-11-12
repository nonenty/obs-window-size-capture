/* WGC lives exclusively in a disposable child process. The OBS video thread
 * only polls ready CPU frames; neither a child nor its supervisor can make it
 * wait on capture shutdown. SPDX-License-Identifier: GPL-2.0-or-later */
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <obs-module.h>
#include "capture-host.h"

struct worker_session {
    HANDLE process, job, mapping, stop;
    struct capture_ipc *ipc;
    ULONGLONG started;
    LONG logged_status;
};
struct capture_host {
    HANDLE thread, stop, wake;
    CRITICAL_SECTION config_lock, frame_lock;
    struct capture_config config;
    volatile LONG revision, showing, target_pid, launches, worker_pid, active_revision;
    struct worker_session *session;
    wchar_t helper[MAX_PATH], obs_bin[MAX_PATH];
    char *label;
    gs_texture_t *texture; /* Graphics-thread owned. */
    uint32_t width, height;
    bool have_frame;
};

static bool executable_paths(struct capture_host *host)
{
    DWORD count = GetModuleFileNameW(GetModuleHandleW(L"obs.dll"), host->helper, MAX_PATH);
    if (!count || count >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(host->helper, L'\\');
    if (!slash) return false;
    size_t prefix = (size_t)(slash + 1 - host->helper);
    const wchar_t *name = L"window-size-capture-worker.exe";
    if (prefix + wcslen(name) >= MAX_PATH) return false;
    wcscpy(slash + 1, name);
    memcpy(host->obs_bin, host->helper, (prefix - 1) * sizeof(wchar_t));
    host->obs_bin[prefix - 1] = L'\0';
    return true;
}

static struct worker_session *launch_worker(struct capture_host *host, const struct capture_config *config,
                                            LONG revision, uint32_t capacity)
{
    struct worker_session *session = calloc(1, sizeof(*session));
    if (!session) return NULL;
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof(sa), .bInheritHandle = TRUE};
    size_t mapping_size = capture_mapping_size(capacity);
    session->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                          (DWORD)((uint64_t)mapping_size >> 32), (DWORD)mapping_size, NULL);
    session->stop = CreateEventW(&sa, TRUE, FALSE, NULL);
    session->job = CreateJobObjectW(NULL, NULL);
    if (!session->mapping || !session->stop || !session->job) goto fail;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(session->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) goto fail;
    session->ipc = MapViewOfFile(session->mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapping_size);
    if (!session->ipc) goto fail;
    session->ipc->version = CAPTURE_IPC_VERSION;
    session->ipc->capacity = capacity;
    session->ipc->revision = revision;
    session->ipc->config = *config;
    session->ipc->enabled = InterlockedCompareExchange(&host->showing, 0, 0);
    wchar_t command[MAX_PATH + 128];
    swprintf(command, sizeof(command) / sizeof(*command), L"\"%ls\" %llu %llu %llu", host->helper,
             (unsigned long long)(uintptr_t)session->mapping, (unsigned long long)(uintptr_t)session->stop,
             (unsigned long long)mapping_size);
    STARTUPINFOEXW startup = {0};
    startup.StartupInfo.cb = sizeof(startup);
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    startup.lpAttributeList = malloc(attr_size);
    if (!startup.lpAttributeList) goto fail;
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attr_size)) {
        free(startup.lpAttributeList); goto fail;
    }
    HANDLE inherited[] = {session->mapping, session->stop};
    BOOL attributes_ok = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                   inherited, sizeof(inherited), NULL, NULL);
    PROCESS_INFORMATION process = {0};
    BOOL created = attributes_ok && CreateProcessW(host->helper, command, NULL, NULL, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, host->obs_bin,
        &startup.StartupInfo, &process);
    DWORD launch_error = created ? 0 : GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList);
    if (!created) {
        blog(LOG_WARNING, "[window-size-capture: %s] worker launch failed (%lu); helper: %ls",
             host->label, (unsigned long)launch_error, host->helper);
        goto fail;
    }
    session->process = process.hProcess;
    if (!AssignProcessToJobObject(session->job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        goto fail;
    }
    session->started = GetTickCount64();
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    InterlockedExchange(&host->worker_pid, (LONG)process.dwProcessId);
    InterlockedIncrement(&host->launches);
    blog(LOG_INFO, "[window-size-capture: %s] isolated worker started, PID %lu", host->label,
         (unsigned long)process.dwProcessId);
    return session;
fail:
    if (session->process) CloseHandle(session->process);
    if (session->ipc) UnmapViewOfFile(session->ipc);
    if (session->job) CloseHandle(session->job);
    if (session->stop) CloseHandle(session->stop);
    if (session->mapping) CloseHandle(session->mapping);
    free(session);
    return NULL;
}

static void retire_session(struct worker_session *session)
{
    if (!session) return;
    SetEvent(session->stop);
    /* The child may hang while closing capture, even on a normal shutdown.
     * This bounded wait is only on our supervisor, never on OBS video/UI. */
    if (WaitForSingleObject(session->process, 750) != WAIT_OBJECT_0)
        TerminateJobObject(session->job, 1);
    CloseHandle(session->job); /* Kill-on-close also covers parent process exit. */
    CloseHandle(session->process);
    UnmapViewOfFile(session->ipc);
    CloseHandle(session->mapping);
    CloseHandle(session->stop);
    free(session);
}

static void publish_session(struct capture_host *host, struct worker_session *session)
{
    EnterCriticalSection(&host->frame_lock);
    host->session = session;
    InterlockedExchange(&host->target_pid, 0);
    InterlockedExchange(&host->active_revision, session ? session->ipc->revision : 0);
    LeaveCriticalSection(&host->frame_lock);
}

static bool worker_timed_out(ULONGLONG now, ULONGLONG heartbeat, ULONGLONG started)
{
    ULONGLONG last = heartbeat ? heartbeat : started;
    ULONGLONG timeout = heartbeat ? CAPTURE_HEARTBEAT_TIMEOUT_MS : CAPTURE_STARTUP_TIMEOUT_MS;
    return now >= last && now - last > timeout;
}

static DWORD WINAPI supervise(void *data)
{
    struct capture_host *host = data;
    struct worker_session *session = NULL;
    LONG revision = 0;
    uint32_t capacity = 0;
    uint64_t resume_window = 0;
    DWORD resume_pid = 0;
    unsigned failures = 0;
    ULONGLONG retry_at = 0;
    HANDLE events[] = {host->stop, host->wake};
    while (WaitForMultipleObjects(2, events, FALSE, 50) != WAIT_OBJECT_0) {
        ULONGLONG now = GetTickCount64();
        LONG requested = InterlockedCompareExchange(&host->revision, 0, 0);
        if (requested != revision) {
            publish_session(host, NULL);
            retire_session(session); session = NULL;
            InterlockedExchange(&host->target_pid, 0);
            revision = requested;
            capacity = 0; failures = 0; retry_at = 0; resume_window = 0; resume_pid = 0;
        }
        if (session) {
            DWORD pid = (DWORD)InterlockedCompareExchange(&session->ipc->target_pid, 0, 0);
            uint64_t window = (uint64_t)InterlockedCompareExchange64(&session->ipc->target_window, 0, 0);
            if (pid && window) { resume_window = window; resume_pid = pid; }
            LONG64 beat = InterlockedCompareExchange64(&session->ipc->heartbeat, 0, 0);
            LONG64 needed = InterlockedCompareExchange64(&session->ipc->required_bytes, 0, 0);
            LONG status = InterlockedCompareExchange(&session->ipc->status, 0, 0);
            if (status < 0 && status != session->logged_status) {
                blog(LOG_WARNING, "[window-size-capture: %s] worker reported error %ld", host->label, (long)status);
                session->logged_status = status;
            }
            bool exited = WaitForSingleObject(session->process, 0) == WAIT_OBJECT_0;
            if (exited) {
                DWORD code = 0; GetExitCodeProcess(session->process, &code);
                blog(LOG_WARNING, "[window-size-capture: %s] worker exit code 0x%08lX", host->label, (unsigned long)code);
                if (code == 0xC0000135u)
                    blog(LOG_WARNING, "[window-size-capture: %s] worker DLL dependency not found; OBS bin: %ls",
                         host->label, host->obs_bin);
            }
            /* Read the clock after the shared heartbeat: the worker may have
             * advanced it since this supervisor iteration began. */
            now = GetTickCount64();
            bool stale = worker_timed_out(now, (ULONGLONG)beat, session->started);
            bool grow = needed > capacity && needed <= CAPTURE_MAX_FRAME_BYTES;
            if (exited || stale || grow) {
                blog(LOG_WARNING, "[window-size-capture: %s] worker %s; restarting", host->label,
                     exited ? "exited" : stale ? "heartbeat timed out" : "frame buffer resized");
                publish_session(host, NULL);
                retire_session(session); session = NULL;
                InterlockedExchange(&host->target_pid, 0);
                InterlockedExchange(&host->worker_pid, 0);
                if (grow) { capacity = (uint32_t)needed; retry_at = now; }
                else {
                    if (failures < 5) ++failures;
                    retry_at = GetTickCount64() + (1000u << (failures - 1));
                }
            } else {
                InterlockedExchange(&session->ipc->enabled, InterlockedCompareExchange(&host->showing, 0, 0));
                InterlockedExchange(&host->target_pid, InterlockedCompareExchange(&session->ipc->target_pid, 0, 0));
                if (now - session->started > 30000 && beat) failures = 0;
            }
        }
        if (!session && requested && now >= retry_at) {
            struct capture_config config;
            EnterCriticalSection(&host->config_lock);
            config = host->config;
            LONG snapshot_revision = InterlockedCompareExchange(&host->revision, 0, 0);
            LeaveCriticalSection(&host->config_lock);
            if (snapshot_revision != revision) continue;
            config.resume_window = resume_window; config.resume_pid = resume_pid;
            if (!*config.exe || !config.width || !config.height) continue;
            uint64_t initial = (uint64_t)config.width * config.height * 4;
            if (initial > CAPTURE_MAX_FRAME_BYTES) {
                blog(LOG_WARNING, "[window-size-capture: %s] frame exceeds 128 MiB limit", host->label);
                retry_at = now + 30000; continue;
            }
            if (capacity < initial) capacity = (uint32_t)initial;
            session = launch_worker(host, &config, revision, capacity);
            if (session) publish_session(host, session);
            else retry_at = now + 5000;
        }
    }
    publish_session(host, NULL);
    retire_session(session);
    InterlockedExchange(&host->worker_pid, 0);
    return 0;
}

struct capture_host *capture_host_create(const char *label)
{
    struct capture_host *host = calloc(1, sizeof(*host));
    if (!host) return NULL;
    host->label = strdup(label ? label : "capture");
    InitializeCriticalSection(&host->config_lock);
    InitializeCriticalSection(&host->frame_lock);
    host->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    host->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!host->label || !host->stop || !host->wake || !executable_paths(host)) goto fail;
    host->thread = CreateThread(NULL, 0, supervise, host, 0, NULL);
    if (host->thread) return host;
fail:
    if (host->stop) CloseHandle(host->stop);
    if (host->wake) CloseHandle(host->wake);
    DeleteCriticalSection(&host->frame_lock);
    DeleteCriticalSection(&host->config_lock);
    free(host->label); free(host);
    return NULL;
}

void capture_host_configure(struct capture_host *host, const struct capture_config *config)
{
    EnterCriticalSection(&host->config_lock);
    const struct capture_config *old = &host->config;
    if (wcscmp(old->exe, config->exe) == 0 && old->width == config->width && old->height == config->height &&
        old->tolerance == config->tolerance && old->client_area == config->client_area &&
        old->cursor == config->cursor && old->include_children == config->include_children) {
        LeaveCriticalSection(&host->config_lock);
        return;
    }
    host->config = *config;
    InterlockedIncrement(&host->revision);
    LeaveCriticalSection(&host->config_lock);
    SetEvent(host->wake);
}

void capture_host_clear(struct capture_host *host)
{
    host->have_frame = false;
    host->width = host->height = 0;
    obs_enter_graphics();
    gs_texture_destroy(host->texture); host->texture = NULL;
    obs_leave_graphics();
}

void capture_host_tick(struct capture_host *host, bool showing)
{
    InterlockedExchange(&host->showing, showing);
    if (!showing || !TryEnterCriticalSection(&host->frame_lock)) return;
    struct worker_session *session = host->session;
    if (!session || session->ipc->revision != InterlockedCompareExchange(&host->revision, 0, 0)) goto done;
    struct capture_ipc *ipc = session->ipc;
    int newest = -1; uint64_t sequence = 0;
    for (unsigned i = 0; i < CAPTURE_IPC_SLOTS; ++i) {
        if (InterlockedCompareExchange(&ipc->slots[i].state, CAPTURE_READING, CAPTURE_READY) != CAPTURE_READY)
            continue;
        if (ipc->slots[i].sequence > sequence) {
            if (newest >= 0) InterlockedExchange(&ipc->slots[newest].state, CAPTURE_FREE);
            newest = (int)i; sequence = ipc->slots[i].sequence;
        } else InterlockedExchange(&ipc->slots[i].state, CAPTURE_FREE);
    }
    if (newest >= 0) {
        const struct capture_slot *slot = &ipc->slots[newest];
        if (capture_frame_valid(slot, ipc->capacity)) {
            obs_enter_graphics();
            if (!host->texture || host->width != slot->width || host->height != slot->height) {
                gs_texture_destroy(host->texture);
                host->texture = gs_texture_create(slot->width, slot->height, GS_BGRA, 1, NULL, GS_DYNAMIC);
            }
            if (host->texture) {
                gs_texture_set_image(host->texture, capture_slot_pixels(ipc, (unsigned)newest), slot->stride, false);
                host->width = slot->width; host->height = slot->height;
                host->have_frame = true;
            }
            obs_leave_graphics();
        }
        InterlockedExchange(&ipc->slots[newest].state, CAPTURE_FREE);
    }
done:
    LeaveCriticalSection(&host->frame_lock);
}

void capture_host_render(struct capture_host *host)
{
    if (!host->have_frame || !host->texture) return;
    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    bool previous = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), host->texture);
    while (gs_effect_loop(effect, "Draw")) gs_draw_sprite(host->texture, 0, 0, 0);
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(previous);
}

uint32_t capture_host_width(struct capture_host *host) { return host->width; }
uint32_t capture_host_height(struct capture_host *host) { return host->height; }
DWORD capture_host_target_pid(struct capture_host *host)
{
    if (InterlockedCompareExchange(&host->active_revision, 0, 0) !=
        InterlockedCompareExchange(&host->revision, 0, 0)) return 0;
    return (DWORD)InterlockedCompareExchange(&host->target_pid, 0, 0);
}
static void dispose_host(void *data)
{
    struct capture_host *host = data;
    WaitForSingleObject(host->thread, INFINITE);
    CloseHandle(host->thread); CloseHandle(host->wake); CloseHandle(host->stop);
    DeleteCriticalSection(&host->frame_lock); DeleteCriticalSection(&host->config_lock);
    free(host->label); free(host);
}
void capture_host_destroy(struct capture_host *host)
{
    if (!host) return;
    SetEvent(host->stop);
    capture_host_clear(host);
    obs_queue_task(OBS_TASK_DESTROY, dispose_host, host, false);
}
