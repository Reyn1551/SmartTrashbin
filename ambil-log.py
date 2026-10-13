#!/usr/bin/env python3
"""
Ambil log lengkap dari SmartTrashbin dan simpan ke file untuk dikirim.

Cara pakai:
    python ambil-log.py                # ambil sekali
    python ambil-log.py --detik 60     # rekam 60 detik sambil kamu uji
    python ambil-log.py --pantau       # rekam terus sampai Ctrl+C

Hasil: file  log-smarttrashbin-<tanggal>.txt  di folder yang sama.
File itu bisa langsung dikirim ke pengembang.
"""

import argparse
import datetime
import json
import sys
import time
import urllib.request
import urllib.error

BASE = "http://192.168.4.1"
TIMEOUT = 5


def ambil(path, biner=False):
    """Ambil satu URL dari ESP32. Kembalikan teks atau None kalau gagal."""
    try:
        with urllib.request.urlopen(BASE + path, timeout=TIMEOUT) as r:
            data = r.read()
            return data if biner else data.decode("utf-8", errors="replace")
    except Exception:
        return None


def ambil_json(path):
    t = ambil(path)
    if t is None:
        return None
    try:
        return json.loads(t)
    except json.JSONDecodeError:
        return None


def header(f, args):
    f.write("=" * 70 + "\n")
    f.write("LOG DIAGNOSA SMART TRASHBIN\n")
    f.write("=" * 70 + "\n")
    f.write(f"Waktu pengambilan : {datetime.datetime.now():%Y-%m-%d %H:%M:%S}\n")
    f.write(f"Alamat ESP32      : {BASE}\n")
    if args.catatan:
        f.write(f"Catatan pengguna  : {args.catatan}\n")
    f.write("\n")


