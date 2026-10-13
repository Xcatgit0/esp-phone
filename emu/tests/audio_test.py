#!/usr/bin/env python3
"""Headless audio tests: python3 tests/audio_test.py [path/to/phone_emu]
Runs the emulator with the WAV sink, then checks the recorded output (duration, tones, levels) with numpy."""
import json, os, subprocess, sys, tempfile, textwrap, wave, time
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from audio_analyze import analyze

EMU = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'build', 'phone_emu')
TMP = tempfile.mkdtemp(prefix='emu_audio_')
fails = 0

def check(name, cond, detail=''):
    global fails
    if cond: print('PASS ', name)
    else: fails += 1; print('FAIL ', name, ('  (%s)' % (detail,)) if detail != '' else '')

def write_wav(name, rate, ch, data):
    p = os.path.join(TMP, name)
    with wave.open(p, 'wb') as w:
        w.setnchannels(ch); w.setsampwidth(2); w.setframerate(rate); w.writeframes(data.astype('<i2').tobytes())
    return p

def tone(rate, sec, f, amp): return amp * 32767 * np.sin(2 * np.pi * f * np.arange(int(rate * sec)) / rate)

mono22 = write_wav('mono22k.wav', 22050, 1, tone(22050, 2, 1000, 0.5))
L, R = tone(44100, 2, 1000, 0.5), tone(44100, 2, 2000, 0.5)
stereo44 = write_wav('stereo44k.wav', 44100, 2, np.stack([L, R], 1).reshape(-1))
mix48 = write_wav('mix48k.wav', 48000, 1, tone(48000, 2, 100, 0.25) + tone(48000, 2, 1000, 0.25) + tone(48000, 2, 6000, 0.25))
mp3 = os.path.join(TMP, 'stereo44k.mp3')
have_mp3 = subprocess.run(['lame', '-S', '-b', '128', stereo44, mp3], capture_output=True).returncode == 0
junk = os.path.join(TMP, 'junk.bin'); open(junk, 'wb').write(b'this is not audio ' * 500)
badwav = os.path.join(TMP, 'bad.wav'); open(badwav, 'wb').write(b'this is not audio ' * 500)

import shutil, socket
PORT = 18081
for f in ('mono22k.wav', 'stereo44k.wav', 'stereo44k.mp3'):
    if os.path.exists(os.path.join(TMP, f)): pass
srv_env = dict(os.environ, AUDIO_ROOT=TMP)
srv = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'http_server.py'), str(PORT)], env=srv_env)
for _ in range(50):
    try: socket.create_connection(('127.0.0.1', PORT), timeout=0.2).close(); break
    except OSError: time.sleep(0.1)
import atexit; atexit.register(srv.kill)

def run(lua, sink='wav', timeout=60, extra=()):
    """returns (stdout lines, analysis dict or None). Lua may use PATHS['name'] and must call emu.quit() at the end."""
    script = os.path.join(TMP, 'case.lua'); out = os.path.join(TMP, 'out.wav')
    if os.path.exists(out): os.remove(out)
    pre = 'local P = { mono22=%r, stereo44=%r, mix48=%r, mp3=%r, junk=%r, badwav=%r, base=%r }\n' % (mono22, stereo44, mix48, mp3, junk, badwav, 'http://127.0.0.1:%d' % PORT)
    open(script, 'w').write(pre + textwrap.dedent(lua))
    spec = ('wav-stereo:' if sink == 'stereo' else 'wav:') + out
    t0 = time.time()
    r = subprocess.run([EMU, '--headless', '--run', script, '--audio', spec, *extra], capture_output=True, text=True, timeout=timeout)
    lines = [l for l in r.stdout.splitlines() if not l.startswith(('I (', 'W ('))]
    for l in r.stderr.splitlines():
        if 'NOTE' in l: print('      ', l)          # the device hides Lua compile errors; the emulator prints them
    res = analyze(out) if os.path.exists(out) and os.path.getsize(out) > 44 else None
    return lines, res, time.time() - t0, r.stderr

