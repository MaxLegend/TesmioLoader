// tesmiolauncher - starts the game with tesmioloader.dll injected.
//
// The game is created suspended, the DLL is loaded into it via a remote thread,
// and only then is the main thread released. Injecting before the game runs a
// single instruction means the import table is already resolved (the loader runs
// on our remote thread) while no game code has executed yet, so every file the
// game opens passes through the gate.
//
// Nothing in the game folder is modified. Steam's file verification stays happy.
//
// Two things sit around that injection.
//
// FINDING THE GAME. The old default was a single path, <self>\..\..\SOVIET64.exe,
// which is right only when tesmioloader\build\ is inside the game folder. Four
// strategies now run in order - an explicit --game, the path the window last
// saved into tesmioloader.ini, a walk up the tree from the launcher looking one
// folder deep at every level, and Steam's own library list via the registry and
// appmanifest_784150.acf. A folder is only believed to be the install when
// SOVIET64.exe is in it beside C3DDLL64.dll or media_soviet: the exe name alone
// would also match a stray copy in someone's Downloads, and injecting into that
// fails in a way nobody can read.
//
// THE WINDOW. One checkbox per DLL in plugins\, written to the [plugins] section
// of tesmioloader.ini. The loader reads that section per DLL, so unchecking one
// leaves the file where it is and simply never loads it - nothing is renamed or
// deleted, which is what makes the choice reversible from either side. --nogui
// skips the window entirely and behaves exactly as this program did before.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>           // WIN32_LEAN_AND_MEAN leaves this one out
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

// Themed controls without a resource script: build.bat compiles one .cpp per
// binary and a .rc would put a second tool in that chain. This is the same
// dependency an .rc-embedded manifest would declare.
#pragma comment(linker, "\"/MANIFESTDEPENDENCY:type='win32' "                  \
                        "name='Microsoft.Windows.Common-Controls' "            \
                        "version='6.0.0.0' processorArchitecture='*' "         \
                        "publicKeyToken='6595b64144ccf1df' language='*'\"")

#define GAME_EXE        L"SOVIET64.exe"
#define GAME_ENGINE     L"C3DDLL64.dll"     // ships with the game and nothing else
#define GAME_MEDIA      L"media_soviet"
#define STEAM_APPID     L"784150"
#define LOADER_DLL      L"tesmioloader.dll"
#define LOADER_INI      L"tesmioloader.ini"

#define MAX_PLUGINS     32                  // mirrors the loader's own cap
#define WALK_UP_LEVELS  8

// ---------------------------------------------------------------- messages
//
// Everything this program has to say goes through Msg: to the console when one
// is attached, to the status line when the window is up, and always into a
// buffer, because a failure with the window open has to be readable after the
// fact rather than only in a console nobody sees.

static bool    g_console;
static HWND    g_status;
static wchar_t g_log[8192];

// A /SUBSYSTEM:WINDOWS process gets no console of its own, which is the point -
// double-clicking it must not open one behind the window. Two ways it can still
// have somewhere to print, and the order matters:
//
//   an inherited handle, when the caller redirected stdout to a pipe or a file.
//   The CRT has already wired the streams to it and reopening CONOUT$ would
//   write past the redirection to a console the caller is not reading.
//
//   the parent's console, when it was started from a terminal with no
//   redirection. Nothing is inherited, so the streams have to be pointed at it.
static void OpenConsole()
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) { g_console = true; return; }

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

    FILE* f = NULL;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    g_console = true;
}

static void Msg(const wchar_t* fmt, ...)
{
    wchar_t line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(line, _countof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    if (g_console) fwprintf(stdout, L"[tesmiolauncher] %s\n", line);

    size_t used = wcslen(g_log);
    _snwprintf_s(g_log + used, _countof(g_log) - used, _TRUNCATE, L"%s\r\n", line);

    if (g_status) SetWindowTextW(g_status, line);
}

static void Fail(const wchar_t* what)
{
    DWORD e = GetLastError();
    wchar_t* text = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, e, 0, (wchar_t*)&text, 0, NULL);
    if (text) for (wchar_t* p = text; *p; p++) if (*p == L'\r' || *p == L'\n') *p = L' ';
    Msg(L"%s failed (%lu): %s", what, e, text ? text : L"?");
    if (text) LocalFree(text);
}