def tulis_status(f, j):
    """Tulis ringkasan status dalam bentuk yang mudah dibaca manusia."""
    if not j:
        f.write("!! TIDAK BISA MEMBACA STATUS - ESP32 tidak merespons\n\n")
        return

    f.write("-" * 70 + "\n")
    f.write("STATUS SAAT INI\n")
    f.write("-" * 70 + "\n")

    f.write(f"Uptime             : {j.get('uptime')} detik\n")
    f.write(f"Boot ke-           : {j.get('bootCount')}\n")
    f.write(f"Alasan reset       : {j.get('resetName')}\n")
    f.write(f"Brownout?          : {'YA - MASALAH!' if j.get('brownout') else 'tidak'}\n")
    f.write(f"Crash/Watchdog?    : {'YA - MASALAH!' if j.get('crash') else 'tidak'}\n")
    f.write(f"Gerakan terputus?  : {'YA - MASALAH!' if j.get('moveInterrupted') else 'tidak'}\n")
    f.write("\n")

    f.write("-- STEPPER / LID --------------------------------------------------\n")
    f.write(f"Lid KIRI  posisi   : {j.get('ll')} derajat (target {j.get('llTarget')})\n")
    f.write(f"Lid KIRI  gerak    : {'bergerak' if j.get('llMoving') else 'diam'}"
            f" @ {j.get('llSpeed')} step/s, enabled={j.get('llEnabled')}\n")
    f.write(f"Lid KANAN posisi   : {j.get('lr')} derajat (target {j.get('lrTarget')})\n")
    f.write(f"Lid KANAN gerak    : {'bergerak' if j.get('lrMoving') else 'diam'}"
            f" @ {j.get('lrSpeed')} step/s, enabled={j.get('lrEnabled')}\n")
    f.write(f"Kecepatan di-set   : {j.get('stepSpeed')} step/s\n")
    f.write(f"Mode tahan motor   : {'AKTIF (dikunci)' if j.get('holdMode') else 'mati (dilepas)'}\n")
    f.write(f"Sudah pernah BUKA  : {j.get('movedUp')}\n")
    f.write(f"Sudah pernah TUTUP : {j.get('movedDown')}\n")
    f.write("\n")

    f.write("-- JITTER (ketepatan waktu pulsa) ---------------------------------\n")
    f.write(f"Jitter maksimum    : {j.get('jitMaxUs')} us\n")
    f.write(f"Jitter rata-rata   : {j.get('jitAvgUs')} us\n")
    f.write(f"Pulsa terlambat    : {j.get('jitLate')} dari {j.get('jitSamples')}\n")
    f.write(f"Total pulsa keluar : {j.get('stepTotal')}\n")
    f.write(f"Lebar pulsa min    : {j.get('pulseMinUs')} us\n")
    f.write(f"Ramp minimum       : {j.get('rampSteps')} step\n")
    f.write("\n")

    f.write("-- POLARITAS & SISTEM ---------------------------------------------\n")
    f.write(f"Polaritas sinyal   : "
            f"{'common-anode 5V (aktif-LOW)' if j.get('pulseActiveLow') else 'common-cathode 3.3V (aktif-HIGH)'}\n")
    f.write(f"RAM bebas          : {j.get('freeHeap')} byte\n")
    f.write(f"RAM minimum        : {j.get('minHeap')} byte\n")
    f.write(f"Klien WiFi         : {j.get('wifiClients')}\n")
    f.write(f"PWM roda           : {j.get('pwm')}\n")
    f.write(f"Status gerak       : {j.get('move')}\n")
    f.write(f"Core task stepper  : {j.get('core')}\n")
    f.write("\n")

    # ---- PENILAIAN OTOMATIS ----
    f.write("-" * 70 + "\n")
    f.write("PENILAIAN OTOMATIS\n")
    f.write("-" * 70 + "\n")

    masalah = []

    jm = j.get("jitMaxUs", 0)
    if jm == 0:
        f.write("[?] Belum ada data jitter (belum ada gerakan).\n")
    elif jm < 50:
        f.write(f"[OK] Jitter maksimum {jm} us - sangat baik (< 50 us).\n")
    elif jm < 200:
        f.write(f"[!] Jitter maksimum {jm} us - agak kasar (50-200 us).\n")
        masalah.append("jitter agak kasar")
    else:
        f.write(f"[X] Jitter maksimum {jm} us - BURUK (> 200 us).\n")
        masalah.append(f"jitter buruk ({jm} us)")

    if j.get("brownout"):
        f.write("[X] BROWNOUT terdeteksi - tegangan turun saat motor jalan.\n")
        masalah.append("brownout")
    if j.get("crash"):
        f.write("[X] ESP32 crash/watchdog - kode macet.\n")
        masalah.append("crash")
    if j.get("moveInterrupted"):
        f.write("[X] Gerakan sebelumnya terputus oleh reset.\n")
        masalah.append("gerakan terputus reset")

    if not j.get("pulseActiveLow"):
        f.write("[!] Firmware aktif-HIGH (common-cathode) - sinyal ke TB6600 lemah.\n")
        masalah.append("polaritas aktif-HIGH")

    if not j.get("movedUp") or not j.get("movedDown"):
        f.write("[?] Belum pernah memerintahkan BUKA dan TUTUP - DIR belum teruji.\n")

    heap = j.get("freeHeap", 0)
    if heap < 20000:
        f.write(f"[X] RAM rendah: {heap} byte.\n")
        masalah.append("RAM rendah")

    f.write("\n")
    if masalah:
        f.write("KESIMPULAN: ADA MASALAH -> " + ", ".join(masalah) + "\n")
    else:
        f.write("KESIMPULAN: TIDAK ADA MASALAH TERDETEKSI dari status.\n")
        f.write("Kalau gejala masih terasa, masalahnya MEKANIK atau NOISE,\n")
        f.write("bukan dari firmware. Perhatikan bagian LOG di bawah.\n")
    f.write("\n")


def tulis_log(f, teks):
    f.write("-" * 70 + "\n")
    f.write("LOG LENGKAP DARI ESP32 (ring buffer 40 baris terakhir)\n")
    f.write("-" * 70 + "\n")
    if teks is None:
        f.write("!! tidak bisa membaca log\n")
    else:
        f.write(teks)
        if not teks.endswith("\n"):
            f.write("\n")
    f.write("\n")