def tone_amp(res, ch, f, tol=60):
    for fr, amp in res['ch'][ch]['tones']:
        if abs(fr - f) <= tol: return amp
    return 0.0

def lines_ok(lines): return all(not l.startswith('FAIL') for l in lines) and any(l.startswith('DONE') for l in lines)

LUA_HELPERS = '''
local function ok(n, c, d) print((c and "PASS " or "FAIL ") .. n .. (d and ("  (" .. tostring(d) .. ")") or "")) end
'''

# ---- 1. WAV mono 22.05 kHz -> 44.1 kHz, volume 1
lines, res, wall, _ = run(LUA_HELPERS + '''
audio.volume(1.0)
local a, e = audio.play(P.mono22)
ok("play returns true", a == true, e)
ok("status is buffering/playing right away", ({buffering=1, playing=1})[audio.status().state] ~= nil, audio.status().state)
ok("wait -> finished", audio.wait(6000) == "finished")
local s = audio.status()
ok("status after: stopped, wav 22050 mono, 2 s", s.state == "stopped" and s.codec == "wav" and s.sample_rate == 22050 and s.channels == 1 and s.duration == 2000, s.state)
print("DONE") emu.quit()''')
check('S1 lua checks', lines_ok(lines), lines)
check('S1 output 44.1 kHz mono, 2.0 s', res and res['rate'] == 44100 and res['channels'] == 1 and abs(res['seconds'] - 2.0) < 0.06, res and (res['rate'], res['seconds']))
check('S1 1 kHz tone, peak ~ 0.5 FS (resampler keeps level)', res and abs(tone_amp(res, 0, 1000) - 16383) < 600 and abs(res['ch'][0]['peak'] - 16383) < 600, res and res['ch'][0])
check('S1 played in real time (2.0-3.5 s)', 2.0 <= wall <= 3.5, round(wall, 2))

# ---- 2. volume 0.5 and 0.25, and volume() validation
lines, res, _, _ = run(LUA_HELPERS + '''
ok("volume default is readable", audio.volume() == 0.5, audio.volume())
ok("volume(0.25) returns 0.25", audio.volume(0.25) == 0.25)
local r, e = audio.volume(1.5); ok("volume(1.5) refused", r == nil and e ~= nil, e)
ok("volume unchanged after refusal", audio.volume() == 0.25)
audio.play(P.mono22); audio.wait(6000)
print("DONE") emu.quit()''')
check('S2 lua checks', lines_ok(lines), lines)
check('S2 volume 0.25 -> peak ~ 4096', res and abs(res['ch'][0]['peak'] - 4096) < 250, res and res['ch'][0]['peak'])

# ---- 3. MP3 (stereo 128k) -> mono output
if have_mp3:
    lines, res, _, _ = run(LUA_HELPERS + '''
    audio.volume(1.0)
    ok("mp3 listed", table.concat(audio.codecs(), ","):find("mp3") ~= nil)
    ok("play mp3", audio.play(P.mp3) == true)
    ok("wait finished", audio.wait(8000) == "finished")
    local s = audio.status()
    ok("codec mp3 44100 stereo", s.codec == "mp3" and s.sample_rate == 44100 and s.channels == 2, s.codec)
    print("DONE") emu.quit()''')
    check('S3 lua checks', lines_ok(lines), lines)
    check('S3 mp3 decoded to ~2 s of mono', res and res['channels'] == 1 and 1.9 < res['seconds'] < 2.2, res and res['seconds'])
    check('S3 both tones present after downmix (1 kHz and 2 kHz ~ 8192)', res and abs(tone_amp(res, 0, 1000) - 8192) < 1200 and abs(tone_amp(res, 0, 2000) - 8192) < 1200,
          res and res['ch'][0]['tones'])
else:
    print('SKIP  S3 (lame not installed)')

