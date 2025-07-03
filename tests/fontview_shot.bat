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
rem        -> optional: capture the whole screen with --monitor (see below)
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
rem  The whole-screen step is gated by a plain PAUSE, because ECAPTURE has no switch
rem  that skips its consent dialog: past the pause it blocks until you click "yes" (or
rem  "no", which is reported as REFUSED and is not a channel failure). So a run with no
rem  one at the keyboard stops there - pause itself returns immediately when stdin is
rem  redirected, it is the dialog that waits. tests\screen.ps1 is the unattended version:
rem  it finds the dialog and clicks it by control ID, so it needs nobody at all.
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

rem Start from an empty scratch folder: a leftover image from an earlier run looks
rem exactly like a fresh success (this folder is only ever test output).
set "CLEARED=0"
for %%F in ("%SHOTDIR%\*.*") do set /a CLEARED+=1 & del /q "%%~fF" >nul 2>&1
echo       cleared %CLEARED% leftover file(s) from "%SHOTDIR%"

rem fontview reads the raw tail of its command line, so hand it a path with
rem neither quotes nor spaces.
for %%F in ("%FONTARG%") do set "FSHORT=%%~sF"
if not defined FSHORT (echo [FAIL] cannot resolve the short path of "%FONTARG%" & set "RC=1" & goto :report)

echo [1/5] start %TNAME%  %FSHORT%
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

echo [2/5] capture with every channel
for %%c in (%CHANNELS%) do call :shoot %%c

if "%BAD%"=="0" (echo       all %DONE% channels ok) else (echo       %BAD% of %DONE% channels failed)

echo [3/5] whole screen (--monitor)
call :shoot_screen

echo [4/5] open the screenshot folder
start "" explorer "%SHOTDIR%"

:kill
echo [5/5] end %TNAME%
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

rem ---------------------------------------------------------------------
rem  :shoot_screen - one whole-screen shot through --monitor, only after a
rem  human yes here. ECAPTURE has no option to skip its own consent dialog,
rem  so this call blocks until that dialog is answered on screen. The default
rem  channel (wgc) is used; tests\screen.ps1 sweeps every screen-capable
rem  channel and clicks the dialog itself.
rem ---------------------------------------------------------------------
:shoot_screen
set "SSHOT=%SHOTDIR%\fontview_screen.png"
if exist "%SSHOT%" del "%SSHOT%"
echo       whole-screen shot next: ECAPTURE opens a consent dialog - click yes to capture,
echo       no to skip it (answering no is NOT counted as a failure).
pause
"%ECAPTURE%" --monitor 1 --out "%SSHOT%" >nul 2>&1
set "ERC=%ERRORLEVEL%"
if "%ERC%"=="6" (echo screen        REFUSED  consent dialog answered no - not a channel failure & goto :eof)
if not "%ERC%"=="0" (echo screen        FAIL  rc=%ERC% & set /a BAD+=1 & goto :eof)
if not exist "%SSHOT%" (echo screen        FAIL  no image written & set /a BAD+=1 & goto :eof)
for %%F in ("%SSHOT%") do set "SIZE=%%~zF"
if %SIZE% lss 1024 (echo screen        FAIL  only %SIZE% bytes & set /a BAD+=1 & goto :eof)

powershell -NoProfile -Command "Add-Type -AssemblyName System.Drawing; $i=[Drawing.Bitmap]::new('%SSHOT%'); try { $s=@{}; for($y=0; $y -lt $i.Height; $y+=11){ for($x=0; $x -lt $i.Width; $x+=11){ $p=$i.GetPixel($x,$y); $s[(($p.R -shl 16) -bor ($p.G -shl 8) -bor $p.B)]=1 } }; $w=$i.Width; $h=$i.Height } finally { $i.Dispose() }; Write-Host ('screen          ' + $w + 'x' + $h + ', ' + $s.Count + ' colors, %SIZE% bytes'); if ($s.Count -lt 12) { exit 1 }"
if not "%ERRORLEVEL%"=="0" (echo screen        FAIL  flat or empty frame & set /a BAD+=1)
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
