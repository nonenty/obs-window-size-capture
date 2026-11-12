/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native Win32 regression tests. Include the implementation so selection and
 * reconnect checks exercise the same code OBS uses, without starting capture. */
#include "../window-size-capture.c"

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static HWND make_window(const wchar_t *title, DWORD ex_style, DWORD style, HWND owner)
{
    HWND window = CreateWindowExW(ex_style, L"CPObsFloatingTest", title, style,
                                 80, 80, 480, 280, owner, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(window != NULL);
    if (window) {
        ShowWindow(window, SW_SHOWNOACTIVATE);
        UpdateWindow(window);
    }
    return window;
}

static bool list_contains(obs_properties_t *props, const char *key)
{
    obs_property_t *list = obs_properties_get(props, "window_pick");
    for (size_t i = 0; i < obs_property_list_item_count(list); ++i) {
        if (strcmp(obs_property_list_item_string(list, i), key) == 0)
            return true;
    }
    return false;
}

static void check_property_refresh(HWND tool)
{
    struct window_info info = {0};
    CHECK(inspect_window(tool, true, true, &info));
    char key[17];
    window_key(&info, key);
    obs_properties_t *props = source_properties(NULL);
    obs_data_t *settings = obs_data_create();
    source_defaults(settings);
    CHECK(!list_contains(props, key));
    obs_data_set_bool(settings, "include_children", true);
    CHECK(obs_property_modified(obs_properties_get(props, "include_children"), settings));
    CHECK(list_contains(props, key));

    /* List dimensions follow the pending client-area setting immediately. */
    obs_data_set_bool(settings, "client_area", false);
    CHECK(obs_property_modified(obs_properties_get(props, "client_area"), settings));
    CHECK(!list_contains(props, key));
    CHECK(inspect_window(tool, false, true, &info));
    window_key(&info, key);
    CHECK(list_contains(props, key));
    obs_data_set_bool(settings, "include_children", false);
    CHECK(obs_property_modified(obs_properties_get(props, "include_children"), settings));
    CHECK(!list_contains(props, key));
    obs_data_release(settings);
    obs_properties_destroy(props);
}

static void check_selection(HWND window, bool untitled)
{
    struct window_info info = {0};
    CHECK(inspect_window(window, true, true, &info));
    char key[17];
    window_key(&info, key);
    struct select_context lookup = {.key = key, .client_area = true};
    EnumWindows(select_window, (LPARAM)&lookup);
    CHECK(lookup.found == !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW));
    lookup.include_children = true;
    EnumWindows(select_window, (LPARAM)&lookup);
    CHECK(lookup.found && lookup.info.window == window);

    obs_data_t *settings = obs_data_create();
    source_defaults(settings);
    CHECK(!obs_data_get_bool(settings, "include_children"));
    obs_data_set_bool(settings, "include_children", true);
    obs_data_set_string(settings, "window_pick", key);
    CHECK(window_selected(NULL, NULL, settings));
    CHECK(obs_data_get_int(settings, "width") == info.width);
    CHECK(obs_data_get_int(settings, "height") == info.height);
    CHECK(strlen(obs_data_get_string(settings, "exe")) > 0);
    CHECK(strlen(obs_data_get_string(settings, "window_class")) == 0);

    struct size_capture source = {0};
    InitializeCriticalSection(&source.lock);
    read_settings(&source, settings);
    DWORD pid = 0;
    int score = -1;
    CHECK(matches_window(&source, window, &pid, &score));
    CHECK(pid == GetCurrentProcessId() && score == 0);
    source.include_children = false;
    CHECK(matches_window(&source, window, &pid, &score) ==
          !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW));
    source.include_children = true;

    ShowWindow(window, SW_HIDE);
    CHECK(!inspect_window(window, true, true, &info));
    CHECK(!matches_window(&source, window, &pid, &score));
    ShowWindow(window, SW_SHOWNOACTIVATE);
    CHECK(matches_window(&source, window, &pid, &score));

    ShowWindow(window, SW_MINIMIZE);
    CHECK(!inspect_window(window, true, true, &info));
    CHECK(!matches_window(&source, window, &pid, &score));
    ShowWindow(window, SW_SHOWNOACTIVATE);
    CHECK(matches_window(&source, window, &pid, &score));

    if (untitled) {
        SetWindowTextW(window, L"Window now has a title");
        CHECK(matches_window(&source, window, &pid, &score));
        SetWindowTextW(window, L"");
    }
    RECT rect;
    GetWindowRect(window, &rect);
    SetWindowPos(window, NULL, 0, 0, rect.right - rect.left + 100, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    CHECK(!matches_window(&source, window, &pid, &score));
    SetWindowPos(window, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    /* Recreated HWNDs must reconnect using saved identity, not the old handle. */
    HWND owner = GetWindow(window, GW_OWNER);
    DWORD ex_style = (DWORD)GetWindowLongPtrW(window, GWL_EXSTYLE);
    DWORD style = (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
    DestroyWindow(window);
    HWND replacement = make_window(L"Recreated with a different title", ex_style, style, owner);
    SetWindowPos(replacement, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    struct window_search search = {.self = &source, .score = INT32_MAX, .pid = UINT32_MAX};
    EnumWindows(find_window, (LPARAM)&search);
    CHECK(search.best == replacement && search.pid == GetCurrentProcessId());
    DestroyWindow(replacement);

    /* Choosing any window clears the obsolete title-based class filter. */
    if (untitled && owner) {
        struct window_info owner_info = {0};
        CHECK(inspect_window(owner, true, true, &owner_info));
        window_key(&owner_info, key);
        obs_data_set_string(settings, "window_pick", key);
        CHECK(window_selected(NULL, NULL, settings));
        CHECK(strlen(obs_data_get_string(settings, "window_class")) == 0);
    }

    free(source.exe);
    DeleteCriticalSection(&source.lock);
    obs_data_release(settings);
}

static void check_title_independent_matching(void)
{
    HWND window = make_window(L"vatSys before connection", 0, WS_OVERLAPPEDWINDOW, NULL);
    SetWindowPos(window, NULL, 0, 0, 730, 450, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    struct window_info info = {0};
    CHECK(inspect_window(window, true, false, &info));
    char key[17];
    window_key(&info, key);
    obs_data_t *settings = obs_data_create();
    source_defaults(settings);
    char *exe = utf8_copy(info.exe);
    obs_data_set_string(settings, "exe", exe);
    free(exe);
    obs_data_set_int(settings, "width", info.width);
    obs_data_set_int(settings, "height", info.height);
    /* An existing scene retains its old title, class and title-dependent key. */
    obs_data_set_string(settings, "title", "Saved obsolete title");
    obs_data_set_string(settings, "window_class", "Saved obsolete class");
    obs_data_set_string(settings, "window_pick", "0123456789ABCDEF");
    struct size_capture source = {0};
    InitializeCriticalSection(&source.lock);
    read_settings(&source, settings);
    source.window = window;
    source.pid = GetCurrentProcessId();
    DWORD pid = 0;
    int score = -1;
    CHECK(matches_window(&source, window, &pid, &score) && score == 0);
    CHECK(anchor_valid(&source));

    obs_properties_t *props = source_properties(NULL);
    populate_window_list(props, settings);
    CHECK(strcmp(obs_data_get_string(settings, "window_pick"), key) == 0);
    CHECK(list_contains(props, key));
    CHECK(obs_properties_get(props, "title") == NULL);
    SetWindowTextW(window, L"vatSys connected - new controller callsign");
    CHECK(inspect_window(window, true, false, &info));
    char changed_key[17];
    window_key(&info, changed_key);
    CHECK(strcmp(key, changed_key) == 0);
    CHECK(anchor_valid(&source));
    source.include_children = true;
    CHECK(anchor_valid(&source));
    source.include_children = false;
    populate_window_list(props, settings);
    CHECK(strcmp(obs_data_get_string(settings, "window_pick"), key) == 0);
    CHECK(window_selected(NULL, NULL, settings));
    CHECK(strcmp(obs_data_get_string(settings, "title"), "vatSys connected - new controller callsign") == 0);
    SetWindowTextW(window, L"");
    CHECK(inspect_window(window, true, false, &info));
    window_key(&info, changed_key);
    CHECK(strcmp(key, changed_key) == 0 && anchor_valid(&source));

    /* Executable identity and dimensions must still reject other targets. */
    wchar_t *original_exe = source.exe;
    source.exe = wide_copy("unrelated.exe");
    CHECK(!matches_window(&source, window, &pid, &score));
    free(source.exe);
    source.exe = NULL;
    CHECK(!matches_window(&source, window, &pid, &score));
    source.exe = original_exe;
    for (wchar_t *letter = source.exe; *letter; ++letter)
        *letter = towupper(*letter);
    CHECK(matches_window(&source, window, &pid, &score));
    wcscpy(info.exe, source.exe);
    window_key(&info, changed_key);
    CHECK(strcmp(key, changed_key) == 0);
    source.tolerance = 0;
    ++source.target_width;
    CHECK(!matches_window(&source, window, &pid, &score));
    source.tolerance = 1;
    CHECK(matches_window(&source, window, &pid, &score) && score == 1);
    --source.target_width;

    /* Reconnect to a new HWND using saved executable/size, despite a new title. */
    DestroyWindow(window);
    window = make_window(L"vatSys after restart", 0, WS_OVERLAPPEDWINDOW, NULL);
    SetWindowPos(window, NULL, 0, 0, 730, 450, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    struct window_search search = {.self = &source, .score = INT32_MAX, .pid = UINT32_MAX};
    EnumWindows(find_window, (LPARAM)&search);
    CHECK(search.best == window && search.pid == GetCurrentProcessId());
    DestroyWindow(window);
    populate_window_list(props, settings);
    CHECK(strcmp(obs_data_get_string(settings, "window_pick"), key) == 0);
    /* The missing entry retains the stable identity while the app is closed. */
    CHECK(list_contains(props, key));
    obs_properties_destroy(props);
    obs_data_release(settings);
    free(source.exe);
    DeleteCriticalSection(&source.lock);
}

static struct process_window *member(struct size_capture *source, HWND window)
{
    for (struct process_window *item = source->process_windows; item; item = item->next) {
        if (item->window == window)
            return item;
    }
    return NULL;
}

static int freed_captures;
static void fake_free(struct winrt_capture *capture)
{
    CHECK(capture != NULL);
    ++freed_captures;
}

static void check_process_collection(HWND root)
{
    struct size_capture source = {.window = root, .pid = GetCurrentProcessId(),
                                 .include_children = true, .client_area = true};
    InitializeCriticalSection(&source.lock);
    source.api.free = fake_free;
    HWND first = make_window(L"Process floating window", 0, WS_OVERLAPPEDWINDOW, root);
    HWND second = make_window(L"", WS_EX_TOOLWINDOW, WS_OVERLAPPEDWINDOW, NULL);
    HWND child = make_window(L"Embedded control", 0, WS_CHILD, root);
    HWND tiny = make_window(L"", WS_EX_TOOLWINDOW, WS_POPUP, root);
    SetWindowPos(tiny, NULL, 160, 160, 20, 15, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(second, HWND_TOP, 240, 200, 480, 280, SWP_NOACTIVATE);
    collect_process_windows(&source);
    CHECK(member(&source, root) != NULL);
    CHECK(member(&source, first) != NULL);
    CHECK(member(&source, second) != NULL);
    CHECK(member(&source, tiny) != NULL);
    CHECK(member(&source, child) == NULL);
    RECT rect;
    RECT client_rect;
    CHECK(process_window_rect(root, source.pid, true, &rect));
    CHECK(EqualRect(&member(&source, root)->rect, &rect));
    CHECK(process_window_rect(second, source.pid, false, &rect));
    CHECK(EqualRect(&member(&source, second)->rect, &rect));
    CHECK(process_window_rect(second, source.pid, true, &client_rect));
    CHECK(rect.top < client_rect.top);
    CHECK(!process_window_rect(GetDesktopWindow(), source.pid, true, &rect));
    GetClientRect(root, &rect);
    CHECK(source_width(&source) == (uint32_t)rect.right);
    CHECK(source_height(&source) == (uint32_t)rect.bottom);
    int count = 0, first_index = -1, second_index = -1;
    for (struct process_window *item = source.process_windows; item; item = item->next, ++count) {
        DWORD pid = 0;
        GetWindowThreadProcessId(item->window, &pid);
        CHECK(pid == source.pid);
        if (item->window == first) first_index = count;
        if (item->window == second) second_index = count;
    }
    CHECK(count == 4 && first_index < second_index);

    struct process_window *original = member(&source, first);
    CHECK(original != NULL);
    if (original) {
        original->capture = (struct winrt_capture *)(uintptr_t)1;
        RECT before = original->rect;
        RECT window_rect;
        GetWindowRect(first, &window_rect);
        SetWindowPos(first, HWND_TOP, window_rect.left + 120, window_rect.top + 80,
                     0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        collect_process_windows(&source);
        CHECK(member(&source, first) == original);
        CHECK(original->capture == (struct winrt_capture *)(uintptr_t)1);
        CHECK(original->rect.left == before.left + 120 && original->rect.top == before.top + 80);
        CHECK(freed_captures == 0);
        ShowWindow(first, SW_HIDE);
        collect_process_windows(&source);
        CHECK(member(&source, first) == NULL && freed_captures == 1);
        ShowWindow(first, SW_SHOWNOACTIVATE);
        collect_process_windows(&source);
        CHECK(member(&source, first) != NULL);
    }
    ShowWindow(second, SW_MINIMIZE);
    collect_process_windows(&source);
    CHECK(member(&source, second) == NULL);
    ShowWindow(second, SW_SHOWNOACTIVATE);
    collect_process_windows(&source);
    CHECK(member(&source, second) != NULL);
    SetWindowPos(second, NULL, source.canvas_rect.right + 100, 0, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    collect_process_windows(&source);
    CHECK(member(&source, second) == NULL);

    source.target_width = 1;
    source.target_height = 1;
    CHECK(anchor_valid(&source));
    RECT root_rect;
    GetWindowRect(root, &root_rect);
    SetWindowPos(root, NULL, 0, 0, root_rect.right - root_rect.left + 60,
                 root_rect.bottom - root_rect.top + 40, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    collect_process_windows(&source);
    GetClientRect(root, &rect);
    CHECK(source_width(&source) == (uint32_t)rect.right);
    CHECK(source_height(&source) == (uint32_t)rect.bottom);
    CHECK(anchor_valid(&source));
    SetWindowPos(root, NULL, 0, 0, root_rect.right - root_rect.left, root_rect.bottom - root_rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(root, SW_HIDE);
    collect_process_windows(&source);
    CHECK(source.process_windows == NULL);
    ShowWindow(root, SW_SHOWNOACTIVATE);
    collect_process_windows(&source);
    CHECK(member(&source, root) != NULL);
    source.include_children = false;
    CHECK(!anchor_valid(&source));
    source.include_children = true;
    DestroyWindow(first);
    collect_process_windows(&source);
    CHECK(member(&source, first) == NULL);
    source.pid = 0;
    collect_process_windows(&source);
    CHECK(source.process_windows == NULL);
    release_process_windows(&source);
    DeleteCriticalSection(&source.lock);
    DestroyWindow(second);
    DestroyWindow(child);
    DestroyWindow(tiny);
}

int main(void)
{
    WNDCLASSW window_class = {.lpfnWndProc = DefWindowProcW,
                             .hInstance = GetModuleHandleW(NULL),
                             .lpszClassName = L"CPObsFloatingTest"};
    CHECK(RegisterClassW(&window_class) != 0);
    HWND root = make_window(L"CP OBS regression test", 0, WS_OVERLAPPEDWINDOW, NULL);
    HWND owned = make_window(L"CP OBS owned floating window", 0, WS_OVERLAPPEDWINDOW, root);
    HWND child = make_window(L"CP OBS embedded panel", 0, WS_CHILD, root);
    struct window_info info = {0};
    CHECK(inspect_window(root, true, false, &info));
    CHECK(inspect_window(owned, true, false, &info));
    CHECK(inspect_window(owned, true, true, &info));
    CHECK(!inspect_window(child, true, true, &info));
    CHECK(!inspect_window(child, true, false, &info));
    DestroyWindow(child);
    DestroyWindow(owned);
    check_title_independent_matching();

    HWND tool = make_window(L"CP OBS floating tool", WS_EX_TOOLWINDOW, WS_OVERLAPPEDWINDOW, root);
    SetWindowPos(tool, NULL, 0, 0, 520, 320, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    CHECK(!inspect_window(tool, true, false, &info));
    CHECK(inspect_window(tool, true, true, &info));
    check_property_refresh(tool);
    check_selection(tool, false);

    HWND untitled = make_window(L"", 0, WS_OVERLAPPEDWINDOW, root);
    SetWindowPos(untitled, NULL, 0, 0, 540, 340, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    CHECK(inspect_window(untitled, true, false, &info));
    CHECK(inspect_window(untitled, true, true, &info));
    check_selection(untitled, true);

    HWND unowned = make_window(L"", 0, WS_OVERLAPPEDWINDOW, NULL);
    SetWindowPos(unowned, NULL, 0, 0, 560, 360, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    CHECK(inspect_window(unowned, true, false, &info));
    CHECK(inspect_window(unowned, true, true, &info));
    check_selection(unowned, true);
    check_process_collection(root);
    DestroyWindow(root);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    printf("Floating window tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