// ---------------------------------------------------------------- paths

static bool Exists(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExists(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void Join(wchar_t* out, size_t n, const wchar_t* dir, const wchar_t* leaf)
{
    size_t len = wcslen(dir);
    bool slash = len > 0 && (dir[len - 1] == L'\\' || dir[len - 1] == L'/');
    _snwprintf_s(out, n, _TRUNCATE, slash ? L"%s%s" : L"%s\\%s", dir, leaf);
}

// Strips the last component in place. False when there was nothing left to
// strip, which is what ends the walk up the tree. The root keeps its slash and
// is reported as a successful step, so it gets looked at once before the walk
// stops - a game unpacked straight into D:\ is unusual but not impossible.
static bool ToParent(wchar_t* p)
{
    wchar_t* s = wcsrchr(p, L'\\');
    if (!s) return false;

    if (s == p || (s == p + 2 && p[1] == L':'))      // "\x" or "C:\x"
    {
        if (!s[1]) return false;                     // already "\" or "C:\"
        s[1] = 0;
        return true;
    }

    *s = 0;
    return true;
}

// ---------------------------------------------------------------- finding the game

// SOVIET64.exe beside something only the install has. Two acceptable witnesses
// because a Steam verify can be mid-flight and because ModelViewer users
// sometimes have a folder with the exe and no media.
static bool IsGameDir(const wchar_t* dir, wchar_t* outExe, size_t n)
{
    wchar_t exe[MAX_PATH], witness[MAX_PATH];
    Join(exe, _countof(exe), dir, GAME_EXE);
    if (!Exists(exe)) return false;

    Join(witness, _countof(witness), dir, GAME_ENGINE);
    if (!Exists(witness))
    {
        Join(witness, _countof(witness), dir, GAME_MEDIA);
        if (!DirExists(witness)) return false;
    }

    wcscpy_s(outExe, n, exe);
    return true;
}

// One level down, every child. The case this catches is the folder having been
// put in the wrong place rather than named oddly - tesmioloader next to
// SovietRepublic instead of inside it - so one level is enough and a deep
// recursive scan would only be a way to walk someone's whole drive.
static bool FindInChildren(const wchar_t* dir, wchar_t* out, size_t n)
{
    wchar_t pattern[MAX_PATH];
    Join(pattern, _countof(pattern), dir, L"*");

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.cFileName[0] == L'.') continue;

        wchar_t child[MAX_PATH];
        Join(child, _countof(child), dir, fd.cFileName);
        if (IsGameDir(child, out, n)) { found = true; break; }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return found;
}

static bool FindNearSelf(const wchar_t* selfDir, wchar_t* out, size_t n)
{
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, _countof(dir), selfDir);

    for (int up = 0; up <= WALK_UP_LEVELS; up++)
    {
        if (IsGameDir(dir, out, n))      return true;
        if (FindInChildren(dir, out, n)) return true;
        if (!ToParent(dir))              break;
    }
    return false;
}

// --- Steam ---------------------------------------------------------------

static char* ReadAll(const wchar_t* path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > (4u << 20)) { CloseHandle(h); return NULL; }

    char* buf = (char*)malloc(size + 1);
    DWORD got = 0;
    if (!buf || !ReadFile(h, buf, size, &got, NULL)) { free(buf); CloseHandle(h); return NULL; }

    buf[got] = 0;
    CloseHandle(h);
    return buf;
}