# ---- 4. device stereo path: wav-stereo sink shows what goes to the MAX98357
lines, res, _, _ = run(LUA_HELPERS + '''
audio.volume(1.0); audio.mono(false)
ok("mono(false)", audio.mono() == false)
audio.play(P.stereo44); audio.wait(6000)
print("DONE") emu.quit()''', sink='stereo')
check('S4 lua checks', lines_ok(lines), lines)
check('S4 stereo kept: L = 1 kHz only, R = 2 kHz only', res and res['channels'] == 2 and tone_amp(res, 0, 1000) > 15000 and tone_amp(res, 0, 2000) < 300
      and tone_amp(res, 1, 2000) > 15000 and tone_amp(res, 1, 1000) < 300, res and [c['tones'] for c in res['ch']])
lines, res, _, _ = run(LUA_HELPERS + '''
audio.volume(1.0)
ok("mono default true", audio.mono() == true)
audio.play(P.stereo44); audio.wait(6000)
print("DONE") emu.quit()''', sink='stereo')
check('S4b mono mode: both I2S channels carry (L+R)/2', res and abs(tone_amp(res, 0, 1000) - 8192) < 800 and abs(tone_amp(res, 1, 1000) - 8192) < 800
      and abs(tone_amp(res, 0, 2000) - 8192) < 800, res and [c['tones'] for c in res['ch']])

# ---- 5. EQ: notch / peak cut / high-pass on a 100 Hz + 1 kHz + 6 kHz mix (48 kHz source)
def eq_case(setup):
    lines, res, _, _ = run(LUA_HELPERS + 'audio.volume(1.0)\n' + setup + '\naudio.play(P.mix48); audio.wait(6000)\nprint("DONE") emu.quit()')
    return lines, res
lines, base = eq_case('')
a100, a1k, a6k = [tone_amp(base, 0, f) for f in (100, 1000, 6000)] if base else (0, 0, 0)
check('S5 baseline has the 3 tones at similar level', base and min(a100, a1k, a6k) > 5000 and max(a100, a1k, a6k) / min(a100, a1k, a6k) < 1.25, (a100, a1k, a6k))
db = lambda a, b: 20 * np.log10(max(a, 1e-6) / max(b, 1e-6))
lines, res = eq_case('ok("eq peak -24 dB @1k", audio.eq({ {type="peak", freq=1000, gain=-24, q=2} }) == true)')
check('S5 peak -24 dB at 1 kHz squeezes it ~24 dB, others untouched', res and lines_ok(lines) and db(tone_amp(res, 0, 1000), a1k) < -18 and abs(db(tone_amp(res, 0, 100), a100)) < 1.5 and abs(db(tone_amp(res, 0, 6000), a6k)) < 1.5,
      res and (db(tone_amp(res, 0, 1000), a1k), db(tone_amp(res, 0, 100), a100), db(tone_amp(res, 0, 6000), a6k)))
lines, res = eq_case('audio.eq_band(1, 1000, 0, 4, "notch")')
check('S5 notch at 1 kHz removes it', res and db(tone_amp(res, 0, 1000), a1k) < -30, res and db(tone_amp(res, 0, 1000), a1k))
lines, res = eq_case('audio.eq({ {type="highpass", freq=3000, q=0.707} })')
check('S5 high-pass 3 kHz: 6 kHz stays, 100 Hz and 1 kHz are gone', res and abs(db(tone_amp(res, 0, 6000), a6k)) < 2 and db(tone_amp(res, 0, 100), a100) < -30 and db(tone_amp(res, 0, 1000), a1k) < -12,
      res and (db(tone_amp(res, 0, 6000), a6k), db(tone_amp(res, 0, 100), a100), db(tone_amp(res, 0, 1000), a1k)))
