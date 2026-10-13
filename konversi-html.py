# Konversi controller-wifi.html -> web_ui.h (string PROGMEM untuk ESP32)
#
# Kenapa pakai script, bukan copy-paste manual:
#   - HTML berisi tanda kutip ganda (") dan backslash (\) yang WAJIB di-escape
#     di C++, kalau tidak firmware akan gagal compile atau HTML rusak.
#   - Raw string literal R"rawliteral(...)rawliteral" dipakai supaya escaping
#     hampir tidak perlu. Script ini memastikan tidak ada urutan ")" di dalam
#     HTML yang bisa menutup raw literal lebih awal.
#
# Jalankan: python konversi-html.py

import re
import sys
from pathlib import Path

HERE = Path(__file__).parent
SRC = HERE / "controller-wifi.html"
DST = HERE / "web_ui.h"

def main() -> int:
    if not SRC.exists():
        print(f"ERROR: {SRC} tidak ditemukan", file=sys.stderr)
        return 1

    html = SRC.read_text(encoding="utf-8")

    # Delimiter raw literal. Kalau urutan penutup ini ada di dalam HTML,
    # raw literal akan berakhir terlalu awal dan firmware gagal compile.
    delim = "rawliteral"
    closer = ")" + delim + '"'
    if closer in html:
        print("ERROR: delimiter raw literal ditemukan di dalam HTML.", file=sys.stderr)
        print(f"       Cari urutan {closer!r} dan ubah delimiter-nya.", file=sys.stderr)
        return 1

    # '#' di awal baris aman di dalam raw literal, tapi panah trigraph
    # seperti '??/' bisa bermasalah di beberapa kompiler lama. Cek saja.
    if re.search(r"\?\?", html):
        print("PERINGATAN: HTML mengandung '??' yang bisa jadi trigraph.", file=sys.stderr)

    nlines = html.count("\n") + 1
    nbytes = len(html.encode("utf-8"))

    header = f"""// ============================================================
//  web_ui.h  --  DIBUAT OTOMATIS oleh konversi-html.py
//  JANGAN EDIT FILE INI LANGSUNG.
//  Edit controller-wifi.html, lalu jalankan:
//      python konversi-html.py
//
//  Sumber : controller-wifi.html ({nbytes} byte, {nlines} baris)
//  Berisi : halaman controller yang disajikan ESP32 di http://192.168.4.1
// ============================================================

#ifndef WEB_UI_H
#define WEB_UI_H

#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"{delim}({html}){delim}";

#endif  // WEB_UI_H
"""

    DST.write_text(header, encoding="utf-8")
    print(f"OK  {SRC.name} -> {DST.name}")
    print(f"    {nbytes} byte HTML, {nlines} baris")
    print(f"    header total {len(header.encode('utf-8'))} byte")
    return 0


if __name__ == "__main__":
    sys.exit(main())
