/*
 * buckle-gui.c — Win32 GUI launcher for Bucklespring
 *
 * Build (MSYS2 / MinGW-w64):
 *
 *   windres -O coff buckle-gui.rc -o buckle-gui.res
 *   gcc -O2 -mwindows -o buckle-gui.exe buckle-gui.c buckle-gui.res \
 *       -luser32 -lgdi32 -lshell32 -lcomctl32 -lole32
 *
 * Place buckle-gui.exe in the same directory as:
 *   buckle.exe  ALURE32.dll  libopenal-1.dll  wav/
 */

#define _WIN32_WINNT 0x0601
#define _WIN32_IE    0x0700
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>

#define IDI_MYICON    1
#define WM_LOG_APPEND (WM_APP + 1)

#include <stdio.h>

/* ── control ids ─────────────────────────────────────────────────────── */
enum {
    ID_SLD_GAIN       = 100,
    ID_VAL_GAIN,
    ID_SLD_STEREO,
    ID_VAL_STEREO,
    ID_EDT_PATH,
    ID_BTN_BROWSE,
    ID_EDT_DEVICE,
    ID_CHK_MUTED,
    ID_CHK_NOCLICK,
    ID_CHK_FALLBACK,
    ID_CHK_AUTOLAUNCH,
    ID_EDT_MUTEKEY,
    ID_BTN_START,
    ID_BTN_STOP,
    ID_BTN_AUTOSTART,
    ID_LBL_STATUS,
    ID_EDT_LOG,
    ID_BTN_CLEARLOG,
    ID_TIMER_POLL     = 200,
    IDM_SHOW          = 300,
    IDM_MUTE,
    IDM_STOP,
    IDM_EXIT,
};

#define WM_TRAYICON    (WM_USER + 1)
#define TRAY_UID       1
#define RUN_KEY        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE      L"Bucklespring"
#define LOG_MAX_CHARS  20000
#define LOG_TRIM_CHARS 10000

/* ── globals ─────────────────────────────────────────────────────────── */
static HINSTANCE           g_inst;
static HWND                g_wnd;
static HANDLE              g_hMutex     = NULL;
static PROCESS_INFORMATION g_pi         = {0};
static NOTIFYICONDATA      g_nid        = {0};
static BOOL                g_running    = FALSE;
static WCHAR               g_dir[MAX_PATH];
static WCHAR               g_ini[MAX_PATH];
static HANDLE              g_hReadPipe  = NULL;
static HANDLE              g_hWritePipe = NULL;
static HANDLE              g_hLogThread = NULL;

static HWND H_sld_gain,    H_val_gain;
static HWND H_sld_stereo,  H_val_stereo;
static HWND H_edt_path,    H_btn_browse;
static HWND H_edt_device;
static HWND H_chk_muted,   H_chk_noclick, H_chk_fallback;
static HWND H_chk_autolaunch;
static HWND H_edt_mutekey;
static HWND H_btn_start,   H_btn_stop, H_btn_autostart;
static HWND H_lbl_status;
static HWND H_edt_log,     H_btn_clearlog;

/* ── tiny widget helpers ─────────────────────────────────────────────── */
static HWND mklabel(HWND p, int x, int y, int w, int h, const WCHAR *t)
{
    return CreateWindowExW(0, L"STATIC", t,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, p, NULL, g_inst, NULL);
}

static HWND mkedit(HWND p, int id, int x, int y, int w)
{
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        x, y, w, 22, p, (HMENU)(INT_PTR)id, g_inst, NULL);
}

static HWND mkbutton(HWND p, int id, int x, int y, int w, int h, const WCHAR *t)
{
    return CreateWindowExW(0, L"BUTTON", t,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, w, h, p, (HMENU)(INT_PTR)id, g_inst, NULL);
}

static HWND mkcheck(HWND p, int id, int x, int y, int w, const WCHAR *t)
{
    return CreateWindowExW(0, L"BUTTON", t,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        x, y, w, 20, p, (HMENU)(INT_PTR)id, g_inst, NULL);
}

