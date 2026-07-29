// tesmiolauncher - starts the game with tesmioloader.dll injected.
//
// The game is created suspended, the DLL is loaded into it via a remote thread,
// and only then is the main thread released. Injecting before the game runs a
// single instruction means the import table is already resolved (the loader runs
// on our remote thread) while no game code has executed yet, so every file the
// game opens passes through the gate.
//
// Nothing in the game folder is modified. Steam's file verification stays happy.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static void Fail(const wchar_t* what)
{
    DWORD e = GetLastError();
    wchar_t* msg = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, e, 0, (wchar_t*)&msg, 0, NULL);
    fwprintf(stderr, L"[tesmiolauncher] %s failed (%lu): %s\n", what, e, msg ? msg : L"?");
    if (msg) LocalFree(msg);
}

static bool Exists(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int wmain(int argc, wchar_t** argv)
{
    wchar_t selfDir[MAX_PATH];
    GetModuleFileNameW(NULL, selfDir, MAX_PATH);
    if (wchar_t* s = wcsrchr(selfDir, L'\\')) *s = 0;

    wchar_t game[MAX_PATH] = {0};
    wchar_t dll[MAX_PATH]  = {0};

    for (int i = 1; i < argc; i++)
    {
        if (!_wcsicmp(argv[i], L"--game") && i + 1 < argc) wcscpy_s(game, MAX_PATH, argv[++i]);
        else if (!_wcsicmp(argv[i], L"--dll") && i + 1 < argc) wcscpy_s(dll, MAX_PATH, argv[++i]);
        else if (!_wcsicmp(argv[i], L"--help"))
        {
            wprintf(L"usage: tesmiolauncher [--game <SOVIET64.exe>] [--dll <tesmioloader.dll>]\n");
            return 0;
        }
    }

    bool gameSpecified = game[0] != 0;

    // Defaults assume this project lives in <game>\tesmioloader\build\.
    if (!game[0]) swprintf_s(game, MAX_PATH, L"%s\\..\\..\\SOVIET64.exe", selfDir);
    if (!dll[0])  swprintf_s(dll,  MAX_PATH, L"%s\\tesmioloader.dll", selfDir);

    wchar_t gameFull[MAX_PATH], dllFull[MAX_PATH];
    if (!GetFullPathNameW(game, MAX_PATH, gameFull, NULL)) { Fail(L"GetFullPathName(game)"); return 1; }
    if (!GetFullPathNameW(dll,  MAX_PATH, dllFull,  NULL)) { Fail(L"GetFullPathName(dll)");  return 1; }

    if (!gameSpecified && !Exists(gameFull))
    {
        wchar_t currentGame[MAX_PATH];
        if (!GetFullPathNameW(L"SOVIET64.exe", MAX_PATH, currentGame, NULL))
        {
            Fail(L"GetFullPathName(current game)");
            return 1;
        }
        if (Exists(currentGame)) wcscpy_s(gameFull, MAX_PATH, currentGame);
    }

    if (!Exists(gameFull)) { fwprintf(stderr, L"[tesmiolauncher] game not found: %s\n", gameFull); return 1; }
    if (!Exists(dllFull))  { fwprintf(stderr, L"[tesmiolauncher] dll not found: %s\n",  dllFull);  return 1; }

    wchar_t workDir[MAX_PATH];
    wcscpy_s(workDir, MAX_PATH, gameFull);
    if (wchar_t* s = wcsrchr(workDir, L'\\')) *s = 0;

    wprintf(L"[tesmiolauncher] game %s\n", gameFull);
    wprintf(L"[tesmiolauncher] dll  %s\n", dllFull);
    wprintf(L"[tesmiolauncher] cwd  %s\n", workDir);

    wchar_t cmdline[MAX_PATH + 4];
    swprintf_s(cmdline, MAX_PATH + 4, L"\"%s\"", gameFull);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessW(gameFull, cmdline, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, workDir, &si, &pi))
    {
        Fail(L"CreateProcess");
        return 1;
    }
    wprintf(L"[tesmiolauncher] pid %lu created suspended\n", pi.dwProcessId);

    int rc = 1;
    SIZE_T bytes = (wcslen(dllFull) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(pi.hProcess, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!remote) { Fail(L"VirtualAllocEx"); goto cleanup; }

    if (!WriteProcessMemory(pi.hProcess, remote, dllFull, bytes, NULL))
    {
        Fail(L"WriteProcessMemory");
        goto cleanup;
    }

    {
        // kernel32 sits at the same base in every process of a boot session,
        // so our own LoadLibraryW address is valid in the target.
        FARPROC loadLib = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        if (!loadLib) { Fail(L"GetProcAddress(LoadLibraryW)"); goto cleanup; }

        HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0,
                                       (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
        if (!th) { Fail(L"CreateRemoteThread"); goto cleanup; }

        WaitForSingleObject(th, 30000);

        DWORD loaded = 0;
        GetExitCodeThread(th, &loaded);
        CloseHandle(th);

        if (!loaded)
        {
            fwprintf(stderr, L"[tesmiolauncher] LoadLibraryW returned NULL - the DLL did not load\n");
            goto cleanup;
        }
        wprintf(L"[tesmiolauncher] tesmioloader.dll loaded\n");
    }

    if (ResumeThread(pi.hThread) == (DWORD)-1) { Fail(L"ResumeThread"); goto cleanup; }
    wprintf(L"[tesmiolauncher] game resumed - see tesmioloader\\tesmioloader.log\n");
    rc = 0;

cleanup:
    if (remote) VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    if (rc != 0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return rc;
}
