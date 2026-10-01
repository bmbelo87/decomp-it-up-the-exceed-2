#!/usr/bin/env python3
"""
gen_exceed2_songs.py - gera src/exceed_songs.c a partir das tabelas do PIU32.EXE (Exceed2).

Tabelas lidas:
  0x004563A0  138 registros x 0x48 bytes (contagem: 0x412DD0 retorna 0x8A)
      +0x00 u32 sequencia   +0x04 u32 id (hex -> "%X")
      +0x08/+0x0C artista KR/EN, +0x10/+0x14 titulo KR/EN (ponteiros, CP949)
      +0x18 double BPM      +0x1C u32 (?)
      +0x24..+0x34 5 x s32 niveis NORMAL HARD CRAZY FREESTYLE NIGHTMARE (-1 = nao existe)
      +0x38 byte visivel (refeito no Begin: 0x413811 = (oculta == 0))
      +0x39 byte oculta     +0x3A byte desligada (EEPROM +0x52B, 0x412F00)
      +0x3C u32 textura do disco (-1), +0x40..+0x44 5 bytes de trava por dificuldade
  0x00458AB8  int canal[5][53] (linha 0xD4), 0 termina a lista
      0 BANYA, 1 K-POP, 2 POP (ARCADE STATION), 3 REMIX, 4 BATTLE

Uso:
  python tools/gen_exceed2_songs.py <PIU32.EXE> <saida.c>
"""
import struct
import sys

SONG_VA = 0x004563A0
SONG_COUNT = 0x8A
SONG_SIZE = 0x48
CHANNEL_VA = 0x00458AB8
CHANNEL_COUNT = 5
CHANNEL_MAX = 53


def load_sections(b):
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    nsec = struct.unpack_from("<H", b, pe + 6)[0]
    optsz = struct.unpack_from("<H", b, pe + 20)[0]
    base = struct.unpack_from("<I", b, pe + 24 + 28)[0]
    secs = []
    o = pe + 24 + optsz
    for i in range(nsec):
        vsize, va, rawsz, raw = struct.unpack_from("<IIII", b, o + 8)
        secs.append((base + va, max(vsize, rawsz), raw, rawsz))
        o += 40
    return secs


def va_to_off(secs, va):
    for sva, size, raw, rawsz in secs:
        if sva <= va < sva + size:
            if va - sva >= rawsz:
                raise ValueError("VA 0x%X fora dos dados do arquivo" % va)
            return raw + (va - sva)
    raise ValueError("VA 0x%X fora das secoes" % va)


def read_cstr(b, secs, va):
    if va == 0:
        return ""
    o = va_to_off(secs, va)
    e = b.index(b"\0", o)
    return b[o:e].decode("cp949", "replace")


def c_escape(s):
    out = []
    for ch in s.encode("utf-8"):
        if ch in (0x22, 0x5C):
            out.append("\\" + chr(ch))
        elif 32 <= ch < 127:
            out.append(chr(ch))
        else:
            out.append("\\%03o" % ch)
    return '"' + "".join(out) + '"'


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    b = open(sys.argv[1], "rb").read()
    secs = load_sections(b)
    lines = []
    lines.append("/* GERADO por tools/gen_exceed2_songs.py a partir do PIU32.EXE (Exceed2) - nao editar a mao.")
    lines.append(" *   g_exSongs     <- 0x004563A0 (138 x 0x48 bytes)")
    lines.append(" *   g_exChannels  <- 0x00458AB8 (int[5][53]) */")
    lines.append('#include "pumpy.h"')
    lines.append("")
    lines.append("const ExceedSong g_exSongs[EX_SONG_COUNT] = {")
    so = va_to_off(secs, SONG_VA)
    for i in range(SONG_COUNT):
        o = so + i * SONG_SIZE
        seq, sid, ak, ae, tk, te = struct.unpack_from("<6I", b, o)
        bpm = struct.unpack_from("<d", b, o + 0x18)[0]
        lv = struct.unpack_from("<5i", b, o + 0x24)
        vis, hid, off = b[o + 0x38], b[o + 0x39], b[o + 0x3A]
        lock = b[o + 0x40:o + 0x45]
        lines.append("    { 0x%X, %s, %s, %s, %s, %.4f, { %s }, %d, %d, { %s } }, /* %d */" % (
            sid, c_escape(read_cstr(b, secs, ak)), c_escape(read_cstr(b, secs, ae)),
            c_escape(read_cstr(b, secs, tk)), c_escape(read_cstr(b, secs, te)), bpm,
            ", ".join(str(x) for x in lv), vis, hid, ", ".join(str(x) for x in lock), i))
        print("  %3d  seq=%-3d %X  bpm=%-6g niveis=%s oculta=%d desligada(estatico)=%d" % (
            i, seq, sid, bpm, list(lv), hid, off))
    lines.append("};")
    lines.append("")
    lines.append("const int g_exChannels[EX_CHANNEL_COUNT][EX_CHANNEL_MAX] = {")
    co = va_to_off(secs, CHANNEL_VA)
    for ch in range(CHANNEL_COUNT):
        ids = struct.unpack_from("<%dI" % CHANNEL_MAX, b, co + ch * CHANNEL_MAX * 4)
        n = next((k for k, x in enumerate(ids) if x == 0), CHANNEL_MAX)
        print("canal %d: %d musicas" % (ch, n))
        lines.append("    { %s }," % ", ".join("0x%X" % x for x in ids))
    lines.append("};")
    lines.append("")
    open(sys.argv[2], "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print("gravado: %s" % sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
