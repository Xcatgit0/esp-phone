/*
 * emu_lua.cpp - Lua VM runner (port of app_main/initAPI from src/main.c), module registry,
 *               event hooks, and the emulator-only `emu` Lua table used for scripted tests.
 */
#include "emu_internal.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "lualib.h"
#include "wrap.h"          // API_INIT, read_file
#include "log.h"
}

namespace emu {
using namespace internal;

/* ---------------------------------------------------------- registry + events */
namespace {
struct Module { std::string name; ModuleOpen open; std::string doc; };
std::vector<Module> &modules() { static std::vector<Module> m; return m; }
std::vector<std::function<void(lua_State *)>> &startCbs() { static std::vector<std::function<void(lua_State *)>> v; return v; }
std::vector<std::function<void()>> &stopCbs() { static std::vector<std::function<void()>> v; return v; }
std::vector<std::function<void()>> &pushCbs() { static std::vector<std::function<void()>> v; return v; }
std::vector<std::function<void(double)>> &frameCbs() { static std::vector<std::function<void(double)>> v; return v; }
std::atomic<bool> g_vmRunning{false};
std::atomic<bool> g_vmEverRan{false};
std::thread g_vmThread;
}  // namespace

void registerModule(const char *n, ModuleOpen o, const char *d) { modules().push_back({n, o, d ? d : ""}); }
void onLuaStart(std::function<void(lua_State *)> cb) { startCbs().push_back(std::move(cb)); }
void onLuaStop(std::function<void()> cb) { stopCbs().push_back(std::move(cb)); }
void onPush(std::function<void()> cb) { pushCbs().push_back(std::move(cb)); }
void onFrame(std::function<void(double)> cb) { frameCbs().push_back(std::move(cb)); }

namespace internal {
void fireLuaStart(lua_State *L) { for (auto &f : startCbs()) f(L); }
void fireLuaStop() { for (auto &f : stopCbs()) f(); }
void firePush() { for (auto &f : pushCbs()) f(); }
void fireFrame(double dt) { for (auto &f : frameCbs()) f(dt); }
bool vmRunning() { return g_vmRunning; }
}  // namespace internal

/* ------------------------------------------------------------ scheduler */
namespace {
struct Scheduler {
    std::mutex m; std::condition_variable cv;
    std::multimap<std::chrono::steady_clock::time_point, std::function<void()>> q;
    std::thread th; bool stop = false;
    void ensure() {
        if (th.joinable()) return;
        th = std::thread([this] {
            std::unique_lock<std::mutex> l(m);
            while (!stop) {
                if (q.empty()) { cv.wait(l); continue; }
                auto when = q.begin()->first;
                if (std::chrono::steady_clock::now() < when) { cv.wait_until(l, when); continue; }
                auto fn = std::move(q.begin()->second); q.erase(q.begin());
                l.unlock(); fn(); l.lock();
            }
        });
    }
    void add(unsigned ms, std::function<void()> fn) {
        { std::lock_guard<std::mutex> l(m);
          q.emplace(std::chrono::steady_clock::now() + std::chrono::milliseconds(ms), std::move(fn)); ensure(); }
        cv.notify_all();
    }
    void clear() { std::lock_guard<std::mutex> l(m); q.clear(); }
    void shutdown() { { std::lock_guard<std::mutex> l(m); stop = true; q.clear(); } cv.notify_all(); if (th.joinable()) th.join(); }
};
Scheduler &sched() { static Scheduler s; return s; }
}  // namespace
void schedule(unsigned delayMs, std::function<void()> fn) { sched().add(delayMs, std::move(fn)); }

/* ------------------------------------------------------------- `emu` table */
namespace {
Stick stickArg(lua_State *L, int i) { return luaL_checkinteger(L, i) == 2 ? Stick::Secondary : Stick::Primary; }

int l_log(lua_State *L) {
    int n = lua_gettop(L); std::string s;
    for (int i = 1; i <= n; i++) { size_t len; const char *p = luaL_tolstring(L, i, &len); s.append(p, len); s += i < n ? "\t" : ""; lua_pop(L, 1); }
    emu::log("%s", s.c_str()); return 0;
}
int l_key(lua_State *L) { hw::setKey((int)luaL_checkinteger(L, 1) - 1, lua_toboolean(L, 2)); return 0; }
int l_stick(lua_State *L) { hw::setStick(stickArg(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)); return 0; }
int l_stick_button(lua_State *L) { hw::setStickButton(stickArg(L, 1), lua_toboolean(L, 2)); return 0; }
int l_touch(lua_State *L) {
    bool down = lua_toboolean(L, 1);
    hw::setTouch(down, (float)luaL_optnumber(L, 2, 0), (float)luaL_optnumber(L, 3, 0)); return 0;
}
int l_gpio(lua_State *L) {
    int n = (int)luaL_checkinteger(L, 1);
    if (!lua_isnoneornil(L, 2)) hw::setGpio(n, (int)luaL_checkinteger(L, 2));
    lua_pushinteger(L, hw::gpio(n)); return 1;
}
int l_adc(lua_State *L) { auto a = hw::adc(); lua_pushinteger(L, a.j1x); lua_pushinteger(L, a.j1y); lua_pushinteger(L, a.j2x); lua_pushinteger(L, a.j2y); return 4; }
int l_touch_raw(lua_State *L) { auto r = hw::touchRaw(); lua_pushinteger(L, r.x); lua_pushinteger(L, r.y); lua_pushinteger(L, r.z1); lua_pushinteger(L, r.z2); return 4; }
int l_pixel(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *src = luaL_optstring(L, 3, "panel");
    lua_pushinteger(L, !strcmp(src, "fb") ? display::framebufferPixel(x, y) : display::panelPixel(x, y)); return 1;
}
int l_screenshot(lua_State *L) { lua_pushboolean(L, display::saveBmp(luaL_checkstring(L, 1))); return 1; }
int l_pushes(lua_State *L) { lua_pushinteger(L, display::pushCount()); return 1; }
int l_cursor(lua_State *L) { auto c = display::cursor(); lua_pushinteger(L, c.x); lua_pushinteger(L, c.y); lua_pushboolean(L, c.visible); return 3; }
int l_sleep(lua_State *L) {   // real-time sleep in ms (NOT rounded to FreeRTOS ticks)
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((long)luaL_checkinteger(L, 1));
    while (std::chrono::steady_clock::now() < end && !g_quit && !g_reset) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return 0;
}
int l_wait_push(lua_State *L) {  // emu.wait_push([n=1], [timeout_ms=2000]) -> true if n new pushes were seen
    uint32_t want = display::pushCount() + (uint32_t)luaL_optinteger(L, 1, 1);
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((long)luaL_optinteger(L, 2, 2000));
    while (display::pushCount() < want && std::chrono::steady_clock::now() < end && !g_quit && !g_reset)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    lua_pushboolean(L, display::pushCount() >= want); return 1;
}
int l_reset(lua_State *) { requestReset(); return 0; }
int l_quit(lua_State *) { requestQuit(); return 0; }
int l_assert(lua_State *L) {
    if (!lua_toboolean(L, 1)) { requestQuit(); return luaL_error(L, "emu.assert failed: %s", luaL_optstring(L, 2, "assertion failed")); }
    return 0;
}
int l_wifi_add_network(lua_State *L) {
    wifiAddNetwork(luaL_checkstring(L, 1), luaL_optstring(L, 2, ""), (int)luaL_optinteger(L, 3, -60),
                   (int)luaL_optinteger(L, 4, 1), luaL_optstring(L, 5, "")); return 0;
}
int l_cpu_ms(lua_State *L) {   // process CPU time: lets tests prove a wait does not busy-loop
    timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    lua_pushnumber(L, ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6); return 1;
}
int l_schedule(lua_State *L) {   // emu.schedule(ms, action, ...): act on the hardware ms from now, without blocking Lua
    unsigned ms = (unsigned)luaL_checkinteger(L, 1);
    std::string act = luaL_checkstring(L, 2);
    if (act == "key") {
        int i = (int)luaL_checkinteger(L, 3) - 1; bool d = lua_toboolean(L, 4);
        schedule(ms, [=] { hw::setKey(i, d); });
    } else if (act == "stick_button") {
        Stick st = stickArg(L, 3); bool d = lua_toboolean(L, 4);
        schedule(ms, [=] { hw::setStickButton(st, d); });
    } else if (act == "stick") {
        Stick st = stickArg(L, 3); float x = (float)luaL_checknumber(L, 4), y = (float)luaL_checknumber(L, 5);
        schedule(ms, [=] { hw::setStick(st, x, y); });
    } else if (act == "touch") {
        bool d = lua_toboolean(L, 3); float x = (float)luaL_optnumber(L, 4, 0), y = (float)luaL_optnumber(L, 5, 0);
        schedule(ms, [=] { hw::setTouch(d, x, y); });
    } else if (act == "gpio") {
        int n = (int)luaL_checkinteger(L, 3), lv = (int)luaL_checkinteger(L, 4);
        schedule(ms, [=] { hw::setGpio(n, lv); });
    } else return luaL_argerror(L, 2, "unknown action (key, stick_button, stick, touch, gpio)");
    return 0;
}
int open_emu(lua_State *L) {
    static const luaL_Reg fns[] = {
        {"log", l_log}, {"key", l_key}, {"stick", l_stick}, {"stick_button", l_stick_button}, {"touch", l_touch},
        {"gpio", l_gpio}, {"adc", l_adc}, {"touch_raw", l_touch_raw}, {"pixel", l_pixel}, {"screenshot", l_screenshot},
        {"pushes", l_pushes}, {"cursor", l_cursor}, {"sleep", l_sleep}, {"wait_push", l_wait_push}, {"reset", l_reset},
        {"quit", l_quit}, {"assert", l_assert}, {"schedule", l_schedule}, {"cpu_ms", l_cpu_ms}, {"wifi_add_network", l_wifi_add_network}, {nullptr, nullptr}};
    luaL_newlib(L, fns);
    return 1;
}
}  // namespace

namespace internal {
void openModules(lua_State *L) {
    lua_pushcfunction(L, open_emu); lua_call(L, 0, 1); lua_setglobal(L, "emu");
    for (auto &m : modules()) {
        lua_pushcfunction(L, m.open);
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) { emu::log("module '%s' failed to open: %s", m.name.c_str(), lua_tostring(L, -1)); lua_pop(L, 1); continue; }
        lua_setglobal(L, m.name.c_str());
    }
}

static void dumpTable(lua_State *L, int idx, const std::string &prefix, int depth, std::vector<std::string> &out) {
    idx = lua_absindex(L, idx);
    std::vector<std::pair<std::string, int>> keys;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) == LUA_TSTRING) keys.push_back({lua_tostring(L, -2), lua_type(L, -1)});
        lua_pop(L, 1);
    }
    std::sort(keys.begin(), keys.end());
    for (auto &k : keys) {
        std::string full = prefix.empty() ? k.first : prefix + "." + k.first;
        if (k.second == LUA_TTABLE && depth > 0 && k.first != "_G" && k.first != "package") {
            lua_getfield(L, idx, k.first.c_str());
            dumpTable(L, -1, full, depth - 1, out);
            lua_pop(L, 1);
        } else out.push_back(full + "  (" + lua_typename(L, k.second) + ")");
    }
}
void dumpApi(lua_State *L) {
    std::vector<std::string> out;
    lua_pushglobaltable(L); dumpTable(L, -1, "", 3, out); lua_pop(L, 1);
    for (auto &s : out) puts(s.c_str());
}
}  // namespace internal