lines, res = eq_case('audio.eq({ {type="lowshelf", freq=300, gain=-18} })')
check('S5 low shelf -18 dB: 100 Hz down ~18 dB, 6 kHz unchanged', res and -22 < db(tone_amp(res, 0, 100), a100) < -14 and abs(db(tone_amp(res, 0, 6000), a6k)) < 1.5, res and db(tone_amp(res, 0, 100), a100))
lines, res = eq_case('audio.eq({ {freq=1000, gain=-24} }); audio.eq_reset()')
check('S5 eq_reset() restores a flat response', res and abs(db(tone_amp(res, 0, 1000), a1k)) < 0.5, res and db(tone_amp(res, 0, 1000), a1k))
lines, _ = eq_case('''
local e1 = select(2, audio.eq({ {freq=5, gain=0} }));        ok("freq too low refused", e1 ~= nil and e1:find("freq"), e1)
local e2 = select(2, audio.eq({ {freq=1000, gain=40} }));    ok("gain 40 refused", e2 ~= nil and e2:find("gain"), e2)
local e3 = select(2, audio.eq({ {freq=1000, type="wobble"} })); ok("bad type refused", e3 ~= nil and e3:find("type"), e3)
local e4 = select(2, audio.eq({ {freq=100},{freq=200},{freq=300},{freq=400},{freq=500},{freq=600} })); ok("6 bands refused", e4 ~= nil, e4)
ok("eq() reads back the bands", (function() audio.eq({ {type="notch", freq=1234, gain=0, q=3} }); local b = audio.eq(); return #b == 1 and b[1].type == "notch" and b[1].freq == 1234 end)())
audio.eq_reset()''')
check('S5 EQ argument validation', lines_ok(lines), lines)

# ---- 6. pause / resume / stop / replace
lines, res, wall, _ = run(LUA_HELPERS + '''
audio.volume(1.0)
audio.play(P.mono22)
emu.sleep(500); audio.pause()
ok("state paused", audio.status().state == "paused", audio.status().state)
local p0 = audio.status().position
emu.sleep(1000)
ok("position frozen while paused (pause latency <= ~DMA buffer)", math.abs(audio.status().position - p0) < 200, audio.status().position - p0)
audio.resume()
ok("state playing again", audio.status().state == "playing", audio.status().state)
ok("wait -> finished", audio.wait(6000) == "finished")
print("DONE") emu.quit()''')
check('S6 pause/resume lua checks', lines_ok(lines), lines)
check('S6 output keeps the full 2 s; wall time shows the 1 s pause', res and abs(res['seconds'] - 2.0) < 0.1 and 2.8 < wall < 4.5, res and (res['seconds'], round(wall, 2)))
lines, res, wall, _ = run(LUA_HELPERS + '''
audio.volume(1.0)
audio.play(P.mono22)
emu.sleep(600)
audio.stop()
ok("state stopped at once", audio.status().state == "stopped")
ok("wait after stop -> stopped", audio.wait(1000) == "stopped")
print("DONE") emu.quit()''')
check('S6b stop lua checks', lines_ok(lines), lines)
check('S6b output cut short (~0.6 s + buffer)', res and 0.4 < res['seconds'] < 1.0, res and res['seconds'])
lines, res, _, _ = run(LUA_HELPERS + '''
audio.volume(1.0)
audio.play(P.mono22); emu.sleep(300)
ok("second play replaces the first", audio.play(P.stereo44) == true)
ok("wait -> finished (the new one)", audio.wait(6000) == "finished")
ok("status shows the new file", audio.status().sample_rate == 44100 and audio.status().channels == 2, audio.status().sample_rate)
print("DONE") emu.quit()''')
check('S6c replace lua checks', lines_ok(lines), lines)
check('S6c output = 0.3 s of the first + 2 s of the second', res and 2.1 < res['seconds'] < 2.6, res and res['seconds'])

