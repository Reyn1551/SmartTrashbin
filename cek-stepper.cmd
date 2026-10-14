@echo off
REM ============================================================
REM  CEK STEPPER SMART TRASHBIN  -  lewat kabel USB (serial)
REM ============================================================
REM
REM  KENAPA LEWAT KABEL USB
REM  Cara ini TIDAK butuh WiFi dan TIDAK butuh BLE, jadi HP kamu
REM  tetap bisa dipakai untuk hal lain (dan laptop tetap online).
REM  Perintah dikirim langsung lewat kabel USB ke ESP32.
REM
REM  CARA PAKAI
REM    1. Sambungkan ESP32 ke laptop dengan kabel USB (COM6).
REM    2. Klik dua kali file ini.
REM    3. Ikuti petunjuk di layar.
REM
REM  Hasil lengkap disimpan ke file:  hasil-cek-stepper.txt
REM  Kirim isi file itu supaya bisa dianalisa.
REM
REM  ============================================================
REM  DIJALANKAN OTOMATIS:  python cek-stepper.py
REM ============================================================

setlocal
cd /d "%~dp0"

echo.
echo ============================================================
echo   CEK STEPPER SMART TRASHBIN  -  via kabel USB
echo ============================================================
echo.

REM ---------- Cari Python ----------
set PY=
where python >nul 2>&1 && set PY=python
if "%PY%"=="" (
  where py >nul 2>&1 && set PY=py
)
if "%PY%"=="" (
  if exist "C:\Users\Reynboo\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe" (
    set PY=C:\Users\Reynboo\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe
  )
)
if "%PY%"=="" (
  echo [X] Python tidak ditemukan di komputer ini.
  echo.
  echo     Install Python dulu dari https://www.python.org/downloads/
  echo     lalu centang "Add Python to PATH" saat install.
  echo.
  pause
  exit /b 1
)

echo [OK] Python ditemukan: %PY%
echo.

REM ---------- Pastikan pyserial ada ----------
"%PY%" -c "import serial" >nul 2>&1
if errorlevel 1 (
  echo [..] Menginstall pyserial ^(sekali saja, butuh internet^)...
  "%PY%" -m pip install --quiet --disable-pip-version-check pyserial
  "%PY%" -c "import serial" >nul 2>&1
  if errorlevel 1 (
    echo [X] Gagal install pyserial.
    echo     Coba jalankan:  "%PY%" -m pip install pyserial
    echo.
    pause
    exit /b 1
  )
)
echo [OK] pyserial siap.
echo.

REM ---------- Jalankan program cek ----------
"%PY%" "%~dp0cek-stepper.py" %*

echo.
echo ============================================================
echo   Selesai. Hasil tersimpan di:  hasil-cek-stepper.txt
echo   Kirim isi file itu untuk dianalisa.
echo ============================================================
echo.
pause