static HWND mkslider(HWND p, int id, int x, int y, int w,
                     int lo, int hi, int val)
{
    HWND h = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        x, y, w, 24, p, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(h, TBM_SETRANGE, TRUE, MAKELONG(lo, hi));
    SendMessageW(h, TBM_SETPOS,   TRUE, val);
    return h;
}

static HWND mkgroup(HWND p, int x, int y, int w, int h, const WCHAR *t)
{
    return CreateWindowExW(0, L"BUTTON", t,
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x, y, w, h, p, NULL, g_inst, NULL);
}

static void fnt(HWND h, HFONT f) { SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); }

/* ── log helpers ─────────────────────────────────────────────────────── */
static void log_trim_if_needed(void)
{
    int len = GetWindowTextLengthW(H_edt_log);
    if (len < LOG_MAX_CHARS) return;
    SendMessageW(H_edt_log, EM_SETSEL, 0, LOG_TRIM_CHARS);
    SendMessageW(H_edt_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
}

static void log_append(const WCHAR *text)
{
    log_trim_if_needed();
    int len = GetWindowTextLengthW(H_edt_log);
    SendMessageW(H_edt_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(H_edt_log, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(H_edt_log, EM_SCROLLCARET, 0, 0);
}

static void log_clear(void)
{
    SetWindowTextW(H_edt_log, L"");
}

/* ── pipe reader thread ──────────────────────────────────────────────── */
static DWORD WINAPI log_reader_thread(LPVOID param)
{
    (void)param;
    char  buf[512];
    DWORD nread;

    while (ReadFile(g_hReadPipe, buf, sizeof(buf) - 1, &nread, NULL) && nread > 0) {
        buf[nread] = '\0';

        /* buckle outputs via fprintf — system ANSI codepage, not UTF-8 */
        int wlen = MultiByteToWideChar(CP_ACP, 0, buf, -1, NULL, 0);
        WCHAR *wbuf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0,
                                          wlen * sizeof(WCHAR));
        if (!wbuf) continue;
        MultiByteToWideChar(CP_ACP, 0, buf, -1, wbuf, wlen);

        int rlen = wlen * 2;
        WCHAR *rbuf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0,
                                          rlen * sizeof(WCHAR));
        if (!rbuf) { HeapFree(GetProcessHeap(), 0, wbuf); continue; }

        int ri = 0;
        for (int i = 0; wbuf[i]; i++) {
            if (wbuf[i] == L'\n' && (i == 0 || wbuf[i-1] != L'\r'))
                rbuf[ri++] = L'\r';
            rbuf[ri++] = wbuf[i];
        }
        rbuf[ri] = L'\0';

        HeapFree(GetProcessHeap(), 0, wbuf);

        /*
         * PostMessageW returns 0 if the window is gone (destroyed or
         * queue full). In that case WM_LOG_APPEND will never run, so
         * we own rbuf and must free it here to avoid a leak.
         */
        if (!PostMessageW(g_wnd, WM_LOG_APPEND, 0, (LPARAM)rbuf))
            HeapFree(GetProcessHeap(), 0, rbuf);
    }

    return 0;
}

/* ── autostart (registry) ────────────────────────────────────────────── */
static BOOL autostart_is_enabled(void)
{
    HKEY  hk;
    WCHAR val[MAX_PATH] = {0};
    DWORD sz = sizeof(val);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return FALSE;

    BOOL found = RegQueryValueExW(hk, RUN_VALUE, NULL, NULL,
                                   (LPBYTE)val, &sz) == ERROR_SUCCESS;
    RegCloseKey(hk);
    return found;
}

static void autostart_set(BOOL enable)
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0,
                      KEY_SET_VALUE, &hk) != ERROR_SUCCESS) {
        log_append(L"[gui] ERROR: could not open Run registry key.\r\n");
        return;
    }

    if (enable) {
        WCHAR exe[MAX_PATH];
        _snwprintf(exe, MAX_PATH, L"\"%s%s\"", g_dir, L"buckle-gui.exe");
        LSTATUS r = RegSetValueExW(hk, RUN_VALUE, 0, REG_SZ,
                                    (const BYTE *)exe,
                                    (DWORD)((wcslen(exe) + 1) * sizeof(WCHAR)));
        if (r == ERROR_SUCCESS)
            log_append(L"[gui] Auto-start enabled.\r\n");
        else
            log_append(L"[gui] ERROR: failed to write registry value.\r\n");
    } else {
        LSTATUS r = RegDeleteValueW(hk, RUN_VALUE);
        if (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND)
            log_append(L"[gui] Auto-start disabled.\r\n");
        else
            log_append(L"[gui] ERROR: failed to remove registry value.\r\n");
    }

    RegCloseKey(hk);
}