// A vdf line is  "key"<tabs>"value"  and the only escape Steam writes inside a
// quoted token is a doubled backslash, so unescaping is one character wide.
// Returns the text just past the closing quote of the value, so a caller can
// keep scanning for the next one.
static const char* VdfValue(const char* pastKey, char* out, size_t n)
{
    const char* p = pastKey;
    while (*p && *p != '"' && *p != '\n') p++;
    if (*p != '"') return NULL;
    p++;

    size_t w = 0;
    while (*p && *p != '"')
    {
        char c = *p++;
        if (c == '\\' && *p) c = *p++;
        if (w + 1 < n) out[w++] = c;
    }
    if (*p != '"') return NULL;

    out[w] = 0;
    return p + 1;
}

static void Widen(const char* in, wchar_t* out, int n)
{
    if (MultiByteToWideChar(CP_UTF8, 0, in, -1, out, n) == 0)
        MultiByteToWideChar(CP_ACP, 0, in, -1, out, n);
}

// The library holding the game is whichever one has its app manifest, so the
// libraries only have to be enumerated - no guess at the folder name, and
// installdir comes out of Steam's own record of it.
static bool TrySteamLibrary(const wchar_t* lib, wchar_t* out, size_t n)
{
    wchar_t acf[MAX_PATH];
    Join(acf, _countof(acf), lib, L"steamapps\\appmanifest_" STEAM_APPID L".acf");
    if (!Exists(acf)) return false;

    wchar_t common[MAX_PATH];
    Join(common, _countof(common), lib, L"steamapps\\common");

    if (char* text = ReadAll(acf))
    {
        char value[MAX_PATH] = {0};
        if (const char* at = strstr(text, "\"installdir\""))
            VdfValue(at + 12, value, sizeof(value));
        free(text);

        if (value[0])
        {
            wchar_t leaf[MAX_PATH], dir[MAX_PATH];
            Widen(value, leaf, _countof(leaf));
            Join(dir, _countof(dir), common, leaf);
            if (IsGameDir(dir, out, n)) return true;
        }
    }

    // The manifest was there but installdir did not resolve - a renamed folder,
    // or a manifest for a move that never finished. Scan the library instead.
    return FindInChildren(common, out, n);
}

static bool RegString(HKEY root, const wchar_t* key, const wchar_t* value,
                      wchar_t* out, DWORD bytes)
{
    HKEY h;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return false;

    // One character short, so a value stored without its terminator - which the
    // registry permits - still comes back as a string.
    DWORD type = 0, n = bytes - sizeof(wchar_t);
    out[0] = 0;
    LSTATUS rc = RegQueryValueExW(h, value, NULL, &type, (BYTE*)out, &n);
    RegCloseKey(h);

    if (rc != ERROR_SUCCESS || type != REG_SZ) return false;
    out[n / sizeof(wchar_t)] = 0;
    return out[0] != 0;
}

static bool FindViaSteam(wchar_t* out, size_t n)
{
    wchar_t steam[MAX_PATH] = {0};
    if (!RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                   steam, sizeof(steam)) &&
        !RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
                   L"InstallPath", steam, sizeof(steam)) &&
        !RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam",
                   L"InstallPath", steam, sizeof(steam)))
        return false;

    // SteamPath is written with forward slashes and in lower case.
    for (wchar_t* p = steam; *p; p++) if (*p == L'/') *p = L'\\';
    Msg(L"steam at %s", steam);

    if (TrySteamLibrary(steam, out, n)) return true;

    wchar_t vdf[MAX_PATH];
    Join(vdf, _countof(vdf), steam, L"steamapps\\libraryfolders.vdf");
    char* text = ReadAll(vdf);
    if (!text) return false;

    bool found = false;
    const char* p = text;
    while (const char* at = strstr(p, "\"path\""))
    {
        char value[MAX_PATH] = {0};
        p = VdfValue(at + 6, value, sizeof(value));
        if (!p) break;
        if (!value[0]) continue;

        wchar_t lib[MAX_PATH];
        Widen(value, lib, _countof(lib));
        if (TrySteamLibrary(lib, out, n)) { found = true; break; }
    }

    free(text);
    return found;
}