def tulis_jejak(f, jejak):
    """Tulis perubahan status yang terekam selama pemantauan."""
    if len(jejak) < 2:
        return
    f.write("-" * 70 + "\n")
    f.write("JEJAK PERUBAHAN STATUS (selama pemantauan)\n")
    f.write("-" * 70 + "\n")

    # Temukan perubahan yang menarik
    ada = False
    for a, b in zip(jejak, jejak[1:]):
        beda = []

        if a.get("stepTotal") != b.get("stepTotal"):
            d = b["stepTotal"] - a["stepTotal"]
            beda.append(f"pulsa +{d}")

        if a.get("llMoving") != b.get("llMoving"):
            beda.append(f"KIRI {'MULAI gerak' if b['llMoving'] else 'BERHENTI'}")
        if a.get("lrMoving") != b.get("lrMoving"):
            beda.append(f"KANAN {'MULAI gerak' if b['lrMoving'] else 'BERHENTI'}")

        if a.get("ll") != b.get("ll"):
            beda.append(f"KIRI {a['ll']}->{b['ll']} derajat")
        if a.get("lr") != b.get("lr"):
            beda.append(f"KANAN {a['lr']}->{b['lr']} derajat")

        if a.get("move") != b.get("move"):
            beda.append(f"roda {a['move']}->{b['move']}")

        # Jitter naik = ada gangguan baru
        if b.get("jitMaxUs", 0) > a.get("jitMaxUs", 0):
            beda.append(f"jitter max naik {a.get('jitMaxUs')}->{b.get('jitMaxUs')} us")

        if beda:
            ada = True
            f.write(f"[{b.get('uptime'):>6} s] " + " | ".join(beda) + "\n")

    if not ada:
        f.write("(tidak ada perubahan berarti selama pemantauan)\n")
    f.write("\n")


def main():
    p = argparse.ArgumentParser(description="Ambil log SmartTrashbin")
    p.add_argument("--detik", type=int, default=0,
                   help="rekam selama N detik sambil kamu menguji")
    p.add_argument("--pantau", action="store_true",
                   help="rekam terus sampai Ctrl+C")
    p.add_argument("--catatan", default="",
                   help="catatan apa yang kamu lakukan saat merekam")
    args = p.parse_args()

    # Cek koneksi dulu
    print(f"Menghubungi {BASE} ...")
    j = ambil_json("/debug/json")
    if not j:
        print("GAGAL: ESP32 tidak merespons.")
        print("Pastikan HP/laptop tersambung ke WiFi 'SmartTrashbin'.")
        sys.exit(1)
    print(f"Terhubung. Uptime {j.get('uptime')} detik.")

    nama = datetime.datetime.now().strftime("log-smarttrashbin-%Y%m%d-%H%M%S.txt")
    jejak = []

    with open(nama, "w", encoding="utf-8") as f:
        header(f, args)

        # Tentukan lama pemantauan
        if args.pantau:
            lama = None
            print("Merekam terus. Tekan Ctrl+C untuk berhenti.")
        elif args.detik > 0:
            lama = args.detik
            print(f"Merekam {lama} detik. LAKUKAN UJIAN SEKARANG (buka/tutup).")
        else:
            lama = 0

        mulai = time.time()
        try:
            while True:
                j = ambil_json("/debug/json")
                if j:
                    jejak.append(j)
                    print(f"  [{int(time.time() - mulai):>3}s] "
                          f"pulsa={j.get('stepTotal')} "
                          f"ll={j.get('ll')} lr={j.get('lr')} "
                          f"jitter={j.get('jitMaxUs')}us", flush=True)

                if lama is not None and time.time() - mulai >= lama:
                    break
                time.sleep(1)
        except KeyboardInterrupt:
            print("\nDihentikan.")

        # Ambil data akhir
        jAkhir = ambil_json("/debug/json")
        log = ambil("/debug/log")

        tulis_status(f, jAkhir)
        tulis_jejak(f, jejak)

        f.write("-" * 70 + "\n")
        f.write("CATATAN UNTUK PENGEMBANG\n")
        f.write("-" * 70 + "\n")
        f.write("File ini berisi status lengkap + log + jejak perubahan.\n")
        f.write("Yang perlu diperhatikan: bagian PENILAIAN OTOMATIS dan\n")
        f.write("JEJAK PERUBAHAN STATUS.\n")
        f.write("\n")
        tulis_log(f, log)

    print(f"\nSelesai. File tersimpan: {nama}")
    print("Kirim file itu ke pengembang.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