static void update_autostart_button(void)
{
    if (autostart_is_enabled())
        SetWindowTextW(H_btn_autostart, L"Autostart: ON");
    else
        SetWindowTextW(H_btn_autostart, L"Autostart: OFF");
}

static void toggle_autostart(void)
{
    autostart_set(!autostart_is_enabled());
    update_autostart_button();
}

/* ── forward declarations ────────────────────────────────────────────── */
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void create_controls(HWND);
static void start_buckle(void);
static void stop_buckle(void);
static void update_status(void);
static void add_tray(void);
static void remove_tray(void);
static void show_tray_menu(void);
static void browse_path(void);
static void update_slider_label(HWND slider, HWND label);
static void save_settings(void);
static void load_settings(void);
static void send_mute_sequence(void);

/* ── WinMain ──────────────────────────────────────────────────────────── */
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;
    g_inst = inst;

    g_hMutex = CreateMutexW(NULL, TRUE, L"BuckleGUI_SingleInstance");
    if (g_hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"BuckleGUI", NULL);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(g_hMutex);
        return 0;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    WCHAR *sl = wcsrchr(g_dir, L'\\');
    if (sl) *(sl + 1) = L'\0';

    _snwprintf(g_ini, MAX_PATH, L"%sbuckle-gui.ini", g_dir);

    WNDCLASSEXW wc   = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"BuckleGUI";
    wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(IDI_MYICON));
    wc.hIconSm       = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_MYICON),
                            IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT  r     = {0, 0, 420, 500};
    AdjustWindowRectEx(&r, style, FALSE, 0);

    g_wnd = CreateWindowExW(0,
        L"BuckleGUI", L"Bucklespring",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right  - r.left,
        r.bottom - r.top,
        NULL, NULL, inst, NULL);

    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}

