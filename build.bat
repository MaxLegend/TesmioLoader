@echo off
setlocal
rem The hard-coded path is one machine's install; when it misses, ask the
rem Visual Studio installer where a toolset actually lives before giving up.
rem %ProgramFiles(x86)% can be unset in a shell that cannot pass a name with
rem parentheses through its environment (Git Bash drops it), so fall back to
rem the location the installer has used since forever.
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
set PF86=%ProgramFiles(x86)%
if not defined PF86 set PF86=C:\Program Files (x86)
set VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VCVARS%" if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat
    )
)
if not exist "%VCVARS%" (
    echo [build] vcvars64.bat not found - is Visual Studio with the C++ toolset installed?
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 ( echo [build] vcvars failed & exit /b 1 )

cd /d "%~dp0"
if not exist build mkdir build

echo [build] tesmioloader.dll
cl /nologo /O2 /MT /W3 /EHsc /LD ^
   /Fo"build\\" /Fd"build\\" /Fe"build\tesmioloader.dll" ^
   src\tesmioloader.cpp /link kernel32.lib
if errorlevel 1 ( echo [build] tesmioloader.dll FAILED & exit /b 1 )

rem A windowed program: wWinMain, and /SUBSYSTEM:WINDOWS so double-clicking it
rem does not open a console behind the window. It attaches to the parent console
rem when there is one, so --nogui from a terminal still prints.
rem
rem /MANIFEST:EMBED because the source declares a MANIFESTDEPENDENCY on
rem Microsoft.Windows.Common-Controls v6 for themed checkboxes, and link.exe
rem otherwise leaves it in a tesmiolauncher.exe.manifest beside the exe - which
rem works only as long as nobody copies the exe on its own.
rem The application icon, compiled separately and handed to the linker like an
rem object file. rc.exe ships with the Windows SDK and vcvars64 puts it on PATH;
rem if it is missing the launcher is still built, just without an icon, because
rem an icon is not worth failing a build over.
echo [build] tesmiolauncher.exe
if exist build\tesmiolauncher.res del build\tesmiolauncher.res
rc /nologo /fo "build\tesmiolauncher.res" src\tesmiolauncher.rc >nul 2>&1
if exist build\tesmiolauncher.res (
    set LAUNCHER_RES=build\tesmiolauncher.res
) else (
    set LAUNCHER_RES=
    echo [build] rc.exe unavailable - launcher built without its icon
)
cl /nologo /O2 /MT /W3 /EHsc ^
   /Fo"build\\" /Fd"build\\" /Fe"build\tesmiolauncher.exe" ^
   src\tesmiolauncher.cpp %LAUNCHER_RES% /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED kernel32.lib
if errorlevel 1 ( echo [build] tesmiolauncher.exe FAILED & exit /b 1 )
if exist build\tesmiolauncher.exe.manifest del build\tesmiolauncher.exe.manifest

rem The signing tool. It only ever runs here, offline: the launcher embeds the
rem public half of the key and verifies, this is the half that touches the
rem private one. bcrypt.lib comes in through a pragma in tesmio_sign.h.
echo [build] tsmsign.exe
cl /nologo /O2 /MT /W3 /EHsc ^
   /Fo"build\\" /Fd"build\\" /Fe"build\tsmsign.exe" ^
   tools\signing\tsmsign.cpp /link kernel32.lib
if errorlevel 1 ( echo [build] tsmsign.exe FAILED & exit /b 1 )

rem Plugins. One folder per plugin under plugins\, each holding <name>.cpp and
rem optionally <name>.ini; both land in build\plugins\, which is what the loader
rem scans. Adding a plugin is adding a folder - nothing here lists them.
if not exist build\plugins mkdir build\plugins

for /d %%P in (plugins\*) do (
    if exist "%%P\%%~nxP.cpp" (
        echo [build] plugins\%%~nxP.dll
        cl /nologo /O2 /MT /W3 /EHsc /LD ^
           /Fo"build\plugins\\" /Fd"build\plugins\\" /Fe"build\plugins\%%~nxP.dll" ^
           "%%P\%%~nxP.cpp" /link kernel32.lib
        if errorlevel 1 ( echo [build] plugins\%%~nxP.dll FAILED & exit /b 1 )
        if exist "%%P\%%~nxP.ini" copy /y "%%P\%%~nxP.ini" "build\plugins\%%~nxP.ini" >nul
    ) else (
        echo [build] plugins\%%~nxP: no %%~nxP.cpp, skipped
    )
)

rem Signing. A rebuilt DLL invalidates its old .dll.sig, so every plugin is
rem re-signed on every build the private key is present for. Without the key
rem there is nothing to do - the launcher then marks the plugins "not from
rem Tesmio", which for a third-party build from source is simply the truth.
rem The key file is the one thing a release must never ship.
if exist tools\signing\tsm_private.bin (
    for %%D in (build\plugins\*.dll) do (
        build\tsmsign.exe sign tools\signing\tsm_private.bin "%%D" >nul
        if errorlevel 1 ( echo [build] signing %%~nxD FAILED & exit /b 1 )
    )
    echo [build] plugins signed with the Tesmio key
) else (
    echo [build] no tools\signing\tsm_private.bin - plugins left unsigned
)

rem build\tesmioloader.ini is the live config: the launcher writes the plugin
rem checkboxes and the game path into it, so overwriting it on every build would
rem throw away what the user chose. Copied only when it is not there yet - to
rem pick up new defaults from the repo copy, delete it and build again.
if not exist build\tesmioloader.ini (
    copy /y tesmioloader.ini build\tesmioloader.ini >nul
) else (
    echo [build] build\tesmioloader.ini kept - delete it to take the repo defaults
)
echo [build] ok -^> build\tesmiolauncher.exe
endlocal