# ---- 7. loop
lines, res, _, _ = run(LUA_HELPERS + '''
audio.volume(1.0)
audio.play(P.mono22, { loop = true })
emu.sleep(4600); ok("still playing after 4.6 s (file is 2 s)", audio.status().state == "playing", audio.status().state)
audio.stop() print("DONE") emu.quit()''')
check('S7 loop lua checks', lines_ok(lines), lines)
check('S7 loop output > 4 s and tone continuous', res and res['seconds'] > 4.0 and abs(tone_amp(res, 0, 1000) - 16383) < 900, res and (res['seconds'], res['ch'][0]['tones']))

# ---- 8. errors
lines, _, _, _ = run(LUA_HELPERS + '''
local r, e = audio.play("/nonexistent/file.mp3"); ok("missing file -> nil, err", r == nil and e:find("cannot open"), e)
ok("junk file accepted by play()", audio.play(P.junk) == true)
local why, msg = audio.wait(3000)
ok("junk file -> wait returns error", why == "error" and msg and msg:find("unknown audio format"), tostring(why) .. " " .. tostring(msg))
ok("status.state == error", audio.status().state == "error" and audio.status().error ~= nil, audio.status().state)
audio.play(P.badwav)
local why2, msg2 = audio.wait(3000)
ok("a .wav that is not a WAV -> decode error", why2 == "error" and msg2 and msg2:find("decode error"), tostring(why2) .. " " .. tostring(msg2))
local r2, e2 = audio.play("http://127.0.0.1:1/x.mp3"); ok("url without network -> nil, err", r2 == nil and e2:find("no network"), e2)
ok("rate(48000) while stopped", audio.rate(48000) == 48000)
ok("rate(100) refused", audio.rate(100) == nil)
audio.rate(44100)
audio.play(P.mono22)
local r3, e3 = audio.rate(22050); ok("rate refused while playing", r3 == nil, e3)
audio.stop()
ok("play(nil) raises", not pcall(audio.play))
ok("bad option types return errors", select(2, audio.play(P.mono22, { timeout = "x" })) ~= nil)
audio.stop()
print("DONE") emu.quit()''')
check('S8 error handling', lines_ok(lines), lines)

# ---- 9. output rate 48 kHz
lines, res, _, _ = run(LUA_HELPERS + '''
audio.volume(1.0); audio.rate(48000)
audio.play(P.stereo44); ok("wait", audio.wait(6000) == "finished")
ok("status.out_rate", audio.status().out_rate == 48000)
print("DONE") emu.quit()''')
check('S9 output rate 48 kHz honoured', lines_ok(lines) and res and res['rate'] == 48000 and abs(res['seconds'] - 2.0) < 0.08, res and (res['rate'], res['seconds']))

# ================= network source =================
NET = LUA_HELPERS + '''
wifi.mode("sta"); wifi.start(); wifi.sta.config({ ssid = "emu", password = "" }); wifi.sta.connect()
local t0 = time.millis(); repeat delay(50) until wifi.sta.status().ip or time.millis() - t0 > 3000
audio.volume(1.0)
'''
lines, res, wall, _ = run(NET + '''
ok("play http wav", audio.play(P.base .. "/file/stereo44k.wav") == true)
ok("wait -> finished", audio.wait(10000) == "finished")
local s = audio.status()
ok("status: wav 44100 stereo, duration 2000", s.codec == "wav" and s.sample_rate == 44100 and s.channels == 2 and s.duration == 2000, s.codec)
print("DONE") emu.quit()''')
check('S10 lua checks (http wav)', lines_ok(lines), lines)
check('S10 streamed output = 2 s, both tones', res and abs(res['seconds'] - 2.0) < 0.1 and abs(tone_amp(res, 0, 1000) - 8192) < 1200 and abs(tone_amp(res, 0, 2000) - 8192) < 1200, res and (res['seconds'], res['ch'][0]['tones']))
if have_mp3:
    lines, res, _, _ = run(NET + '''
    audio.play(P.base .. "/redir-file/stereo44k.mp3")
    ok("wait -> finished (through a 302)", audio.wait(10000) == "finished")
    ok("codec mp3", audio.status().codec == "mp3", audio.status().codec)
    print("DONE") emu.quit()''')
    check('S11 lua checks (http mp3 via redirect)', lines_ok(lines), lines)
    check('S11 mp3 over http decoded', res and 1.9 < res['seconds'] < 2.2 and abs(tone_amp(res, 0, 1000) - 8192) < 1200, res and (res['seconds'], res['ch'][0]['tones']))
