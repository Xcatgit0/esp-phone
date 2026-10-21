# Audio player (MP3/WAV + Lua API) — คู่มือภาษาไทย

เล่นเสียงจากไฟล์ (`/fs/test.mp3`) หรือสตรีมเครือข่าย (`http(s)://…`) ผ่าน I2S ไปยัง **MAX98357**
บนเครื่องจริง และผ่าน **PulseAudio (mono)** ใน emulator โค้ดส่วนที่เหมือนกันทั้งสองฝั่งคอมไพล์จากไฟล์เดียวกัน
ต่างกันเฉพาะ **decoder** (เลือกได้) และ **I2S backend**

## 1. โครงสร้าง

```
 ไฟล์ /fs/..   ┐
 http(s)://..  ┘ → audio_source ─► decoder ─► mono/stereo ─► resampler ─► EQ ─► volume ─► audio_out (I2S)
                   (ไฟล์/เครือข่าย)  (WAV, MP3)                                                 │
                                                                      บอร์ด: i2s_std → MAX98357 │ emu: PulseAudio mono
```

| ส่วน | ไฟล์ | บอร์ด | emulator |
|---|---|---|---|
| Lua API `audio.*` | `src/audio/lua_audio.c` | ใช้ | **ไฟล์เดียวกัน** |
| ตัวเล่น (task, resample, EQ, volume, สถานะ) | `src/audio/audio_player.c` | ใช้ | **ไฟล์เดียวกัน** |
| แหล่งข้อมูล ไฟล์ / HTTP(S) stream + ring buffer | `src/audio/audio_source.c` | ใช้ | **ไฟล์เดียวกัน** |
| ตรวจชนิดไฟล์ + เลือก decoder | `src/audio/audio_decoder.c` | ใช้ | **ไฟล์เดียวกัน** |
| decoder WAV | `src/audio/dec_wav.c` | ใช้ | **ไฟล์เดียวกัน** |
| **decoder MP3** (ค่าเริ่มต้น) | `src/audio/dec_mp3_minimp3.c` + `minimp3.h` | ใช้ | **ไฟล์เดียวกัน** |
| **decoder อื่นๆ** (ไม่บังคับ) | `src/audio/dec_esp_codec.c` (`-DAUDIO_USE_ESP_CODEC`) | MP3 AAC FLAC OGG M4A | — |
| **I2S backend** | `src/audio/out_i2s_esp.c` | ESP-IDF `i2s_std` | `emu/src/emu_audio_out.cpp` (PulseAudio/WAV/null) |

ประกาศ interface ชุดเดียวกัน: `include/audio_decoder.h`, `include/audio_out.h`, `include/audio_source.h`, `include/audio_player.h`
(ฝั่ง C ใช้ `audio_player_*` ได้เลยโดยไม่ผ่าน Lua เผื่อระบบอื่นใน OS)

## 2. ติดตั้งฝั่งบอร์ด

1. แตกไฟล์ทับโปรเจค (เพิ่ม `src/audio/`, `include/audio_*.h`, `include/lua_audio.h`)
2. `src/api.c` เพิ่ม 2 บรรทัด (ดู `patches/api.c.diff`):
   ```c
   #include "lua_audio.h"
   ...
   audio_register(L);      // ท้าย API_INIT
   ```
3. PlatformIO: Full Clean หนึ่งครั้ง (ไฟล์ใน `src/` ถูก glob ตอน configure)
4. ถ้า link หา `i2s_*` ไม่เจอ ให้เติม `REQUIRES esp_driver_i2s` (และ `esp_http_client mbedtls` ถ้ายังไม่ได้เติมจาก wifi.http) ใน `src/CMakeLists.txt`
5. (ไม่บังคับ) เล่นรูปแบบอื่นด้วย esp_audio_codec: เพิ่ม `build_flags = -DAUDIO_USE_ESP_CODEC -DAUDIO_TASK_STACK=12288`
   และสร้าง `src/idf_component.yml`
   ```yaml
   dependencies:
     espressif/esp_audio_codec: "^2.4.0"
   ```
   โค้ดตรวจกับหัวไฟล์จริงของ esp_audio_codec 2.6.x แล้ว แต่**ยังไม่ได้รันบนบอร์ด**

