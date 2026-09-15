@echo off
REM ============================================================
REM  Build KeyRemap.exe  (single-file, ~1.2 MB, zero dependency)
REM  Bundles the AutoHotkey interpreter and the tray icons.
REM  Target machines need NOT install .NET / AutoHotkey / anything.
REM ============================================================
setlocal
set "ROOT=%~dp0"
set "A2E=%ROOT%tools\Ahk2Exe\Ahk2Exe.exe"
set "BASE=%ROOT%tools\base\AutoHotkey.exe"
REM 注意：产物改名带 -ahk 后缀。dist\KeyRemap.exe 是原生 C++ 版（build_cpp.cmd 生成），
REM 这里若用同名会把原生版覆盖掉
set "OUT=%ROOT%dist\KeyRemap-ahk.exe"

if not exist "%A2E%" (
    echo [ERROR] Ahk2Exe not found: %A2E%
    pause
    exit /b 1
)

REM Ahk2Exe only accepts a base file whose name ends with "AutoHotkey.exe"
if not exist "%BASE%" (
    echo [PREP] Creating base file...
    if not exist "%ROOT%tools\base" mkdir "%ROOT%tools\base"
    copy /y "%ROOT%tools\AutoHotkey\AutoHotkey64.exe" "%BASE%" >nul
)

REM Required: a running instance locks the output file, so kill it first
taskkill /f /im KeyRemap.exe >nul 2>&1
if not errorlevel 1 (
    echo [PREP] Stopped the old running instance.
    ping -n 2 127.0.0.1 >nul
)

if not exist "%ROOT%dist" mkdir "%ROOT%dist"

echo [BUILD] Packing...
"%A2E%" /in "%ROOT%prototype\KeyRemap.ahk" /out "%OUT%" /base "%BASE%" /icon "%ROOT%icons\normal.ico" /silent

if exist "%OUT%" (
    echo [OK] Built: %OUT%
    for %%A in ("%OUT%") do echo      Size: %%~zA bytes
    echo.
    echo Double-click the exe to run it. The tray icon lets you switch mode / exit.
) else (
    echo [FAILED] No output produced.
)
pause
