@echo off
rem btfconvert.exe - its own build, deliberately not part of ..\..\build.bat.
rem
rem That build links tesmioloader.dll, which the running game holds open, so it
rem cannot be run while playing. This tool has nothing to do with the loader and
rem should not inherit that.
rem
rem /MT statically links the CRT: the result is one file with no VC++ redist and
rem no Python behind it, which is the whole point - it gets handed to people.
rem /SUBSYSTEM:WINDOWS so double-clicking opens no console; the exe attaches to
rem the parent console when it has one, so --selftest from a terminal prints.
setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
    echo [build] vcvars64.bat not found at "%VCVARS%"
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 ( echo [build] vcvars failed & exit /b 1 )

cd /d "%~dp0"

echo [build] btfconvert.exe
cl /nologo /O2 /MT /W3 /EHsc ^
   /Fo"obj\\" /Fd"obj\\" /Fe"btfconvert.exe" ^
   btfconvert.cpp /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED kernel32.lib
if errorlevel 1 ( echo [build] btfconvert.exe FAILED & exit /b 1 )
if exist btfconvert.exe.manifest del btfconvert.exe.manifest

echo [build] ok -^> %~dp0btfconvert.exe
endlocal
