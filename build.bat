@echo off
setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
    echo [build] vcvars64.bat not found at "%VCVARS%"
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

rem build\tesmioloader.ini is the live config: the launcher writes the plugin
rem checkboxes and the game path into it, so overwriting it on every build would
rem throw away what the user chose. Copied only when it is not there yet - to
rem pick up new defaults from the repo copy, delete it and build again.
if not exist build\tesmioloader.ini (
    copy /y tesmioloader.ini build\tesmioloader.ini >nul
) else (
    echo [build] build\tesmioloader.ini kept - delete it to take the repo defaults
)
if not exist build\vfs mklink /J build\vfs vfs >nul 2>&1

echo [build] ok -^> build\tesmiolauncher.exe
endlocal