### ต่อสาย MAX98357

| MAX98357 | ESP32-S3 | หมายเหตุ |
|---|---|---|
| BCLK | GPIO5 | |
| LRC (WS) | GPIO6 | |
| DIN | GPIO4 | |
| VIN / GND | 3.3–5 V / GND | |
| SD_MODE, GAIN | ตาม datasheet | SD_MODE เลือกช่องซ้าย/ขวา/ผสม (L+R)/2 · GAIN เลือกเกน |

เช็กแล้วว่า GPIO 4/5/6 ไม่ชนกับพินที่โปรเจคใช้อยู่ (TFT 7/15/16/17/18, touch 1/2/41/42, ปุ่ม 9/10/11, joystick 3/8/12/13/14/46)
เปลี่ยนพินได้ที่ `AUDIO_I2S_PIN_*` ใน `include/audio_out.h`

บอร์ดส่ง **stereo** เสมอ ค่าเริ่มต้น `audio.mono(true)` ผสม (L+R)/2 ใส่ทั้งสองช่อง ลำโพงตัวเดียวจึงได้ครบทุกช่องไม่ว่า SD_MODE เลือกช่องไหน

## 3. ติดตั้งฝั่ง emulator

```sh
sudo apt install libpulse-dev libcurl4-openssl-dev      # pulse = เสียง, curl = network source
cd emu && cmake -B build && cmake --build build -j
./build/phone_emu                       # เสียงออก PulseAudio (mono)
./build/phone_emu --audio null          # ไม่มีเสียง แต่เดินเวลาเหมือนเครื่องจริง
./build/phone_emu --audio wav:out.wav   # อัดสิ่งที่ PulseAudio จะได้ (mono)
./build/phone_emu --audio wav-stereo:out.wav   # อัดสิ่งที่บอร์ดส่งออก I2S (stereo)
```

- WSL2 บน Windows 11 มี PulseAudio ผ่าน WSLg อยู่แล้ว (ถ้าไม่มี emulator จะเตือนแล้วใช้ `null`)
- `/fs/test.mp3` ใน emulator ชี้ไป `<root>/data/test.mp3`

## 4. Lua API

```lua
audio.play("/fs/test.mp3")                   -- true | nil, err
audio.volume(0.6)                            -- 0..1 (เส้นตรง) คืนค่าปัจจุบัน
audio.pause()  audio.resume()  audio.stop()
audio.wait()                                 -- หลับจนเพลงจบ: "finished" | "stopped" | "error", ข้อความ
audio.status()                               -- {state, uri, codec, position, duration, sample_rate, channels, out_rate, volume, mono, buffered, error}

-- เล่นจากเครือข่าย (ต้องต่อ wifi.sta ให้ได้ IP ก่อน)
audio.play("https://example.com/song.mp3", {
    timeout   = 15000,                       -- ms
    prebuffer = 32768,                       -- ไบต์ที่เก็บก่อนเริ่มเล่น
    headers   = { Authorization = "Bearer …" },
    ca_cert   = PEM,                         -- เซิร์ฟเวอร์ที่เซ็นใบรับรองเอง
})
audio.play("/fs/loop.wav", { loop = true })  -- ไฟล์เท่านั้น
```

`audio.play` คืน `true` ทันที ถ้าไฟล์เปิดไม่ได้หรือไม่มีเครือข่ายจะคืน `nil, err` ส่วนปัญหาที่เกิดทีหลัง (เซิร์ฟเวอร์ตอบ 404,
รูปแบบไฟล์ไม่รองรับ) จะมาทาง `audio.wait()` / `audio.status().error`

### ปรับความถี่ (EQ) — ผมตีความคำว่า "บีบได้ในความถี่ต่าง ๆ" ว่าเป็นอีควอไลเซอร์

ถ้าหมายถึงอย่างอื่น (เช่น compressor) บอกได้ครับ

