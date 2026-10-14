#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
============================================================
 STEPPER CHECK, SMART TRASHBIN  -  diagnostics over USB cable
============================================================

Tests the lid stepper STEP BY STEP over a USB cable, with NO WiFi
and NO BLE. So you don't have to kill the internet, and there is no
risk of getting "stuck on connecting".

Steps, in order:
  1. Open the serial port, wait for the ESP32 to be ready
  2. J  -> test the ESP32 pins (can they be driven or not)
  3. E  -> pulse test, motor moved VERY slowly
  4. D  -> direction test, motor jogged forward/back
  5. Y2 -> full diagnostic report from the firmware
  6. Save everything to hasil-cek-stepper.txt

Usage:
    cek-stepper.cmd            (double-click - easiest way)
    python cek-stepper.py      (directly)
    python cek-stepper.py --port COM7
    python cek-stepper.py --nol  (no need to press Enter)

Output: hasil-cek-stepper.txt
"""

import argparse
import datetime
import io
import os
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("[X] Modul 'pyserial' belum terpasang.")
    print("    Jalankan:  python -m pip install pyserial")
    sys.exit(1)


BAUD = 115200
OUTFILE = "hasil-cek-stepper.txt"


# ------------------------------------------------------------
#  PORT SELECTION
# ------------------------------------------------------------
def pilih_port(port_diminta=None):
    """Tentukan port serial ESP32. Kembalikan nama port atau None."""
    ports = list(list_ports.comports())

    if not ports:
        print("[X] Tidak ada port serial sama sekali.")
        print("    Pastikan kabel USB tersambung ke ESP32,")
        print("    dan driver CP210x sudah terpasang.")
        return None

    # Explicit request: verify it exists first
    if port_diminta:
        for p in ports:
            if p.device.upper() == port_diminta.upper():
                return p.device
        print("[!] Port %s tidak ditemukan." % port_diminta)
        print("    Port yang ada:")
        for p in ports:
            print("      %s - %s" % (p.device, p.description))

    # Match CP210x / CH340 / CH341 -- the USB bridge chips ESP32 boards use
    for p in ports:
        teks = ("%s %s" % (p.description or "", p.hwid or "")).upper()
        if ("CP210" in teks) or ("CH340" in teks) or ("CH341" in teks):
            print("[OK] ESP32 terdeteksi di %s (%s)" % (p.device, p.description))
            return p.device

    # No known bridge: if only one port exists, just use it
    if len(ports) == 1:
        print("[OK] Memakai satu-satunya port: %s (%s)"
              % (ports[0].device, ports[0].description))
        return ports[0].device

    print("[!] Beberapa port ditemukan, tidak ada yang jelas ESP32:")
    for p in ports:
        print("      %s - %s" % (p.device, p.description))
    return None


# ------------------------------------------------------------
#  COMMUNICATION CLASS
# ------------------------------------------------------------
class Esp:
    def __init__(self, port):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = BAUD
        # dtr/rts False on purpose: leaving them asserted holds the ESP32
        # in reset, so it prints nothing.
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.timeout = 0.25
        self.ser.write_timeout = 2
        self.buffer = []

    def buka(self):
        self.ser.open()
        time.sleep(0.5)
        self.ser.reset_input_buffer()

    def tutup(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def reset(self):
        """Reset ringan lewat RTS supaya kita lihat pesan boot."""
        try:
            self.ser.rts = True
            time.sleep(0.15)
            self.ser.rts = False
            time.sleep(0.05)
            self.ser.dtr = False
            self.ser.rts = False
        except Exception:
            pass

    def kirim(self, cmd):
        print("    > %s" % cmd)
        self.ser.write((cmd + "\n").encode("ascii"))
        time.sleep(0.3)

    def baca(self, detik):
        """Baca selama `detik` detik, kembalikan teks dan simpan."""
        keluar = []
        akhir = time.time() + detik
        while time.time() < akhir:
            try:
                data = self.ser.read(16384)
            except Exception:
                data = None
            if data:
                teks = data.decode("utf-8", "replace")
                self.buffer.append(teks)
                keluar.append(teks)
        return "".join(keluar)


# ------------------------------------------------------------
#  DISPLAY HELPERS
# ------------------------------------------------------------
def garis(char="=", n=64):
    print(char * n)


def judul(teks):
    print()
    garis()
    print("  " + teks)
    garis()


def tampilkan(teks, saring=None):
    """Tampilkan baris log yang relevan saja, biar tidak kebanjiran."""
    for baris in teks.splitlines():
        b = baris.strip()
        if not b:
            continue
        if saring and not any(s in b for s in saring):
            continue
        print("    " + b)


# ============================================================
#  MAIN PROGRAM
# ============================================================
def main():
    p = argparse.ArgumentParser(
        description="Diagnostik stepper SmartTrashbin via kabel USB")
    p.add_argument("--port", default=None,
                   help="Port serial, mis. COM6 (default: deteksi otomatis)")
    p.add_argument("--nol", action="store_true",
                   help="Tanpa perlu menekan Enter (untuk pengujian otomatis)")
    args = p.parse_args()

    print()
    garis()
    print("  CEK STEPPER SMART TRASHBIN")
    print("  Diagnostik via kabel USB - tanpa WiFi, tanpa BLE")
    garis()

    # ---------- 1. Locate port ----------
    judul("LANGKAH 1/6  -  Mencari ESP32")
    port = pilih_port(args.port)
    if not port:
        print()
        print("Selesai tanpa hasil: ESP32 tidak ditemukan.")
        return 1

    esp = Esp(port)
    try:
        esp.buka()
    except Exception as e:
        print("[X] Tidak bisa membuka %s: %s" % (port, e))
        print("    Kemungkinan: port sedang dipakai program lain.")
        print("    Tutup Serial Monitor Arduino IDE / aplikasi lain,")
        print("    lalu jalankan ulang file ini.")
        return 1

    print("[OK] Port %s terbuka." % port)

    if not args.nol:
        print()
        print("    PENTING: pastikan kabel USB tersambung dan ESP32 menyala.")
        try:
            input("    Tekan ENTER kalau sudah siap... ")
        except EOFError:
            pass

    # ---------- 2. Reset and read boot ----------
    judul("LANGKAH 2/6  -  Menyalakan ulang ESP32, membaca pesan boot")
    print("    (ini memastikan ESP32 hidup dan firmware benar-benar jalan)")
    esp.reset()
    boot = esp.baca(7)
    tampilkan(boot, saring=["Boot", "WiFi", "Web server", "Task stepper",
                            "Stepper siap", "BLE", "BROWNOUT", "TERPUTUS",
                            "SUDUT", "STEP]"])
    if "Task stepper" in boot:
        print()
        print("    [OK] Firmware jalan, task stepper aktif di core 1.")
    else:
        print()
        print("    [!!] Pesan boot tidak lengkap. ESP32 mungkin belum siap.")
        print("         Coba cabut-pasang kabel USB, lalu jalankan lagi.")

    # ---------- 3. Pin test ----------
    judul("LANGKAH 3/6  -  UJI PIN (apakah pin ESP32 sehat?)")
    print("    Ini memisahkan masalah firmware dari masalah kabel.")
    print("    Motor TIDAK bergerak di langkah ini.")
    esp.kirim("J")
    pin = esp.baca(6)
    tampilkan(pin, saring=["PUL OK", "DIR OK", "ENA OK",
                           "PUL BERMASALAH", "DIR BERMASALAH",
                           "ENA BERMASALAH", "Kesimpulan",
                           "SEHAT", "SETELAH ESP32", "TIDAK BISA DIKENDALIKAN"])
    sehat = pin.count("OK - pin bisa dibolak-balik") >= 6
    if sehat:
        print()
        print("    HASIL: PIN ESP32 SEHAT SEMUA.")
    else:
        print()
        print("    HASIL: ADA PIN BERMASALAH - masalah di kabell/board.")

    # ---------- 4. Pulse test ----------
    judul("LANGKAH 4/6  -  UJI PULSA (motor digerakkan sangat pelan)")
    print("    >>> PERHATIKAN MOTOR STEPPER SEKARANG <<<")
    print("    Motor akan bergerak 200 langkah, pelan sekali.")
    print("    Kalau hanya mendesis dan tidak bergerak, itu temuan penting.")
    if not args.nol:
        try:
            input("    Tekan ENTER untuk mulai... ")
        except EOFError:
            pass
    esp.kirim("E")
    pulsa = esp.baca(12)
    tampilkan(pulsa, saring=["UJIPIN", "pulsa keluar", "SUDAH BENAR",
                             "TIDAK COCOK", "PRESISI"])
    keluar_ok = "200 dari 200" in pulsa
    if keluar_ok:
        print()
        print("    HASIL: ESP32 mengirim pulsa LENGKAP (200 dari 200).")
        print("    Kalau motor tetap diam, penyebabnya DI LUAR ESP32.")
    else:
        print()
        print("    HASIL: jumlah pulsa tidak lengkap - masalah di ESP32.")

    # ---------- 5. Direction test ----------
    judul("LANGKAH 5/6  -  UJI ARAH (motor digoyang maju-mundur)")
    print("    >>> PERHATIKAN MOTOR LAGI <<<")
    print("    Yang benar: motor BERGoyang bolak-balik di tempat.")
    print("    Yang salah : motor jalan terus satu arah.")
    if not args.nol:
        try:
            input("    Tekan ENTER untuk mulai... ")
        except EOFError:
            pass
    esp.kirim("D")
    arah = esp.baca(12)
    tampilkan(arah, saring=["UJIDIR", "firmware benar", "TIDAK membalik",
                            "pulsa, selisih", "BERGOYANG", "JALAN TERUS"])

    # ---------- 6. Diagnostic report ----------
    judul("LANGKAH 6/6  -  LAPORAN DIAGNOSA DARI FIRMWARE")
    esp.kirim("Y2")
    diag = esp.baca(4)
    tampilkan(diag, saring=["LAPORAN", "DIAG"])

    # ---------- Save ----------
    esp.tutup()

    isi = "".join(esp.buffer)
    # Cleanup: the ESP32 emits garbage bytes on reset. Drop NULs and
    # normalise line endings.
    isi = isi.replace("\x00", "").replace("\r\n", "\n").replace("\r", "\n")

    garis64 = "=" * 64

    header = (
        garis64 + "\n"
        "HASIL CEK STEPPER SMART TRASHBIN\n"
        + garis64 + "\n"
        "Waktu   : %s\n"
        "Port    : %s\n"
        "Baud    : %d\n"
        "Catatan : dijalankan lewat kabel USB (tanpa WiFi/BLE)\n"
        "\n" % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                port, BAUD)
    )

    with io.open(OUTFILE, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
        f.write(garis64 + "\n")
        f.write("RINGKASAN OTOMATIS\n")
        f.write(garis64 + "\n")
        f.write("Pin ESP32 sehat        : %s\n" % ("YA" if sehat else "TIDAK"))
        f.write("Pulsa keluar lengkap   : %s\n" % ("YA" if keluar_ok else "TIDAK"))
        f.write("Firmware jalan         : %s\n"
                % ("YA" if "Task stepper" in boot else "TIDAK"))
        f.write("\n")
        f.write(garis64 + "\n")
        f.write("LOG LENGKAP MENTAH\n")
        f.write(garis64 + "\n\n")
        f.write(isi)

    # ---------- Final conclusion ----------
    judul("KESIMPULAN")
    print("  Pin ESP32 sehat        : %s" % ("YA" if sehat else "TIDAK"))
    print("  Pulsa keluar lengkap   : %s" % ("YA" if keluar_ok else "TIDAK"))
    print("  Firmware jalan         : %s"
          % ("YA" if "Task stepper" in boot else "TIDAK"))
    print()

    if sehat and keluar_ok:
        print("  Artinya: SISI ESP32 SUDAH BENAR SEMUA.")
        print()
        print("  Karena itu, kalau motor tetap hanya mendesis, penyebabnya")
        print("  ada di LUAR ESP32. Periksa berurutan:")
        print()
        print("   1. TEGANGAN CATU TB6600")
        print("      TB6600 kamu bertulis DC 19-40VDC. Ukur dengan")
        print("      multimeter di terminal VCC dan GND TB6600.")
        print("      Kalau di bawah 19V, motor akan mendesis tanpa")
        print("      berputar. Ini penyebab paling sering.")
        print()
        print("   2. ARUS DIP SWITCH TB6600")
        print("      NEMA 17 biasanya butuh 1.5 - 2.0 A.")
        print()
        print("   3. KABEL MOTOR A+/A-/B+/B-")
        print("      Pastikan keempatnya kuat dan tidak tertukar pasangan.")
        print()
        print("   4. TUKAR MODUL TB6600 kiri <-> kanan")
        print("      Kalau masalahnya ikut pindah, modulnya rusak.")
    elif sehat and not keluar_ok:
        print("  Pin sehat TAPI pulsa tidak lengkap.")
        print("  Ini masalah di firmware ESP32, bukan hardware.")
    else:
        print("  Ada pin yang tidak bisa dikendalikan.")
        print("  Coba lepas kabel pin itu dari TB6600, lalu jalankan")
        print("  file ini lagi. Kalau jadi sehat, rangkaian input")
        print("  TB6600 yang menahan pin.")

    print()
    garis()
    print("  Hasil lengkap disimpan di:  %s" % OUTFILE)
    print("  Kirim isi file itu untuk dianalisa.")
    garis()
    print()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[!] Dihentikan oleh pengguna.")
        sys.exit(130)