/* ── WndProc ─────────────────────────────────────────────────────────── */
static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE:
        create_controls(wnd);
        load_settings();
        add_tray();
        update_autostart_button();
        SetTimer(wnd, ID_TIMER_POLL, 1000, NULL);
        if (SendMessageW(H_chk_autolaunch, BM_GETCHECK, 0, 0) == BST_CHECKED)
            start_buckle();
        return 0;

    case WM_LOG_APPEND: {
        WCHAR *text = (WCHAR *)lp;
        if (text) {
            log_append(text);
            HeapFree(GetProcessHeap(), 0, text);
        }
        return 0;
    }

    case WM_HSCROLL: {
        HWND ctl = (HWND)lp;
        if (ctl == H_sld_gain)   update_slider_label(H_sld_gain,   H_val_gain);
        if (ctl == H_sld_stereo) update_slider_label(H_sld_stereo, H_val_stereo);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_START:     start_buckle();     break;
        case ID_BTN_STOP:      stop_buckle();      break;
        case ID_BTN_BROWSE:    browse_path();      break;
        case ID_BTN_CLEARLOG:  log_clear();        break;
        case ID_BTN_AUTOSTART: toggle_autostart(); break;
        case IDM_SHOW:
            ShowWindow(wnd, SW_RESTORE);
            SetForegroundWindow(wnd);
            break;
        case IDM_MUTE: send_mute_sequence(); break;
        case IDM_STOP: stop_buckle();        break;
        case IDM_EXIT:
            stop_buckle();
            save_settings();
            DestroyWindow(wnd);
            break;
        }
        return 0;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK) {
            ShowWindow(wnd, SW_RESTORE);
            SetForegroundWindow(wnd);
        } else if (lp == WM_RBUTTONUP) {
            show_tray_menu();
        }
        return 0;

    case WM_TIMER:
        if (wp == ID_TIMER_POLL && g_running && g_pi.hProcess) {
            DWORD code = STILL_ACTIVE;
            GetExitCodeProcess(g_pi.hProcess, &code);
            if (code != STILL_ACTIVE) {
                CloseHandle(g_pi.hProcess);
                CloseHandle(g_pi.hThread);
                ZeroMemory(&g_pi, sizeof(g_pi));
                g_running = FALSE;
                update_status();
                log_append(L"[gui] buckle process exited.\r\n");
            }
        }
        return 0;

    case WM_CLOSE:
        save_settings();
        stop_buckle(); 
        DestroyWindow(wnd);
        return 0;

    case WM_DESTROY:
        KillTimer(wnd, ID_TIMER_POLL);
        stop_buckle();
        remove_tray();
        if (g_hMutex) {
            CloseHandle(g_hMutex);
            g_hMutex = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

/* ── create_controls ─────────────────────────────────────────────────── */
static void create_controls(HWND wnd)
{
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int   m = 10;
    int   gw = 400;

    /* Audio group ───────────────────────────────────────────────── */
    HWND g1 = mkgroup(wnd, m, m, gw, 152, L" Audio ");
    fnt(g1, f);

    int y = m + 22;
    fnt(mklabel(wnd, m+8,   y+3, 90, 16, L"Gain:"), f);
    H_sld_gain  = mkslider(wnd, ID_SLD_GAIN,   m+102, y,    236, 0, 100, 100);
    H_val_gain  = mklabel (wnd, m+346, y+3, 44, 16, L"100");
    fnt(H_val_gain, f);

    y += 30;
    fnt(mklabel(wnd, m+8,   y+3, 90, 16, L"Stereo Width:"), f);
    H_sld_stereo  = mkslider(wnd, ID_SLD_STEREO, m+102, y,    236, 0, 100, 50);
    H_val_stereo  = mklabel (wnd, m+346, y+3, 44, 16, L"50");
    fnt(H_val_stereo, f);

    y += 30;
    fnt(mklabel(wnd, m+8,   y+3, 90, 16, L"Audio Path:"), f);
    H_edt_path   = mkedit  (wnd, ID_EDT_PATH,   m+102, y,    214);
    H_btn_browse = mkbutton(wnd, ID_BTN_BROWSE, m+322, y-1,   78, 25, L"Browse...");
    fnt(H_edt_path,   f);
    fnt(H_btn_browse, f);

    y += 30;
    fnt(mklabel(wnd, m+8, y+3, 90, 16, L"Device:"), f);
    H_edt_device = mkedit(wnd, ID_EDT_DEVICE, m+102, y, 298);
    SendMessageW(H_edt_device, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"(leave blank for default device)");
    fnt(H_edt_device, f);

    /* Options group ─────────────────────────────────────────────── */
    HWND g2 = mkgroup(wnd, m, m+162, gw, 122, L" Options ");
    fnt(g2, f);

    y = m + 162 + 20;
    H_chk_muted      = mkcheck(wnd, ID_CHK_MUTED,      m+8, y,    200, L"Start muted");
    H_chk_noclick    = mkcheck(wnd, ID_CHK_NOCLICK,    m+8, y+24, 200, L"No mouse click sounds");
    H_chk_fallback   = mkcheck(wnd, ID_CHK_FALLBACK,   m+8, y+48, 230, L"Fallback sound for unknown keys");
    H_chk_autolaunch = mkcheck(wnd, ID_CHK_AUTOLAUNCH, m+8, y+72, 230, L"Start buckle when GUI opens");
    fnt(H_chk_muted,      f);
    fnt(H_chk_noclick,    f);
    fnt(H_chk_fallback,   f);
    fnt(H_chk_autolaunch, f);

    fnt(mklabel(wnd, m+244, y+3, 68, 16, L"Mute key:"), f);
    H_edt_mutekey = mkedit(wnd, ID_EDT_MUTEKEY, m+318, y, 82);
    fnt(H_edt_mutekey, f);
    fnt(mklabel(wnd, m+244, y+27, 160, 32,
        L"Hex scancode\n(0x46 = Scroll Lock)"), f);

    /* Start / Stop / Autostart buttons ─────────────────────────── */
    y = m + 294;
    H_btn_start     = mkbutton(wnd, ID_BTN_START,     m,     y, 100, 30, L"Start");
    H_btn_stop      = mkbutton(wnd, ID_BTN_STOP,      m+110, y, 100, 30, L"Stop");
    H_btn_autostart = mkbutton(wnd, ID_BTN_AUTOSTART, m+220, y, 120, 30, L"Autostart: OFF");
    fnt(H_btn_start,     f);
    fnt(H_btn_stop,      f);
    fnt(H_btn_autostart, f);
    EnableWindow(H_btn_stop, FALSE);

    H_lbl_status = mklabel(wnd, m+350, y+7, 50, 18, L"Stopped");
    fnt(H_lbl_status, f);

    /* Log group ─────────────────────────────────────────────────── */
    HWND g3 = mkgroup(wnd, m, m+334, gw, 156, L" Log ");
    fnt(g3, f);

    H_edt_log = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        m+8, m+350, gw-16-80, 128,
        wnd, (HMENU)(INT_PTR)ID_EDT_LOG, g_inst, NULL);
    SendMessageW(H_edt_log, WM_SETFONT,
        (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);

    H_btn_clearlog = mkbutton(wnd, ID_BTN_CLEARLOG,
        m+gw-72, m+350, 64, 22, L"Clear");
    fnt(H_btn_clearlog, f);

    log_append(L"[gui] Ready. Press Start to launch buckle.\r\n");
}

/* ── process management ──────────────────────────────────────────────── */
static void build_cmdline(WCHAR *out, int outlen)
{
    WCHAR path[MAX_PATH] = {0};
    WCHAR dev[256]       = {0};
    WCHAR key[32]        = {0};
    WCHAR exe[MAX_PATH]  = {0};

    GetWindowTextW(H_edt_path,    path, MAX_PATH);
    GetWindowTextW(H_edt_device,  dev,  256);
    GetWindowTextW(H_edt_mutekey, key,  32);

    WCHAR *kp = key;
    while (*kp == L' ') kp++;

    _snwprintf(exe, MAX_PATH, L"%sbuckle.exe", g_dir);

    int  gain   = (int)SendMessageW(H_sld_gain,    TBM_GETPOS, 0, 0);
    int  stereo = (int)SendMessageW(H_sld_stereo,  TBM_GETPOS, 0, 0);
    BOOL bmut   = SendMessageW(H_chk_muted,    BM_GETCHECK, 0, 0) == BST_CHECKED;
    BOOL bnoc   = SendMessageW(H_chk_noclick,  BM_GETCHECK, 0, 0) == BST_CHECKED;
    BOOL bfbk   = SendMessageW(H_chk_fallback, BM_GETCHECK, 0, 0) == BST_CHECKED;

    _snwprintf(out, outlen,
        L"\"%ls\" -g %d -s %d -p \"%ls\"%ls%ls%ls",
        exe, gain, stereo, path,
        bmut ? L" -M" : L"",
        bnoc ? L" -c" : L"",
        bfbk ? L" -f" : L"");

    WCHAR tmp[2048];

    if (dev[0]) {
        _snwprintf(tmp, 2048, L"%ls -d \"%ls\"", out, dev);
        _snwprintf(out, outlen, L"%ls", tmp);
    }

    if (kp[0] && wcscmp(kp, L"0x46") != 0) {
        _snwprintf(tmp, 2048, L"%ls -m %ls", out, kp);
        _snwprintf(out, outlen, L"%ls", tmp);
    }
}

static void start_buckle(void)
{
    if (g_running) return;

    WCHAR exe[MAX_PATH];
    _snwprintf(exe, MAX_PATH, L"%sbuckle.exe", g_dir);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        WCHAR errmsg[MAX_PATH + 64];
        _snwprintf(errmsg, MAX_PATH + 64,
            L"[gui] ERROR: buckle.exe not found at:\r\n[gui]   %ls\r\n", exe);
        log_append(errmsg);
        return;
    }

    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&g_hReadPipe, &g_hWritePipe, &sa, 0)) {
        log_append(L"[gui] ERROR: failed to create pipe.\r\n");
        return;
    }
    SetHandleInformation(g_hReadPipe, HANDLE_FLAG_INHERIT, 0);

    WCHAR cmd[2048] = {0};
    build_cmdline(cmd, 2048);

    WCHAR logline[2200];
    _snwprintf(logline, 2200, L"[gui] Launching: %ls\r\n", cmd);
    log_append(logline);

    STARTUPINFOW si = {0};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = g_hWritePipe;
    si.hStdError  = g_hWritePipe;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, g_dir, &si, &g_pi)) {
        WCHAR errmsg[512];
        _snwprintf(errmsg, 512,
            L"[gui] ERROR: CreateProcess failed (code %lu).\r\n",
            GetLastError());
        log_append(errmsg);
        CloseHandle(g_hReadPipe);  g_hReadPipe  = NULL;
        CloseHandle(g_hWritePipe); g_hWritePipe = NULL;
        return;
    }

    CloseHandle(g_hWritePipe);
    g_hWritePipe = NULL;

    g_hLogThread = CreateThread(NULL, 0, log_reader_thread, NULL, 0, NULL);

    g_running = TRUE;
    update_status();
}