```lua
audio.eq({                                   -- ได้สูงสุด 5 แถบ, แทนที่ทั้งชุด
  { type = "peak",      freq = 1000, gain = -12, q = 2 },   -- ลดแถวความถี่ ~1 kHz ลง 12 dB
  { type = "lowshelf",  freq = 150,  gain = 4 },            -- เพิ่มเบส
  { type = "highpass",  freq = 80 },                        -- ตัดความถี่ต่ำกว่า 80 Hz
})
audio.eq_band(2, 6000, -6, 1.5)              -- แก้แถบที่ 2 แถบเดียว (freq, gain dB, q, type)
audio.eq_band(2, nil)                        -- ปิดแถบที่ 2
audio.eq_reset()                             -- ราบเรียบ
audio.eq()                                   -- อ่านค่าปัจจุบัน
```
ชนิด: `peak` (ยก/ลดรอบความถี่) · `lowshelf` · `highshelf` · `lowpass` · `highpass` · `notch` (บีบความถี่เดียวทิ้ง)
ช่วงที่รับ: freq 20 Hz – 45% ของ sample rate ขาออก, gain ±24 dB, q 0.1–20

อื่นๆ: `audio.mono(false)` ส่งสเตอริโอจริง · `audio.rate(48000)` เปลี่ยน sample rate ขาออก (8000–48000, เปลี่ยนได้ตอนหยุด; ค่าเริ่มต้น 44100)
เพลงทุกไฟล์ถูก resample เป็นอัตรานี้ · `audio.codecs()` รายการที่ build นี้ถอดรหัสได้

## 5. ระบบทำงานอย่างไร (สรุป)

- task `audio` ตัวเดียวทำ ถอดรหัส → resample → EQ → volume → เขียน I2S (DMA ~70 ms คือ buffer กันกระตุก)
- สตรีมเครือข่ายมี task อ่านแยกลง ring buffer 128 KB (อยู่ใน PSRAM) และ prebuffer ก่อนเล่น เน็ตสะดุดเสียงจะเงียบแทนที่จะค้าง
- เปลี่ยน volume/EQ/mono ได้ระหว่างเล่น ตอบสนองภายในราว 1 DMA buffer; volume ไล่ระดับ ~10 ms ไม่มีเสียงแปะ
- `audio.pause()` หยุดหลัง buffer ที่ส่งไปแล้วหมด (หน่วงได้ ~70–100 ms)

## 6. หมายเหตุและข้อจำกัด

- **ไม่รองรับ seek** และ MP3 ไม่ทราบความยาว (`duration` เป็น nil) WAV ทราบ
- **resampler เป็น linear interpolation** เบา แต่ถ้าลด sample rate มากๆ (เช่น 96 kHz → 44.1 kHz) จะมี aliasing ให้ตั้ง `audio.rate()` ให้ใกล้ไฟล์ที่เล่น
- **ไม่ gapless**: MP3 มีช่องเงียบสั้นๆ ตอนเริ่ม/จบจากตัวเข้ารหัส และ `loop=true` มีช่องว่างตอนเปิดไฟล์ใหม่ (WAV ไม่มีปัญหานี้)
- **ID3v2 ถูกข้าม** (ไม่อ่านชื่อเพลง/ศิลปิน) · ICY metadata ในสตรีมวิทยุไม่รองรับ · "ICY 200 OK" ของ Shoutcast v1 อาจถูก esp_http_client บนบอร์ดปฏิเสธ (emulator รับ)
- **minimp3 ใช้ stack ~17 KB** ของ task จึงตั้ง `AUDIO_TASK_STACK` ไว้ 32 KB (internal RAM); ถ้าใช้ esp_audio_codec ลดเหลือ ~12 KB ได้
- **ยังไม่ได้วัด CPU ของ minimp3 บน ESP32-S3 ที่ 160 MHz** และ config ของคุณคอมไพล์แบบ `-Og` ซึ่งช้าลงมาก: ถ้าเสียงสะดุดให้เปลี่ยนเป็น `-O2` (เหมือนที่แนะนำไว้ตอนวัด Lua)
  หรือใช้ esp_audio_codec (มี assembly สำหรับ S3) หรือเล่น MP3 บิตเรตต่ำ/mono