lines, res, _, _ = run(NET + '''
audio.play(P.base .. "/token-file"); local w, m = audio.wait(5000)
ok("without the header: 403 -> error", w == "error" and m:find("403"), tostring(w) .. " " .. tostring(m))
ok("with headers option", audio.play(P.base .. "/token-file", { headers = { ["X-Token"] = "abc" } }) == true)
ok("wait -> finished", audio.wait(10000) == "finished")
audio.play(P.base .. "/file/nope.mp3"); w, m = audio.wait(5000)
ok("404 -> error", w == "error" and m:find("404"), tostring(w) .. " " .. tostring(m))
audio.play("http://127.0.0.1:1/x.mp3"); w, m = audio.wait(8000)
ok("connection refused -> error", w == "error" and m:find("cannot connect"), tostring(w) .. " " .. tostring(m))
ok("bad headers option", select(2, audio.play(P.base .. "/file/x", { headers = { ["X-A"] = "1\\r\\n2" } })) ~= nil)
print("DONE") emu.quit()''')
check('S12 http errors + headers', lines_ok(lines), lines)
check('S12 the authenticated file played (1 kHz mono)', res and abs(res['seconds'] - 2.0) < 0.1 and tone_amp(res, 0, 1000) > 15000, res and res['seconds'])
t_stop = time.time()
lines, res, wall, _ = run(NET + '''
audio.play(P.base .. "/radio")
emu.sleep(3500)
local s = audio.status()
ok("endless stream is playing", s.state == "playing" and s.codec == "wav" and s.sample_rate == 22050 and s.duration == nil, s.state)
ok("buffered bytes reported", type(s.buffered) == "number")
local t1 = time.millis()
audio.stop()
ok("stop() returns at once", time.millis() - t1 < 100)
ok("wait -> stopped", audio.wait(2000) == "stopped")
ok("replay works right after a stop", audio.play(P.mono22) == true)
audio.wait(5000)
print("DONE") emu.quit()''')
check('S13 radio lua checks', lines_ok(lines), lines)
check('S13 radio output: ~3 s of a 500 Hz tone, then the 2 s file', res and 4.5 < res['seconds'] < 6.5 and tone_amp(res, 0, 500) > 8000, res and (res['seconds'], res['ch'][0]['tones']))
check('S13 total run is short (stream torn down, no 30 s wait)', wall < 14, round(wall, 1))
lines, _, wall, _ = run(NET + '''
audio.play(P.base .. "/stall-start")
emu.sleep(400)
ok("state buffering while connecting", audio.status().state == "buffering", audio.status().state)
local t1 = time.millis(); audio.stop()
ok("stop() while buffering is immediate", time.millis() - t1 < 100 and audio.status().state == "stopped")
ok("wait -> stopped", audio.wait(1000) == "stopped")
ok("a new play works while the old connect is still pending", audio.play(P.mono22) == true)
ok("... and finishes", audio.wait(8000) == "finished")
print("DONE") emu.quit()''')
check('S14 stop during connect', lines_ok(lines), lines)
lines, res, wall, _ = run(NET + '''
audio.play(P.base .. "/stall-mid", { prebuffer = 4096 })
ok("wait -> finished despite a 1.5 s stall", audio.wait(12000) == "finished")
print("DONE") emu.quit()''')
check('S15 network stall tolerated', lines_ok(lines), lines)
check('S15 all 2 s arrived after the stall', res and abs(res['seconds'] - 2.0) < 0.15, res and res['seconds'])

print('\nALL PASSED' if fails == 0 else '\n%d FAILED' % fails)
sys.exit(1 if fails else 0)