static void stop_buckle(void)
{
    if (!g_running || !g_pi.hProcess) return;

    /* buckle has no open files or transactions — terminate is safe here */
    TerminateProcess(g_pi.hProcess, 0);
    WaitForSingleObject(g_pi.hProcess, 3000);
    CloseHandle(g_pi.hProcess);
    CloseHandle(g_pi.hThread);
    ZeroMemory(&g_pi, sizeof(g_pi));

    if (g_hReadPipe) {
        CloseHandle(g_hReadPipe);
        g_hReadPipe = NULL;
    }

    if (g_hLogThread) {
        WaitForSingleObject(g_hLogThread, 2000);
        CloseHandle(g_hLogThread);
        g_hLogThread = NULL;
    }

    g_running = FALSE;
    update_status();
    log_append(L"[gui] Stopped.\r\n");
}

static void update_status(void)
{
    if (g_running) {
        SetWindowTextW(H_lbl_status, L"Running");
        EnableWindow(H_btn_start, FALSE);
        EnableWindow(H_btn_stop,  TRUE);
    } else {
        SetWindowTextW(H_lbl_status, L"Stopped");
        EnableWindow(H_btn_start, TRUE);
        EnableWindow(H_btn_stop,  FALSE);
    }
    _snwprintf(g_nid.szTip, 128, L"Bucklespring \u2014 %ls",
               g_running ? L"Running" : L"Stopped");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

/* ── slider label sync ───────────────────────────────────────────────── */
static void update_slider_label(HWND slider, HWND label)
{
    WCHAR buf[8];
    _snwprintf(buf, 8, L"%d", (int)SendMessageW(slider, TBM_GETPOS, 0, 0));
    SetWindowTextW(label, buf);
}

/* ── folder browser ──────────────────────────────────────────────────── */
static void browse_path(void)
{
    WCHAR path[MAX_PATH] = {0};
    BROWSEINFOW bi = {0};
    bi.hwndOwner      = g_wnd;
    bi.pszDisplayName = path;
    bi.lpszTitle      = L"Select the audio (wav) folder";
    bi.ulFlags        = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, path))
            SetWindowTextW(H_edt_path, path);
        CoTaskMemFree(pidl);
    }
}