/* ------------------------------------------------------------------ the VM */
static void interruptHook(lua_State *L, lua_Debug *) {
    if (g_reset || g_quit) luaL_error(L, "emu: VM interrupted (reset)");
}

static void initAPI(lua_State *L) {            // identical library set to src/main.c
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
    API_INIT(L);
    openModules(L);
}

static void vmMain() {
    t_isLua = true;
    while (!g_quit) {
        g_reset = false;
        sched().clear();
        wifiReset();
        displayStart();
        heapCharge(0);

        lua_State *L = lua_newstate(luaAlloc, nullptr, (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
        initAPI(L);
        fireLuaStart(L);

        if (g_opt.dumpApi) { dumpApi(L); lua_close(L); requestQuit(); break; }

        std::string boot = !g_opt.runScript.empty() ? g_opt.runScript : g_opt.bootstrap;
        char *src = nullptr;
        if (boot.empty()) src = read_file("/fs/bootstrap.lua");
        else {
            FILE *f = fopen(boot.c_str(), "rb");   // host path (emu_fopen leaves non-/fs paths alone)
            if (f) {
                fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
                src = (char *)malloc(n + 1); if (src) { size_t r = fread(src, 1, n, f); src[r] = 0; } fclose(f);
            }
        }
        if (!src) {
            LOG_ERROR("CANNOT LOAD \"bootstrap.lua\"");
            emu::log("device would abort() here; waiting for reset (F5)");
        } else {
            /* Same as the device: the loadbuffer status is ignored. On a syntax error the error
             * MESSAGE stays on the stack and lua_pcall then fails with "attempt to call a string value". */
            int ls = luaL_loadbuffer(L, src, strlen(src), "bootstrap.lua");
            if (ls != LUA_OK) emu::log("NOTE: bootstrap failed to compile (device hides this): %s", lua_tostring(L, -1));
            free(src);
            lua_sethook(L, interruptHook, LUA_MASKCOUNT, 1000);
            g_vmRunning = true; g_vmEverRan = true;
            int st = lua_pcall(L, 0, 0, 0);
            if (st != LUA_OK) { printf("Lua error: %s\n", lua_tostring(L, -1)); lua_pop(L, 1); }
            g_vmRunning = false;
            LOG_WARN("LUA VM STOPPED");
            LOG_WARN("RESET TO RESTART");
        }
        lua_close(L);
        fireLuaStop();

        /* like the device: display/cursor keep running; wait for RESET */
        while (!g_quit && !g_reset) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        displayStop();
    }
    displayStop();
}

namespace internal {
void vmThreadStart() { g_vmThread = std::thread(vmMain); }
void vmThreadJoin() { if (g_vmThread.joinable()) g_vmThread.join(); sched().shutdown(); }

int runHeadless() {
    vmThreadStart();
    auto t0 = std::chrono::steady_clock::now();
    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (g_opt.runSeconds > 0 && el >= g_opt.runSeconds) break;
        if (g_vmEverRan && !g_vmRunning && g_opt.runSeconds == 0) break;   // script finished
    }
    if (!g_opt.shotPath.empty()) { display::saveBmp(g_opt.shotPath); emu::log("screenshot -> %s", g_opt.shotPath.c_str()); }
    requestQuit();
    vmThreadJoin();
    return 0;
}
}  // namespace internal
}  // namespace emu