// ---------------------------------------------------------------- config
//
// The ini beside the DLL, which is the one the loader reads: baseDir there is
// the folder tesmioloader.dll lives in, not this program's folder, and --dll can
// make those different.
//
// The plugin keys go through the ANSI profile API because that is what the
// loader uses, so both sides agree on what a key looks like. game_exe goes
// through the wide one - it is a path, this program is its only reader, and a
// non-ASCII folder name is at least worth trying to keep.

static wchar_t g_ini[MAX_PATH];
static char    g_iniA[MAX_PATH];
static wchar_t g_version[64] = L"";

struct PluginEntry
{
    wchar_t file[64];       // resources.dll
    char    key[64];        // resources - the [plugins] key, and the loader's
    bool    on;
    HWND    box;
};
static PluginEntry g_plug[MAX_PLUGINS];
static int         g_plugCount;
static bool        g_host = true;       // [tesmioloader] plugins

static void SetIniPath(const wchar_t* dllDir)
{
    Join(g_ini, _countof(g_ini), dllDir, LOADER_INI);
    WideCharToMultiByte(CP_ACP, 0, g_ini, -1, g_iniA, sizeof(g_iniA), NULL, NULL);
}

// Alphabetical, which is what FindFirstFile gives on NTFS and the order the
// loader will load them in.
static void ScanPlugins(const wchar_t* dllDir)
{
    wchar_t pattern[MAX_PATH];
    Join(pattern, _countof(pattern), dllDir, L"plugins\\*.dll");

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_plugCount >= MAX_PLUGINS) break;

        PluginEntry* e = &g_plug[g_plugCount++];
        memset(e, 0, sizeof(*e));
        wcscpy_s(e->file, _countof(e->file), fd.cFileName);

        wchar_t stem[64];
        wcscpy_s(stem, _countof(stem), fd.cFileName);
        if (wchar_t* dot = wcsrchr(stem, L'.')) *dot = 0;
        WideCharToMultiByte(CP_ACP, 0, stem, -1, e->key, sizeof(e->key), NULL, NULL);

        e->on = GetPrivateProfileIntA("plugins", e->key, 1, g_iniA) != 0;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

static void ReadConfig()
{
    g_host = GetPrivateProfileIntA("tesmioloader", "plugins", 1, g_iniA) != 0;
    GetPrivateProfileStringW(L"tesmioloader", L"version", L"unknown", 
                             g_version, _countof(g_version), g_ini);
}

// Creates the ini when it is not there. Everything the loader does not find in
// it falls back to the same compiled default it would have used with no file at
// all, so a file holding only these keys behaves as before - and the game path
// is remembered even on an install that arrived without an ini.
//
// The profile API never writes a BOM and edits in place, so the comments in the
// shipped file survive. That matters: writing this file with PowerShell's
// -Encoding UTF8 has broken the whole config once already.
static void SaveConfig(const wchar_t* gameExe)
{
    WritePrivateProfileStringW(L"tesmioloader", L"game_exe", gameExe, g_ini);
    WritePrivateProfileStringA("tesmioloader", "plugins", g_host ? "1" : "0", g_iniA);

    char versionA[64] = {0};
    WideCharToMultiByte(CP_ACP, 0, g_version, -1, versionA, sizeof(versionA), NULL, NULL);
    char menu_tagA[128];
    _snprintf_s(menu_tagA, _countof(menu_tagA), _TRUNCATE, "tesmioloader v. %s", versionA);
    WritePrivateProfileStringA("tsmloader", "menu_tag", menu_tagA, g_iniA);

    for (int i = 0; i < g_plugCount; i++)
        WritePrivateProfileStringA("plugins", g_plug[i].key,
                                   g_plug[i].on ? "1" : "0", g_iniA);
}

// ---------------------------------------------------------------- injection