/* ── system tray ─────────────────────────────────────────────────────── */
static void add_tray(void)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_wnd;
    g_nid.uID              = TRAY_UID;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = (HICON)LoadImageW(g_inst,
                                MAKEINTRESOURCEW(IDI_MYICON),
                                IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wcscpy(g_nid.szTip, L"Bucklespring \u2014 Stopped");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void remove_tray(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void show_tray_menu(void)
{
    HMENU hm = CreatePopupMenu();
    AppendMenuW(hm, MF_STRING,                                IDM_SHOW, L"Show / Hide");
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING | (g_running ? 0 : MF_GRAYED), IDM_MUTE,
        L"Toggle Mute (Scroll Lock \u00d72)");
    AppendMenuW(hm, MF_STRING | (g_running ? 0 : MF_GRAYED), IDM_STOP, L"Stop Buckle");
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING,                                IDM_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(hm, TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
                   pt.x, pt.y, 0, g_wnd, NULL);
    DestroyMenu(hm);
}

/* ── mute via simulated Scroll Lock ×2 ──────────────────────────────── */
static void send_mute_sequence(void)
{
    INPUT keys[4];
    ZeroMemory(keys, sizeof(keys));
    keys[0].type       = INPUT_KEYBOARD;
    keys[0].ki.wVk     = VK_SCROLL;
    keys[1]            = keys[0];
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    keys[2]            = keys[0];
    keys[3]            = keys[1];
    SendInput(4, keys, sizeof(INPUT));
}

/* ── settings (INI) ──────────────────────────────────────────────────── */
#define _W(k, v)  WritePrivateProfileStringW(L"S", k, v, g_ini)
#define _WI(k, n) _snwprintf(_buf, MAX_PATH, L"%d", (int)(n)); _W(k, _buf)

static void save_settings(void)
{
    WCHAR _buf[MAX_PATH];

    GetWindowTextW(H_edt_path,    _buf, MAX_PATH); _W(L"AudioPath",   _buf);
    GetWindowTextW(H_edt_device,  _buf, 256);      _W(L"Device",      _buf);
    GetWindowTextW(H_edt_mutekey, _buf, 32);       _W(L"MuteKey",     _buf);
    _WI(L"Gain",        SendMessageW(H_sld_gain,    TBM_GETPOS, 0, 0));
    _WI(L"StereoWidth", SendMessageW(H_sld_stereo,  TBM_GETPOS, 0, 0));
    _WI(L"Muted",      SendMessageW(H_chk_muted,      BM_GETCHECK, 0, 0) == BST_CHECKED);
    _WI(L"NoClick",    SendMessageW(H_chk_noclick,    BM_GETCHECK, 0, 0) == BST_CHECKED);
    _WI(L"Fallback",   SendMessageW(H_chk_fallback,   BM_GETCHECK, 0, 0) == BST_CHECKED);
    _WI(L"AutoLaunch", SendMessageW(H_chk_autolaunch, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

#undef _W
#undef _WI

static void load_settings(void)
{
    WCHAR buf[MAX_PATH];
    WCHAR def_wav[MAX_PATH];
    _snwprintf(def_wav, MAX_PATH, L"%swav", g_dir);

#define _R(k, def, w) \
    GetPrivateProfileStringW(L"S", k, def, buf, w, g_ini)
#define _RI(k, def) \
    (int)GetPrivateProfileIntW(L"S", k, def, g_ini)

    _R(L"AudioPath", def_wav, MAX_PATH); SetWindowTextW(H_edt_path,    buf);
    _R(L"Device",    L"",     256);      SetWindowTextW(H_edt_device,  buf);
    _R(L"MuteKey",   L"0x46", 32);       SetWindowTextW(H_edt_mutekey, buf);

    SendMessageW(H_sld_gain,   TBM_SETPOS, TRUE, _RI(L"Gain",        100));
    SendMessageW(H_sld_stereo, TBM_SETPOS, TRUE, _RI(L"StereoWidth",  50));
    update_slider_label(H_sld_gain,   H_val_gain);
    update_slider_label(H_sld_stereo, H_val_stereo);

    SendMessageW(H_chk_muted,    BM_SETCHECK,
        _RI(L"Muted",      0) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(H_chk_noclick,  BM_SETCHECK,
        _RI(L"NoClick",    0) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(H_chk_fallback, BM_SETCHECK,
        _RI(L"Fallback",   0) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(H_chk_autolaunch, BM_SETCHECK,
        _RI(L"AutoLaunch", 0) ? BST_CHECKED : BST_UNCHECKED, 0);

#undef _R
#undef _RI
}
