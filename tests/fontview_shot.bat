@echo off
rem Console must be UTF-8 so ECAPTURE's Chinese messages stay readable.
rem Keep this file ASCII-only: non-ASCII bytes plus a codepage switch can break
rem cmd's goto/label parsing.
chcp 65001 >nul

rem =====================================================================
rem  EvernightCapture real-machine test (Windows Font Viewer)
rem
rem  Flow: start fontview.exe on a font file
rem        -> capture that window with EVERY --capture channel
rem        -> verify each PNG really has content (size + distinct colors)
rem        -> open the screenshot folder in Explorer
rem        -> end fontview.exe
rem
rem  Usage: tests\fontview_shot.bat [font-file] ["channel list"]
rem  Example: tests\fontview_shot.bat C:\Windows\Fonts\simhei.ttf
rem  Example: tests\fontview_shot.bat "" "wgc bitblt"    (only these channels)
rem
rem  Channels below must stay in sync with kCaptureValues in src/CliOptions.cpp
rem  (tests\channels.ps1 does the same sweep with stricter occlusion checks).
rem
rem  Two things this target teaches about ECAPTURE:
rem   1. fontview.exe does NOT strip quotes around its argument, so the font
rem      path goes through the 8.3 short name (no quotes, no spaces). A quoted
rem      path makes fontview pop "not a valid font file" instead of a preview.
rem   2. That error box is a plain #32770 dialog while the real preview is
rem      FontViewWClass, so --class rejects it: a wrong window fails with
rem      match.no_window instead of quietly passing.
rem
rem  Exit: 0 all channels ok / 1 environment or window problem /
rem        7 at least one channel failed (capture or empty frame)
rem =====================================================================
setlocal EnableExtensions

set "TARGET=%SYSTEMROOT%\System32\fontview.exe"
set "TNAME=fontview.exe"
set "TCLASS=FontViewWClass"
set "CHANNELS=wgc dwm printwindow bitblt duplication auto"
set "FONTARG=%~1"
if not defined FONTARG set "FONTARG=%SYSTEMROOT%\Fonts\consola.ttf"
if not "%~2"=="" set "CHANNELS=%~2"

set "ECAPTURE=%~dp0..\build\ecapture.exe"
set "SHOTDIR=%TEMP%\ecapture-tests"
set "PROBE=%SHOTDIR%\fontview_probe.png"

set "RC=0"
set "BAD=0"
set "WTRY=0"
set "DONE=0"

if not exist "%TARGET%"   (echo [FAIL] target not found: "%TARGET%" & set "RC=1" & goto :report)
if not exist "%ECAPTURE%" (echo [FAIL] no build\ecapture.exe, run .\build.ps1 & set "RC=1" & goto :report)
if not exist "%FONTARG%"  (echo [FAIL] font file not found: "%FONTARG%" & set "RC=1" & goto :report)
if not exist "%SHOTDIR%"   mkdir "%SHOTDIR%"
if not exist "%SHOTDIR%"  (echo [FAIL] cannot create "%SHOTDIR%" & set "RC=1" & goto :report)
del "%SHOTDIR%\fontview_*.png" >nul 2>&1

rem fontview reads the raw tail of its command line, so hand it a path with
rem neither quotes nor spaces.
for %%F in ("%FONTARG%") do set "FSHORT=%%~sF"
if not defined FSHORT (echo [FAIL] cannot resolve the short path of "%FONTARG%" & set "RC=1" & goto :report)

echo [1/4] start %TNAME%  %FSHORT%
start "" "%TARGET%" %FSHORT%

:wait_win
set /a WTRY+=1
"%ECAPTURE%" --process %TNAME% --class %TCLASS% --newest --dry-run --quiet --out "%PROBE%" >nul 2>&1
if "%ERRORLEVEL%"=="0" goto :win_up
if %WTRY% geq 25 (echo [FAIL] no %TCLASS% window after %WTRY%s - fontview likely showed its error dialog & set "RC=1" & goto :kill)
ping -n 2 127.0.0.1 >nul
goto :wait_win