- volume เป็นแอมพลิจูดเส้นตรง ความดังที่ได้ยินไม่เป็นเส้นตรง ถ้าอยากได้ curve (เช่น v²) บอกได้
- HTTPS บนบอร์ดใช้ certificate bundle ของ ESP-IDF; emulator ใช้ CA ของเครื่อง PC จึงอาจต่างกันได้
- ใน emulator ความเร็วคำนวณเร็วกว่าบอร์ดมาก จึง**ใช้ตัดสินเรื่อง CPU/การสะดุดไม่ได้** ต้องลองบนบอร์ดจริง

## 7. เขียน decoder / backend ของตัวเอง

- **decoder**: ทำ `audio_decoder_ops_t` (`open/decode/duration_ms/close`, ดูท้าย `include/audio_decoder.h`) แล้วเพิ่มใน `ops_for()` ของ `audio_decoder.c`
  โมเดลเป็น *push decoder*: ป้อนไบต์เท่าที่มี ได้ PCM กลับมา และบอกว่า "ใช้ไปกี่ไบต์ / ต้องการข้อมูลเพิ่ม"
- **backend**: เขียน `audio_out_open/write/drain/close` (ดู `include/audio_out.h`) รับ stereo int16 แล้วส่งต่อ (I2S, ไฟล์, ฯลฯ) `write` ควร block เมื่อ buffer เต็ม เพราะนั่นคือสิ่งที่ควบคุมจังหวะการเล่น

## 8. สถานะการทดสอบ

ทดสอบใน emulator ผ่านทั้งหมด (`emu/tests/audio_test.py`, ตรวจเสียงที่บันทึกด้วย numpy): ถอดรหัส WAV/MP3, resample 22.05k/44.1k/48k,
volume, downmix mono/stereo, EQ ทุกชนิด (วัดระดับ dB ที่ความถี่ทดสอบ), pause/resume/stop/replace/loop, ข้อผิดพลาด, สตรีม HTTP (Content-Length, redirect,
header, 404, สตรีมไม่สิ้นสุด, หยุดระหว่างเชื่อมต่อ, เน็ตสะดุดกลางทาง) และเล่นผ่าน PulseAudio จริง (null sink) ได้ระดับและความถี่ตรง
โค้ดฝั่งบอร์ดคอมไพล์ผ่านแบบไม่มี warning กับหัวไฟล์จริงของ ESP-IDF 6.0.1 และ esp_audio_codec แต่**ยังไม่ได้ลองบนฮาร์ดแวร์**

## audio.getVU()

```lua
local l, r = audio.getVU()   -- ระดับเสียงขาออกตอนนี้ แยกซ้าย/ขวา ช่วง 0..1 (peak, เชิงเส้น)
```

- วัดจากสัญญาณที่ส่งเข้าแอมป์จริง (หลัง EQ และ volume) ถ้าลด `audio.volume` แท่งก็จะเตี้ยลงตาม
- ทุก 512 เฟรม (~11 ms) ระบบเก็บ peak ของช่วงนั้น แล้วค่าจะค่อย ๆ ลดลงเองด้วยค่าคงที่ ~220 ms (ปรับที่ `AUDIO_VU_RELEASE_MS`) จึงเรียกกี่ครั้งก็ได้ ค่าเรียบ ไม่กระตุก
- หยุด/พัก/เน็ตสะดุด ค่าจะลดลงเป็น 0 เอง
- โหมด `audio.mono(true)` ทั้งสองค่าจะเท่ากัน
- เสียงมาถึงลำโพงช้ากว่าค่าที่อ่านได้เล็กน้อย (DMA buffer ~70 ms) แต่ยังสัมพันธ์กับจังหวะเพลงดี
- แปลงเป็น dB: `20 * math.log(v, 10)` (ระวัง v = 0)
