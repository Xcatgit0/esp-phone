#include "emu/emu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef EMU_DEFAULT_ROOT
#define EMU_DEFAULT_ROOT ".."
#endif

static void usage() {
    puts(
"phone_emu - SDL2 emulator of the ESP32-S3 phone (Lua API level)\n"
"\n"
"  --root DIR          phone project root (default: " EMU_DEFAULT_ROOT ")\n"
"  --fs DIR            directory mapped to /fs (default: <root>/data)\n"
"  --boot FILE         boot script (host path) instead of /fs/bootstrap.lua\n"
"  --run FILE          same as --boot (handy for test scripts using the `emu` table)\n"
"  --scale N           LCD scale factor (default 2)\n"
"  --headless          no window; runs until the script ends or --seconds elapse\n"
"  --seconds N         headless: stop after N seconds\n"
"  --shot FILE.bmp     headless: save the LCD on exit\n"
"  --heap-kb N         total 8-bit heap reported to Lua (default 8492)\n"
"  --psram-kb N        PSRAM part of it (default 8192)\n"
"  --heap-limit-kb N   hard limit: Lua/C allocations beyond it fail (simulate OOM)\n"
"  --dump-api          print every global the Lua VM exposes, then exit\n"
"  --set KEY=VALUE     hardware tuning: touch-noise, adc-noise, adc-center,\n"
"                      j1-invert-x, j1-invert-y, j2-invert-x, j2-invert-y\n"
"\n"
"keys: arrows=joy1 WASD=joy2 space/enter=stick buttons 1 2 3=KEY1..3 F5=reset F12=shot Esc=quit");
}

int main(int argc, char **argv) {
    emu::Options o;
    o.root = EMU_DEFAULT_ROOT;
    auto need = [&](int &i) -> const char * {
        if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); exit(2); }
        return argv[++i];
    };
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--root") o.root = need(i);
        else if (a == "--fs") o.fsDir = need(i);
        else if (a == "--boot") o.bootstrap = need(i);
        else if (a == "--run") o.runScript = need(i);
        else if (a == "--scale") o.scale = atoi(need(i));
        else if (a == "--headless") o.headless = true;
        else if (a == "--seconds") o.runSeconds = atof(need(i));
        else if (a == "--shot") o.shotPath = need(i);
        else if (a == "--heap-kb") o.heapKb = (size_t)atol(need(i));
        else if (a == "--psram-kb") o.psramKb = (size_t)atol(need(i));
        else if (a == "--heap-limit-kb") o.heapLimitKb = (size_t)atol(need(i));
        else if (a == "--dump-api") o.dumpApi = true;
        else if (a == "--set") {
            std::string kv = need(i); auto p = kv.find('=');
            if (p == std::string::npos) { fprintf(stderr, "--set expects KEY=VALUE\n"); return 2; }
            std::string k = kv.substr(0, p); double v = atof(kv.c_str() + p + 1);
            auto &c = emu::hw::config();
            if (k == "touch-noise") c.touchNoise = (float)v;
            else if (k == "adc-noise") c.adcNoise = (float)v;
            else if (k == "adc-center") c.adcCenter = (int)v;
            else if (k == "j1-invert-x") c.j1InvertX = v != 0;
            else if (k == "j1-invert-y") c.j1InvertY = v != 0;
            else if (k == "j2-invert-x") c.j2InvertX = v != 0;
            else if (k == "j2-invert-y") c.j2InvertY = v != 0;
            else { fprintf(stderr, "unknown --set key '%s'\n", k.c_str()); return 2; }
        } else { fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 2; }
    }
    if (o.fsDir.empty()) o.fsDir = o.root + "/data";
    return emu::run(o);
}