:win_up
echo       preview window up, waited %WTRY%s

echo [2/4] capture with every channel
for %%c in (%CHANNELS%) do call :shoot %%c

if "%BAD%"=="0" (echo       all %DONE% channels ok) else (echo       %BAD% of %DONE% channels failed)

echo [3/4] open the screenshot folder
start "" explorer "%SHOTDIR%"

:kill
echo [4/4] end %TNAME%
taskkill /IM %TNAME% /F
call :is_running
if "%RUNNING%"=="1" (echo [FAIL] %TNAME% is still running & if "%RC%"=="0" set "RC=1") else echo       stopped

:report
echo.
if not "%BAD%"=="0" set "RC=7"
if "%RC%"=="0" (echo RESULT: PASS) else (echo RESULT: FAIL rc=%RC%)
echo IMAGES: %SHOTDIR%\fontview_*.png
exit /b %RC%

rem ---------------------------------------------------------------------
rem  :shoot <channel> - capture once, then reject blank frames.
rem  A frame that is one flat color must never count as success: an error
rem  dialog, an unpainted window and a failed GPU copy all look like that.
rem ---------------------------------------------------------------------
:shoot
set "CH=%~1"
set "SHOT=%SHOTDIR%\fontview_%CH%.png"
set "ATTEMPT=0"
if exist "%SHOT%" del "%SHOT%"

:s_retry
set /a ATTEMPT+=1
"%ECAPTURE%" --process %TNAME% --class %TCLASS% --newest --capture %CH% --out "%SHOT%" >nul 2>&1
set "ERC=%ERRORLEVEL%"
if "%ERC%"=="0" goto :s_file
if %ATTEMPT% geq 2 goto :s_err
rem duplication can miss the first frame; one retry before calling it a failure
ping -n 3 127.0.0.1 >nul
goto :s_retry

:s_err
call :pad %CH%
echo %PCH%  FAIL  rc=%ERC%
set /a BAD+=1
goto :s_done

:s_file
call :pad %CH%
if not exist "%SHOT%" (echo %PCH%  FAIL  no image written & set /a BAD+=1 & goto :s_done)
for %%F in ("%SHOT%") do set "SIZE=%%~zF"
if %SIZE% lss 1024 (echo %PCH%  FAIL  only %SIZE% bytes & set /a BAD+=1 & goto :s_done)

powershell -NoProfile -Command "Add-Type -AssemblyName System.Drawing; $i=[Drawing.Bitmap]::new('%SHOT%'); try { $s=@{}; for($y=0; $y -lt $i.Height; $y+=7){ for($x=0; $x -lt $i.Width; $x+=7){ $p=$i.GetPixel($x,$y); $s[(($p.R -shl 16) -bor ($p.G -shl 8) -bor $p.B)]=1 } }; $w=$i.Width; $h=$i.Height } finally { $i.Dispose() }; Write-Host ('%PCH%  ' + $w + 'x' + $h + ', ' + $s.Count + ' colors, %SIZE% bytes'); if ($w -lt 200 -or $h -lt 150 -or $s.Count -lt 12) { exit 1 }"
if not "%ERRORLEVEL%"=="0" (echo %PCH%  FAIL  flat or empty frame & set /a BAD+=1)

:s_done
set /a DONE+=1
goto :eof

rem ---- pad the channel name to a fixed column so the table lines up ----
:pad
set "PCH=%~1             "
set "PCH=%PCH:~0,13%"
goto :eof

rem ---- is TNAME running? sets RUNNING=1/0 without find or findstr: their names
rem ---- get shadowed by MSYS coreutils when a bash PATH leaks into cmd. ----
:is_running
set "RUNNING=0"
for /f "tokens=1 delims=," %%L in ('tasklist /NH /FO CSV /FI "IMAGENAME eq %TNAME%"') do if /i "%%~L"=="%TNAME%" set "RUNNING=1"
goto :eof