static bool Inject(const wchar_t* gameFull, const wchar_t* dllFull)
{
    wchar_t workDir[MAX_PATH];
    wcscpy_s(workDir, _countof(workDir), gameFull);
    ToParent(workDir);

    Msg(L"game %s", gameFull);
    Msg(L"dll  %s", dllFull);
    Msg(L"cwd  %s", workDir);

    wchar_t cmdline[MAX_PATH + 4];
    _snwprintf_s(cmdline, _countof(cmdline), _TRUNCATE, L"\"%s\"", gameFull);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessW(gameFull, cmdline, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, workDir, &si, &pi))
    {
        Fail(L"CreateProcess");
        return false;
    }
    Msg(L"pid %lu created suspended", pi.dwProcessId);

    bool ok = false;
    SIZE_T bytes = (wcslen(dllFull) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(pi.hProcess, NULL, bytes,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!remote)
        Fail(L"VirtualAllocEx");
    else if (!WriteProcessMemory(pi.hProcess, remote, dllFull, bytes, NULL))
        Fail(L"WriteProcessMemory");
    else
    {
        // kernel32 sits at the same base in every process of a boot session,
        // so our own LoadLibraryW address is valid in the target.
        FARPROC loadLib = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        if (!loadLib)
            Fail(L"GetProcAddress(LoadLibraryW)");
        else
        {
            HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0,
                                           (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
            if (!th)
                Fail(L"CreateRemoteThread");
            else
            {
                WaitForSingleObject(th, 30000);

                DWORD loaded = 0;
                GetExitCodeThread(th, &loaded);
                CloseHandle(th);

                if (!loaded)
                    Msg(L"LoadLibraryW returned NULL - the DLL did not load");
                else if (ResumeThread(pi.hThread) == (DWORD)-1)
                    Fail(L"ResumeThread");
                else
                {
                    Msg(L"tesmioloader.dll loaded, game resumed");
                    ok = true;
                }
            }
        }
    }

    if (remote) VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    if (!ok) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

// ---------------------------------------------------------------- the window
//
// Plain Win32, controls created by hand. Metrics are written at 96 dpi and
// scaled through S(), and the process declares itself dpi-aware, so the window
// is crisp on a scaled display rather than bitmap-stretched.

#define IDC_LAUNCH  IDOK            // so IsDialogMessage's Enter lands here
#define IDC_CLOSE   IDCANCEL        // and Escape here
#define IDC_PATH    100
#define IDC_BROWSE  101
#define IDC_HOST    102
#define IDC_STATUS  103
#define IDC_PLUGIN0 200

static HFONT   g_font;
static HWND    g_path;
static HWND    g_hostBox;
static int     g_dpi = 96;
static bool    g_launched;
static wchar_t g_dllFull[MAX_PATH];

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HWND Child(HWND parent, const wchar_t* cls, const wchar_t* text,
                  DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, NULL, NULL);
    if (c && g_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void SyncPluginBoxes()
{
    for (int i = 0; i < g_plugCount; i++)
        EnableWindow(g_plug[i].box, g_host ? TRUE : FALSE);
}

static void Browse(HWND owner)
{
    wchar_t file[MAX_PATH] = {0};
    GetWindowTextW(g_path, file, _countof(file));

    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = L"SOVIET64.exe\0SOVIET64.exe\0Executables\0*.exe\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = _countof(file);
    ofn.lpstrTitle  = L"Where is SOVIET64.exe?";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_path, file);
}

// What the edit box currently names, resolved. A folder is accepted and the exe
// appended, because pasting the game folder is the obvious mistake to forgive.
static bool ResolveEdit(wchar_t* out, size_t n)
{
    wchar_t typed[MAX_PATH] = {0};
    GetWindowTextW(g_path, typed, _countof(typed));

    wchar_t* p = typed;
    while (*p == L' ' || *p == L'"') p++;
    for (wchar_t* e = p + wcslen(p); e > p && (e[-1] == L' ' || e[-1] == L'"'); ) *--e = 0;
    if (!*p) return false;

    if (DirExists(p)) return IsGameDir(p, out, n);

    if (!Exists(p)) return false;
    wcscpy_s(out, n, p);
    return true;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CTLCOLORSTATIC:
        // Statics, group boxes and checkboxes all ask, and all of them sit on
        // the dialog face rather than on a white field.
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_COMMAND:
    {
        int id = LOWORD(wp);

        if (id == IDC_BROWSE) { Browse(hwnd); return 0; }

        if (id == IDC_HOST)
        {
            g_host = SendMessageW(g_hostBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SyncPluginBoxes();
            return 0;
        }

        if (id >= IDC_PLUGIN0 && id < IDC_PLUGIN0 + g_plugCount)
        {
            PluginEntry* e = &g_plug[id - IDC_PLUGIN0];
            e->on = SendMessageW(e->box, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        }

        if (id == IDC_CLOSE) { DestroyWindow(hwnd); return 0; }

        if (id == IDC_LAUNCH)
        {
            wchar_t game[MAX_PATH];
            if (!ResolveEdit(game, _countof(game)))
            {
                MessageBoxW(hwnd,
                            L"That is not a Workers & Resources install.\n\n"
                            L"Point this at SOVIET64.exe, or at the folder holding it.",
                            L"tesmioloader", MB_ICONWARNING | MB_OK);
                SetFocus(g_path);
                return 0;
            }

            SaveConfig(game);

            ShowWindow(hwnd, SW_HIDE);
            if (Inject(game, g_dllFull))
            {
                g_launched = true;
                DestroyWindow(hwnd);
            }
            else
            {
                ShowWindow(hwnd, SW_SHOW);
                MessageBoxW(hwnd, g_log, L"tesmioloader - launch failed",
                            MB_ICONERROR | MB_OK);
            }
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Both of these have to happen before the process touches anything dpi-sensitive
// - the first call that is fixes the awareness for good and SetProcessDPIAware
// then fails - so this runs at the top of wWinMain rather than beside the window
// it is for. Per-monitor v2 when the OS has it, because that is the only mode in
// which a window dragged to a second monitor is redrawn rather than stretched;
// system-wide otherwise.
static void GoDpiAware()
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;

    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
    if (SetCtxFn ctx = (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
        if (ctx((HANDLE)-4))            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            return;

    SetProcessDPIAware();
}

static void DetectDpi()
{
    HDC dc = GetDC(NULL);
    if (!dc) return;
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (dpi >= 96) g_dpi = dpi;
}

static bool ShowWindowUi(const wchar_t* gameFull)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    {
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        {
            // The metrics come back already sized for the system dpi, so only a
            // per-monitor difference is left to apply.
            ncm.lfMessageFont.lfHeight = -MulDiv(9, g_dpi, 72);
            g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        }
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    // UNICODE is not defined here, so MAKEINTRESOURCE in IDC_ARROW is the ANSI one.
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"tesmiolauncher";
    if (gameFull[0]) wc.hIcon = ExtractIconW(wc.hInstance, gameFull, 0);
    if (wc.hIcon == (HICON)1) wc.hIcon = NULL;
    if (!RegisterClassExW(&wc)) { Fail(L"RegisterClassEx"); return false; }

    // 96-dpi layout. The height follows the plugin count rather than scrolling:
    // the loader caps at 32 plugins and six is what ships.
    const int W       = 460;
    const int pad     = 12;
    const int rowH    = 22;
    const int btnW    = 92;
    const int btnH    = 26;
    const int groupH  = 20 + rowH + (g_plugCount ? g_plugCount * 20 : 20) + 10;

    int y = pad;
    const int labelY  = y;                      y += 18;
    const int pathY   = y;                      y += rowH + pad;
    const int groupY  = y;                      y += groupH + pad;
    const int statusY = y;                      y += 16 + pad;
    const int btnY    = y;                      y += btnH + pad;

    RECT rc = { 0, 0, S(W), S(y) };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);

    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;
    int cx = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int cy = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

wchar_t windowTitle[128];
    _snwprintf_s(windowTitle, _countof(windowTitle), _TRUNCATE, 
                 L"tesmioloader v. %s", g_version);

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName,
                                windowTitle, WS_OVERLAPPED | WS_CAPTION |
                                WS_SYSMENU | WS_MINIMIZEBOX,
                                cx, cy, winW, winH, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { Fail(L"CreateWindowEx"); return false; }

    Child(hwnd, L"STATIC", L"Game executable", 0, pad, labelY, W - 2 * pad, 16, 0);

    g_path = Child(hwnd, L"EDIT", gameFull, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                   pad, pathY, W - 2 * pad - btnW - 6, rowH, IDC_PATH);
    Child(hwnd, L"BUTTON", L"Browse...", WS_TABSTOP,
          W - pad - btnW, pathY, btnW, rowH, IDC_BROWSE);

    Child(hwnd, L"BUTTON", L"Plugins", BS_GROUPBOX,
          pad, groupY, W - 2 * pad, groupH, 0);

    g_hostBox = Child(hwnd, L"BUTTON", L"Load plugins at all",
                      BS_AUTOCHECKBOX | WS_TABSTOP,
                      pad + 12, groupY + 18, W - 2 * pad - 24, rowH - 2, IDC_HOST);
    SendMessageW(g_hostBox, BM_SETCHECK, g_host ? BST_CHECKED : BST_UNCHECKED, 0);

    for (int i = 0; i < g_plugCount; i++)
    {
        PluginEntry* e = &g_plug[i];
        e->box = Child(hwnd, L"BUTTON", e->file, BS_AUTOCHECKBOX | WS_TABSTOP,
                       pad + 26, groupY + 18 + rowH + i * 20,
                       W - 2 * pad - 38, 18, IDC_PLUGIN0 + i);
        SendMessageW(e->box, BM_SETCHECK, e->on ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (!g_plugCount)
        Child(hwnd, L"STATIC", L"none found - is build\\plugins\\ there?", 0,
              pad + 26, groupY + 18 + rowH, W - 2 * pad - 38, 18, 0);

    SyncPluginBoxes();

    g_status = Child(hwnd, L"STATIC", gameFull[0] ? L"ready" : L"game not found - browse for it",
                     SS_PATHELLIPSIS, pad, statusY, W - 2 * pad, 16, IDC_STATUS);

    Child(hwnd, L"BUTTON", L"Launch", BS_DEFPUSHBUTTON | WS_TABSTOP,
          W - pad - 2 * btnW - 6, btnY, btnW, btnH, IDC_LAUNCH);
    Child(hwnd, L"BUTTON", L"Close", WS_TABSTOP,
          W - pad - btnW, btnY, btnW, btnH, IDC_CLOSE);

    ShowWindow(hwnd, SW_SHOW);
    SetFocus(gameFull[0] ? GetDlgItem(hwnd, IDC_LAUNCH) : g_path);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        // Gives tab traversal, space on a checkbox, Enter on Launch and Escape
        // on Close without any of it being written here.
        if (!IsDialogMessageW(hwnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }

    g_status = NULL;
    return g_launched;
}

// ---------------------------------------------------------------- entry

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    GoDpiAware();
    DetectDpi();
    OpenConsole();

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    wchar_t selfDir[MAX_PATH];
    GetModuleFileNameW(NULL, selfDir, MAX_PATH);
    ToParent(selfDir);

    wchar_t game[MAX_PATH] = {0};
    wchar_t dll[MAX_PATH]  = {0};
    bool    gui            = true;
    bool    findOnly       = false;

    for (int i = 1; argv && i < argc; i++)
    {
        if (!_wcsicmp(argv[i], L"--game") && i + 1 < argc) wcscpy_s(game, MAX_PATH, argv[++i]);
        else if (!_wcsicmp(argv[i], L"--dll") && i + 1 < argc) wcscpy_s(dll, MAX_PATH, argv[++i]);
        else if (!_wcsicmp(argv[i], L"--nogui")) gui = false;
        else if (!_wcsicmp(argv[i], L"--find")) { findOnly = true; gui = false; }
        else if (!_wcsicmp(argv[i], L"--help"))
        {
            Msg(L"usage: tesmiolauncher [--game <SOVIET64.exe|folder>] "
                L"[--dll <tesmioloader.dll>] [--nogui] [--find]");
            Msg(L"  no --game: the ini's game_exe, then a walk up from here, then Steam");
            Msg(L"  --nogui:   skip the window, launch with whatever is in the ini");
            Msg(L"  --find:    say what the search resolved and exit, changing nothing");
            if (!g_console) MessageBoxW(NULL, g_log, L"tesmiolauncher", MB_OK);
            return 0;
        }
    }

    // --- the DLL. Its folder is what decides which ini both halves read.
    if (!dll[0])
    {
        Join(dll, MAX_PATH, selfDir, LOADER_DLL);
        if (!Exists(dll)) Join(dll, MAX_PATH, selfDir, L"build\\" LOADER_DLL);
    }
    if (!GetFullPathNameW(dll, MAX_PATH, g_dllFull, NULL))
    {
        Fail(L"GetFullPathName(dll)");
        return 1;
    }

    wchar_t dllDir[MAX_PATH];
    wcscpy_s(dllDir, MAX_PATH, g_dllFull);
    ToParent(dllDir);
    SetIniPath(dllDir);
    ReadConfig();
    ScanPlugins(dllDir);

    // --- the game, in order of how much the user meant it.
    wchar_t found[MAX_PATH] = {0};

    if (game[0])
    {
        wchar_t full[MAX_PATH];
        if (GetFullPathNameW(game, MAX_PATH, full, NULL))
        {
            if (DirExists(full))      IsGameDir(full, found, MAX_PATH);
            else if (Exists(full))    wcscpy_s(found, MAX_PATH, full);
        }
        if (!found[0]) Msg(L"--game %s is not there", game);
    }

    if (!found[0])
    {
        wchar_t saved[MAX_PATH] = {0};
        GetPrivateProfileStringW(L"tesmioloader", L"game_exe", L"",
                                 saved, _countof(saved), g_ini);
        if (saved[0] && Exists(saved)) { wcscpy_s(found, MAX_PATH, saved); Msg(L"game_exe from the ini"); }
    }

    if (!found[0] && FindNearSelf(selfDir, found, MAX_PATH))
        Msg(L"found by walking up from %s", selfDir);

    if (!found[0] && FindViaSteam(found, MAX_PATH))
        Msg(L"found through Steam");

    // Nothing written, nothing started - the only way to see what the four
    // strategies resolved to, since the winner is the only one that leaves a
    // trace once the game is running.
    if (findOnly)
    {
        Msg(L"dpi    %d", g_dpi);
        Msg(L"ini    %s", g_ini);
        Msg(L"dll    %s%s", g_dllFull, Exists(g_dllFull) ? L"" : L"   (missing)");
        for (int i = 0; i < g_plugCount; i++)
            Msg(L"plugin %-20s %s", g_plug[i].file, g_plug[i].on ? L"on" : L"off");
        Msg(L"game   %s", found[0] ? found : L"NOT FOUND");
        return found[0] ? 0 : 1;
    }

    if (!Exists(g_dllFull))
    {
        Msg(L"dll not found: %s", g_dllFull);
        if (gui) MessageBoxW(NULL, g_log, L"tesmioloader", MB_ICONERROR | MB_OK);
        return 1;
    }

    if (gui) return ShowWindowUi(found) ? 0 : 1;

    if (!found[0])
    {
        Msg(L"%s not found. Pass --game, or run without --nogui and browse for it.", GAME_EXE);
        return 1;
    }

    SaveConfig(found);
    return Inject(found, g_dllFull) ? 0 : 1;
}
