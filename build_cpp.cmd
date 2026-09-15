@echo off
REM ============================================================
REM  Build KeyRemap.exe  (native Win32 C++, ~100-200 KB)
REM  Links the CRT statically (/MT) => zero runtime dependency.
REM ============================================================
setlocal
set "ROOT=%~dp0"
set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VCVARS=%VS%\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" (
    echo [ERROR] MSVC toolchain not found at:
    echo         %VCVARS%
    pause
    exit /b 1
)

echo [PREP] Setting up MSVC environment...
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed.
    pause
    exit /b 1
)

REM A running instance locks the output file - kill it first
taskkill /f /im KeyRemap.exe >nul 2>&1
if not errorlevel 1 (
    echo [PREP] Stopped the old running instance.
    ping -n 2 127.0.0.1 >nul
)

if not exist "%ROOT%dist" mkdir "%ROOT%dist"
REM Delete the old output first, so a failed compile can't be mistaken for success
if exist "%ROOT%dist\KeyRemap.exe" del /f /q "%ROOT%dist\KeyRemap.exe" >nul 2>&1
pushd "%ROOT%cpp"

echo [BUILD] Compiling resources...
rc /nologo /fo keyremap.res keyremap.rc
if errorlevel 1 (
    echo [FAILED] Resource compiler failed.
    popd
    pause
    exit /b 1
)

echo [BUILD] Compiling and linking...
cl /nologo /O2 /MT /GS /W3 /utf-8 /DUNICODE /D_UNICODE /EHsc keyremap.cpp keyremap.res /link /SUBSYSTEM:WINDOWS /OUT:"%ROOT%dist\KeyRemap.exe" user32.lib gdi32.lib gdiplus.lib shell32.lib

REM Clean intermediate files
if exist keyremap.obj del /f /q keyremap.obj >nul 2>&1
if exist keyremap.res del /f /q keyremap.res >nul 2>&1

popd
if exist "%ROOT%dist\KeyRemap.exe" (
    echo [OK] Built: %ROOT%dist\KeyRemap.exe
    for %%A in ("%ROOT%dist\KeyRemap.exe") do echo      Size: %%~zA bytes
) else (
    echo [FAILED] No output produced.
)
pause
