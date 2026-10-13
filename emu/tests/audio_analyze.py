#!/usr/bin/env python3
"""audio_analyze.py FILE.wav -> prints: channels rate seconds peak rms_dbfs and the strongest tones per channel."""
import sys, wave, json
import numpy as np

def analyze(path):
    with wave.open(path, 'rb') as w:
        ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
        raw = np.frombuffer(w.readframes(n), dtype='<i2').astype(np.float64)
    x = raw.reshape(-1, ch)
    out = {'channels': ch, 'rate': rate, 'seconds': round(len(x) / rate, 3), 'ch': []}
    for c in range(ch):
        s = x[:, c]
        if len(s) < 16:
            out['ch'].append({'peak': 0, 'rms_db': -999, 'tones': []}); continue
        win = np.hanning(len(s)); spec = np.abs(np.fft.rfft(s * win)); freqs = np.fft.rfftfreq(len(s), 1.0 / rate)
        # up to 3 strongest local peaks above 1% of the maximum
        tones = []
        sp = spec.copy()
        for _ in range(3):
            i = int(np.argmax(sp))
            if sp[i] < 0.01 * spec.max(): break
            tones.append((round(float(freqs[i])), round(float(sp[i] / (len(s) / 4)), 1)))   # approx amplitude (hanning)
            lo, hi = max(0, i - 40), min(len(sp), i + 40); sp[lo:hi] = 0
        rms = float(np.sqrt(np.mean(s ** 2)))
        out['ch'].append({'peak': int(np.max(np.abs(s))), 'rms_db': round(20 * np.log10(max(rms, 1e-9) / 32768), 1), 'tones': tones})
    return out

if __name__ == '__main__':
    print(json.dumps(analyze(sys.argv[1])))
