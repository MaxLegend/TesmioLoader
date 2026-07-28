@echo off
rem Generic headless Ghidra runner for the persistent SOVIET64 project.
rem
rem   run.bat <Script.py> <script args...>
rem
rem The project lives in tesmioloader\ghidra\proj and is already analysed, so
rem -noanalysis makes every run finish in seconds. If the project is ever lost,
rem re-create it once with:
rem
rem   analyzeHeadless.bat <projdir> soviet -import "<game>\SOVIET64.exe"
rem
rem which takes a few minutes.
setlocal
set GHIDRA=A:\Programs\ghidra_11.3.2_PUBLIC\support\analyzeHeadless.bat
set PROJ=A:\SteamLibrary\steamapps\common\SovietRepublic\tesmioloader\ghidra\proj
set SCRIPTS=A:\SteamLibrary\steamapps\common\SovietRepublic\tesmioloader\tools\ghidra
call "%GHIDRA%" "%PROJ%" soviet -process SOVIET64.exe -noanalysis -scriptPath "%SCRIPTS%" -postScript %*
echo EXIT=%errorlevel%
