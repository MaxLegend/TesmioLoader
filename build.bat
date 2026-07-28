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

echo [build] tesmiolauncher.exe
cl /nologo /O2 /MT /W3 /EHsc ^
   /Fo"build\\" /Fd"build\\" /Fe"build\tesmiolauncher.exe" ^
   src\tesmiolauncher.cpp /link kernel32.lib
if errorlevel 1 ( echo [build] tesmiolauncher.exe FAILED & exit /b 1 )

copy /y tesmioloader.ini build\tesmioloader.ini >nul
if exist resources.ini copy /y resources.ini build\resources.ini >nul
if exist deposits.ini copy /y deposits.ini build\deposits.ini >nul
if not exist build\vfs mklink /J build\vfs vfs >nul 2>&1

echo [build] ok -^> build\tesmiolauncher.exe
endlocal
