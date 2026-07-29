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
   src\tesmioloader.cpp src\tesmioloader.def /link kernel32.lib /NOIMPLIB /NOEXP
if errorlevel 1 ( echo [build] tesmioloader.dll FAILED & exit /b 1 )

echo [build] tesmiolauncher.exe
cl /nologo /O2 /MT /W3 /EHsc ^
   /Fo"build\\" /Fd"build\\" /Fe"build\tesmiolauncher.exe" ^
   src\tesmiolauncher.cpp /link kernel32.lib
if errorlevel 1 ( echo [build] tesmiolauncher.exe FAILED & exit /b 1 )

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

copy /y tesmioloader.ini build\tesmioloader.ini >nul
if not exist build\vfs mklink /J build\vfs vfs >nul 2>&1

echo [build] ok -^> build\tesmiolauncher.exe
endlocal
