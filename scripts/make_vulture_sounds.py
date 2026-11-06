# make_vulture_sounds.py <chopride.wav> <JETENGIN.WAV> <jetengine.wav> <jet-powerdown.wav> -- the Vulture's engine
# sounds (user's stock sounds; the StarCraft originals could not be found) as GhostPC custom sounds, in game\ and
# assets\Sounds\custom:
#   ghostpc\vulture_idle.wav             chopride: the idle rumble while a Vulture is ridden (vehicles.cpp), very quiet
#   playerVulture\lwalk_solid.wav        jetengine: the engine loop while it moves (the game's own loop, pitched by
#                                        speed; customsound.cpp fades it in and out)
#   playerVulture\lwalk_solid_start.wav  JETENGIN: once when it starts moving (customsound.cpp: <mode>_start)
#   playerVulture\lwalk_solid_stop.wav   jet-powerdown: once when it stops, after the loop's fade-out (<mode>_stop;
#                                        truncated, faded out)
# Each is made 22050 Hz 16-bit mono and brought to a target loudness (RMS, dBFS); the loops get their ends crossfaded
# into their starts so they repeat without a click. (jet-powerdown came as .m4a: decode it to .wav first, e.g. with
# Windows' MediaTranscoder.)
import os
import sys
import wave

import numpy as np

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUTS = [os.path.join(ROOT, "game", "Sounds", "custom"), os.path.join(ROOT, "assets", "Sounds", "custom")]
RATE = 22050

# (source index, output, target RMS dBFS, loop, max seconds)
JOBS = [(0, "ghostpc/vulture_idle.wav", -30.0, True, None),        # user: very loud, must be a quiet rumble
        (2, "playerVulture/lwalk_solid.wav", -20.0, True, None),   # user: loud
        (1, "playerVulture/lwalk_solid_start.wav", -19.0, False, None),
        (3, "playerVulture/lwalk_solid_stop.wav", -19.0, False, 3.2)]   # user: long, may need truncating


def load(path):
    w = wave.open(path)
    n, ch, sw, sr = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
    raw = w.readframes(n)
    if sw == 1:
        a = (np.frombuffer(raw, np.uint8).astype(np.float64) - 128) / 128
    elif sw == 2:
        a = np.frombuffer(raw, np.int16).astype(np.float64) / 32768
    else:
        raise SystemExit("%s: %d-bit samples not handled" % (path, sw * 8))
    a = a.reshape(-1, ch).mean(axis=1)
    if sr != RATE:   # linear resampling
        t = np.arange(int(len(a) * RATE / sr)) * sr / RATE
        a = np.interp(t, np.arange(len(a)), a)
    return a - a.mean()


def crossfade_loop(a, seconds=0.12):
    """the last <seconds> blended into the first, then dropped: the end runs straight into the start"""
    k = int(seconds * RATE)
    head, tail = a[:k], a[-k:]
    ramp = np.linspace(0, 1, k)
    out = a[:-k].copy()
    out[:k] = tail * (1 - ramp) + head * ramp
    return out


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__ if __doc__ else "usage: make_vulture_sounds.py <chopride> <JETENGIN> <jetengine> <powerdown.wav>")
    for src, dst, target, loop, maxsec in JOBS:
        a = load(sys.argv[1 + src])
        if maxsec and len(a) > maxsec * RATE:
            a = a[:int(maxsec * RATE)]
            fade = int(0.8 * RATE)
            a[-fade:] *= np.linspace(1, 0, fade) ** 2
        if loop:
            a = crossfade_loop(a)
        rms = np.sqrt((a * a).mean())
        a *= 10 ** (target / 20) / rms
        peak = abs(a).max()
        if peak > 0.98:
            a *= 0.98 / peak
        pcm = (np.clip(a, -1, 1) * 32767).astype(np.int16).tobytes()
        for base in OUTS:
            out = os.path.join(base, *dst.split("/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with wave.open(out, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(RATE)
                w.writeframes(pcm)
        print("%-38s %.2f s, RMS %.1f dBFS%s" % (dst, len(a) / RATE, 20 * np.log10(np.sqrt((a * a).mean())), ", loop" if loop else ""))


if __name__ == "__main__":
    main()
