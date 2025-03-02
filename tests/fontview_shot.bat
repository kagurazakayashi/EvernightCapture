@echo off
rem Console must be UTF-8 so ECAPTURE's Chinese messages stay readable.
rem Keep this file ASCII-only: non-ASCII bytes plus a codepage switch can break
rem cmd's goto/label parsing.
chcp 65001 >nul

rem =====================================================================
rem  EvernightCapture real-machine test (Windows Font Viewer)
rem
rem  Flow: start fontview.exe on a font file -> ECAPTURE grabs that window
rem        -> verify the PNG really has content -> open the PNG with its
rem        default app -> end fontview.exe
rem
rem  Usage: tests\fontview_shot.bat [font-file]
rem  Example: tests\fontview_shot.bat C:\Windows\Fonts\simhei.ttf
rem
rem  Two things this target teaches about ECAPTURE:
rem   1. fontview.exe does NOT strip quotes around its argument, so the font
rem      path goes through the 8.3 short name (no quotes, no spaces). A quoted
rem      path makes fontview pop "not a valid font file" instead of a preview.
rem   2. That error box is a plain #32770 dialog while the real preview is
rem      FontViewWClass, so --class rejects it: a wrong window fails with
rem      match.no_window instead of quietly passing.
rem
rem  Exit: 0 pass / 1 environment or window problem / 4-9 mirror ECAPTURE
rem =====================================================================
setlocal EnableExtensions

set "TARGET=%SYSTEMROOT%\System32\fontview.exe"
set "TNAME=fontview.exe"
set "TCLASS=FontViewWClass"
set "FONTARG=%~1"
if not defined FONTARG set "FONTARG=%SYSTEMROOT%\Fonts\consola.ttf"

set "ECAPTURE=%~dp0..\build\ecapture.exe"
set "SHOTDIR=%TEMP%\ecapture-tests"
set "SHOT=%SHOTDIR%\fontview.png"

set "RC=0"
set "TRY=0"
set "WTRY=0"

if not exist "%TARGET%"   (echo [FAIL] target not found: "%TARGET%" & set "RC=1" & goto :report)
if not exist "%ECAPTURE%" (echo [FAIL] no build\ecapture.exe, run .\build.ps1 & set "RC=1" & goto :report)
if not exist "%FONTARG%"  (echo [FAIL] font file not found: "%FONTARG%" & set "RC=1" & goto :report)
if not exist "%SHOTDIR%"   mkdir "%SHOTDIR%"
if not exist "%SHOTDIR%"  (echo [FAIL] cannot create "%SHOTDIR%" & set "RC=1" & goto :report)
if exist "%SHOT%"          del "%SHOT%"

rem fontview reads the raw tail of its command line, so hand it a path with
rem neither quotes nor spaces.
for %%F in ("%FONTARG%") do set "FSHORT=%%~sF"
if not defined FSHORT (echo [FAIL] cannot resolve the short path of "%FONTARG%" & set "RC=1" & goto :report)

echo [1/5] start %TNAME%  %FSHORT%
start "" "%TARGET%" %FSHORT%

:wait_win
set /a WTRY+=1
"%ECAPTURE%" --process %TNAME% --class %TCLASS% --newest --dry-run --quiet --out "%SHOT%" >nul 2>&1
if "%ERRORLEVEL%"=="0" goto :win_up
if %WTRY% geq 25 (echo [FAIL] no %TCLASS% window after %WTRY%s - fontview likely showed its error dialog & set "RC=1" & goto :kill)
ping -n 2 127.0.0.1 >nul
goto :wait_win

:win_up
echo       preview window up, waited %WTRY%s

echo [2/5] capture
:capture
set /a TRY+=1
"%ECAPTURE%" --process %TNAME% --class %TCLASS% --newest --out "%SHOT%"
set "RC=%ERRORLEVEL%"
if "%RC%"=="0" goto :captured
if %TRY% geq 3 goto :captured
echo       capture rc=%RC%, retrying...
ping -n 3 127.0.0.1 >nul
goto :capture

:captured
if not "%RC%"=="0"    (echo [FAIL] capture failed rc=%RC% & goto :kill)
if not exist "%SHOT%" (echo [FAIL] no image written & set "RC=8" & goto :kill)
for %%F in ("%SHOT%") do set "SIZE=%%~zF"
if %SIZE% lss 1024    (echo [FAIL] image too small: %SIZE% bytes & set "RC=8" & goto :kill)

echo [3/5] sample the pixels, a blank or black frame must not pass
powershell -NoProfile -Command "Add-Type -AssemblyName System.Drawing; $i=[Drawing.Bitmap]::new('%SHOT%'); try { $s=@{}; for($y=0; $y -lt $i.Height; $y+=7){ for($x=0; $x -lt $i.Width; $x+=7){ $p=$i.GetPixel($x,$y); $s[(($p.R -shl 16) -bor ($p.G -shl 8) -bor $p.B)]=1 } }; $w=$i.Width; $h=$i.Height } finally { $i.Dispose() }; Write-Host ('      frame {0}x{1}, {2} distinct colors' -f $w,$h,$s.Count); if ($w -lt 200 -or $h -lt 150 -or $s.Count -lt 12) { exit 1 }"
if not "%ERRORLEVEL%"=="0" (echo [FAIL] the captured frame looks empty & set "RC=7")

echo [4/5] open the image with its default app
start "" "%SHOT%"

:kill
echo [5/5] end %TNAME%
taskkill /IM %TNAME% /F
call :is_running
if "%RUNNING%"=="1" (echo [FAIL] %TNAME% is still running & if "%RC%"=="0" set "RC=1") else echo       stopped

:report
echo.
if "%RC%"=="0" (echo RESULT: PASS) else (echo RESULT: FAIL rc=%RC%)
if defined SIZE echo IMAGE: %SHOT%  BYTES: %SIZE%
exit /b %RC%

rem ---- is TNAME running? sets RUNNING=1/0 without find or findstr: their names
rem ---- get shadowed by MSYS coreutils when a bash PATH leaks into cmd. ----
:is_running
set "RUNNING=0"
for /f "tokens=1 delims=," %%L in ('tasklist /NH /FO CSV /FI "IMAGENAME eq %TNAME%"') do if /i "%%~L"=="%TNAME%" set "RUNNING=1"
goto :eof
