# xwbwaves.py -- list or extract the waves of a game wave bank (game/Sounds/<bank>.xwb)
#   xwbwaves.py <bank> unused <outdir> [maxsec]   decode every wave no .xsb references (to <outdir>/<bank>_<idx>.wav)
#   xwbwaves.py <bank> wave <idx-hex> <out.wav>  decode one wave
#   xwbwaves.py <bank> cues <xsb>                the waves an .xsb's cues play
# XWB: entries from 0x50, 24 bytes each (flags, format, offset, length, loop start, loop length); data from the u32 at
# 0x20; format: bits 0-1 tag (1 = Xbox ADPCM, 36-byte blocks per channel = 64 samples), 2-4 channels, 5- rate.
# XSB: the bank names are at the u32 offset +8 (count u16 at +0x1E); a wave reference is (u16 index, 00 00, bank, FE ...)
# (the reference scan misses variation tables, so "unused" is a candidate list, not proof).
import glob
import os
import re
import struct
import sys
import wave

SND = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game', 'Sounds')
IDX_T = [-1, -1, -1, -1, 2, 4, 6, 8] * 2
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
        118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
        6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767]


def adpcm(raw, ch):
    out = []
    bs = 36 * ch
    for b in range(0, len(raw) - bs + 1, bs):
        blk = raw[b:b + bs]
        preds, idxs = [], []
        for c in range(ch):
            p, i = struct.unpack_from('<hB', blk, 4 * c)
            preds.append(p), idxs.append(i)
        chans = [[p] for p in preds]
        data = blk[4 * ch:]
        for g in range(8):
            for c in range(ch):
                for byte in data[(g * ch + c) * 4:(g * ch + c) * 4 + 4]:
                    for nib in (byte & 15, byte >> 4):
                        s = STEP[idxs[c]]
                        d = s >> 3
                        if nib & 4: d += s
                        if nib & 2: d += s >> 1
                        if nib & 1: d += s >> 2
                        preds[c] = max(-32768, min(32767, preds[c] - d if nib & 8 else preds[c] + d))
                        idxs[c] = max(0, min(88, idxs[c] + IDX_T[nib]))
                        chans[c].append(preds[c])
        for k in range(65):
            for c in range(ch):
                out.append(chans[c][k])
    return out


class Bank:
    def __init__(self, name):
        self.name = name
        self.d = open(os.path.join(SND, name + '.xwb'), 'rb').read()
        self.base = struct.unpack_from('<I', self.d, 0x20)[0]
        self.n = struct.unpack_from('<I', self.d, 0x2c)[0]   # entry count

    def entry(self, k):
        return struct.unpack_from('<6I', self.d, 0x50 + 24 * k)

    def valid(self, k):
        fl, fmt, off, ln, ls, ll = self.entry(k)
        return ln > 0 and self.base + off + ln <= len(self.d) and (fmt >> 5) & 0x3ffffff in range(4000, 96001)

    def seconds(self, k):
        fl, fmt, off, ln, ls, ll = self.entry(k)
        ch = max((fmt >> 2) & 7, 1)
        sr = (fmt >> 5) & 0x3ffffff
        return ln / 36 * 64 / ch / sr if fmt & 3 == 1 else ln / 2 / ch / sr

    def write(self, k, path):
        fl, fmt, off, ln, ls, ll = self.entry(k)
        ch = max((fmt >> 2) & 7, 1)
        sr = (fmt >> 5) & 0x3ffffff
        raw = self.d[self.base + off:self.base + off + ln]
        if fmt & 3 == 1:
            s = adpcm(raw, ch)
            pcm = struct.pack('<%dh' % len(s), *s)
        elif fmt & 3 == 0:
            pcm = raw
        else:
            return False
        with wave.open(path, 'wb') as w:
            w.setnchannels(ch), w.setsampwidth(2), w.setframerate(sr), w.writeframes(pcm)
        return True


def xsb_banks(d):
    nameoff = struct.unpack('<I', d[8:12])[0]
    nb = struct.unpack('<H', d[0x1e:0x20])[0]
    return [s.decode('latin1').lower() for s in d[nameoff:].split(b'\0')[:nb]]


def xsb_waves(d):
    used = set()
    for m in re.finditer(rb'(..)\x00\x00.\xfe\x00\x00\x01\xff\x00[\x01\x09]', d, re.S):
        used.add(struct.unpack('<H', m.group(1))[0])
    for m in re.finditer(rb'\x01\x00\x00\x00\x10\x00\x00\x00(..)\x00\x00.\xfe', d, re.S):
        used.add(struct.unpack('<H', m.group(1))[0])
    return used


def referenced(bank):
    used = {}
    for f in glob.glob(os.path.join(SND, '*.xsb')):
        d = open(f, 'rb').read()
        if bank.lower() not in xsb_banks(d): continue
        for w in xsb_waves(d): used.setdefault(w, set()).add(os.path.basename(f))
    return used


if __name__ == '__main__':
    b = Bank(sys.argv[1])
    if sys.argv[2] == 'unused':
        out = sys.argv[3]
        maxsec = float(sys.argv[4]) if len(sys.argv) > 4 else 1e9
        os.makedirs(out, exist_ok=True)
        used = referenced(b.name)
        n = 0
        for k in range(b.n):
            if k in used or not b.valid(k) or b.seconds(k) > maxsec: continue
            if b.write(k, os.path.join(out, '%s_%03x.wav' % (b.name, k))): n += 1
        print('%s: %d waves, %d referenced, %d unreferenced written to %s' % (b.name, b.n, len([w for w in used if w < b.n]), n, out))
    elif sys.argv[2] == 'wave':
        print(b.write(int(sys.argv[3], 16), sys.argv[4]), '%.2f s' % b.seconds(int(sys.argv[3], 16)))
    elif sys.argv[2] == 'cues':
        print(sorted('%x' % w for w in xsb_waves(open(os.path.join(SND, sys.argv[3]), 'rb').read())))
