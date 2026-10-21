# fs API (ระบบไฟล์)

ใช้ POSIX (`opendir/stat/mkdir/rename/unlink`) ผ่าน VFS ของ ESP-IDF จึงใช้ได้กับทุกไดรฟ์ที่เมาท์ไว้
(`/fs`, `/usb`, `/sdcard` ฯลฯ) โค้ดชุดเดียวกันบนเครื่องจริงและใน emulator

## ใช้งาน

```lua
for _, e in ipairs(fs.list("/usb/music")) do        -- โฟลเดอร์ขึ้นก่อน แล้วเรียงชื่อ
    print(e.type, e.name, e.size)
end
local t = fs.list("/")           -- รายชื่อจุดเมาท์ที่ "มีอยู่ตอนนี้" (เช่น fs, usb) ใช้ทำตัวเลือกไดรฟ์ใน file manager
local list, cut = fs.list("/usb", {size=false, max=200})   -- size=false เร็วกว่ามากบน USB; cut=true ถ้าเกิน max
fs.mkdir("/fs/a/b/c", true)      -- true = สร้างโฟลเดอร์แม่ให้ด้วย
fs.write("/fs/a/x.txt", "hi");  fs.append("/fs/a/x.txt", "!")
print(fs.read("/fs/a/x.txt"))    -- จำกัด 256 KB (ใส่ fs.read(p, n) เพื่อเปลี่ยน)
fs.copy("/usb/a.mp3", "/fs/a.mp3"); fs.rename(a, b); fs.remove("/fs/a", true)
audio.play(fs.join("/usb/music", e.name))
```

ทุกฟังก์ชันที่พลาดจะคืน `nil, "ข้อความ"` (เช่น `No such file or directory`)

## ข้อควรรู้
- path ต้องขึ้นต้นด้วย `/` ยาวไม่เกิน 255 ตัว และห้ามมี segment `..`
- `fs.list` ไม่ใส่ `.` กับ `..` ให้ ใช้หน่วยความจำตามจำนวนรายการ (ค่าเริ่มต้นสูงสุด 1000)
- ไดรฟ์ USB ช้า: ใช้ `size=false` ตอนแสดงรายการยาว ๆ แล้วค่อย `fs.stat` เฉพาะไฟล์ที่ต้องการ
- ถอด USB ระหว่างเล่นเพลง ให้ `audio.stop()` ก่อน
- ถ้าระบบไฟล์เป็น SPIFFS (ไม่มีโฟลเดอร์จริง) `mkdir/list` ของโฟลเดอร์ย่อยจะไม่ทำงานเหมือน FAT/LittleFS
- ถ้าเพิ่มจุดเมาท์อื่น ให้ประกาศ `-DFS_MOUNTS='{"/fs","/usb","/sdcard"}'` เพื่อให้ `fs.list("/")` เห็น
- ตรวจว่าโปรเจกต์ไม่มี global ชื่อ `fs` อยู่แล้ว (ถ้ามี ให้เปลี่ยนชื่อใน `fs_register`)

## emulator
- `/fs` → `<root>/data`, `/usb` → `<root>/usb` (หรือ `--usb DIR`) สร้างโฟลเดอร์ `usb` ไว้เพื่อเลียนการเสียบไดรฟ์ ลบทิ้งเพื่อเลียนการถอด
- เข้าถึงได้เฉพาะ `/fs` กับ `/usb` เท่านั้น ไฟล์อื่นของเครื่อง host เข้าไม่ได้
- ทดสอบ: `./build/phone_emu --run tests/fs_test.lua`
