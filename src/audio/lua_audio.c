/* lua_audio.c - Lua API `audio`. See include/lua_audio.h. Same code on the device and in the emulator. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "lua.h"
#include "lauxlib.h"
#include "audio_decoder.h"
#include "audio_player.h"
#include "lua_audio.h"

static const char *const state_names[] = {"stopped", "buffering", "playing", "paused", "error"};
static const char *const eq_type_names[] = {"peak", "lowshelf", "highshelf", "lowpass", "highpass", "notch"};

static int fail(lua_State *L, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

/* ---------------------------------------------------------------------------- play & transport */

static int l_play(lua_State *L)
{
    const char *uri = luaL_checkstring(L, 1);
    audio_play_opts_t o;
    memset(&o, 0, sizeof(o));
    char err[128];

    if (!lua_isnoneornil(L, 2))
    {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "loop");
        o.loop = lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 2, "timeout");
        if (lua_type(L, -1) == LUA_TNUMBER)
            o.timeout_ms = (int)lua_tonumber(L, -1);
        else if (!lua_isnil(L, -1))
            return fail(L, "timeout must be a number (ms)");
        lua_pop(L, 1);

        lua_getfield(L, 2, "prebuffer");
        if (lua_type(L, -1) == LUA_TNUMBER)
            o.prebuffer_bytes = (size_t)lua_tonumber(L, -1);
        else if (!lua_isnil(L, -1))
            return fail(L, "prebuffer must be a number (bytes)");
        lua_pop(L, 1);

        lua_getfield(L, 2, "ca_cert"); /* stays on the stack: keeps the string alive during the call */
        if (lua_type(L, -1) == LUA_TSTRING)
            o.ca_cert = lua_tostring(L, -1);
        else if (!lua_isnil(L, -1))
            return fail(L, "ca_cert must be a PEM string");

        lua_getfield(L, 2, "headers");
        if (lua_type(L, -1) == LUA_TTABLE)
        {
            int ht = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, ht))
            {
                if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING)
                    return fail(L, "headers must be { Name = \"value\" }");
                const char *k = lua_tostring(L, -2), *v = lua_tostring(L, -1);
                if (strpbrk(k, ":\r\n ") || strpbrk(v, "\r\n"))
                    return fail(L, "invalid header (no spaces/colons in names, no CR/LF in values)");
                if (o.n_hdr >= 8)
                    return fail(L, "too many headers (max 8)");
                o.hdr_name[o.n_hdr] = k;
                o.hdr_value[o.n_hdr] = v;
                o.n_hdr++;
                lua_pop(L, 1);
            }
        }
        else if (!lua_isnil(L, -1))
            return fail(L, "headers must be a table");
    }

    if (audio_player_play(uri, &o, err, sizeof(err)) != 0)
        return fail(L, err);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_stop(lua_State *L) { (void)L; audio_player_stop(); return 0; }
static int l_pause(lua_State *L) { (void)L; audio_player_pause(); return 0; }
static int l_resume(lua_State *L) { (void)L; audio_player_resume(); return 0; }

static int l_volume(lua_State *L)
{
    if (!lua_isnoneornil(L, 1))
    {
        lua_Number v = luaL_checknumber(L, 1);
        if (!(v >= 0.0 && v <= 1.0))
            return fail(L, "volume must be between 0 and 1");
        audio_player_set_volume((float)v);
    }
    lua_pushnumber(L, (lua_Number)audio_player_get_volume());
    return 1;
}

static int l_mono(lua_State *L)
{
    if (!lua_isnoneornil(L, 1))
        audio_player_set_mono(lua_toboolean(L, 1));
    lua_pushboolean(L, audio_player_get_mono());
    return 1;
}

static int l_rate(lua_State *L)
{
    if (!lua_isnoneornil(L, 1))
    {
        lua_Integer hz = luaL_checkinteger(L, 1);
        if (audio_player_set_rate((int)hz) != 0)
            return fail(L, "rate must be 8000..48000 Hz and can only change while stopped");
    }
    audio_status_t st;
    audio_player_status(&st);
    lua_pushinteger(L, st.out_rate);
    return 1;
}

/* ------------------------------------------------------------------------------------------ EQ */

static int eq_type_from(const char *s)
{
    for (int i = 0; i < 6; i++)
        if (strcasecmp(s, eq_type_names[i]) == 0)
            return i;
    return -1;
}

/* fills *b from the values on the stack; returns NULL or an error message */
static const char *eq_check(audio_eq_band_t *b, lua_Number freq, lua_Number gain, lua_Number q, int type, int rate, char *buf, size_t n)
{
    if (type < 0)
        return "type must be peak, lowshelf, highshelf, lowpass, highpass or notch";
    float fmax = 0.45f * (float)rate;
    if (!(freq >= 20.0 && freq <= fmax))
    {
        snprintf(buf, n, "freq must be 20..%d Hz (45%% of the output rate %d)", (int)fmax, rate);
        return buf;
    }
    if (!(gain >= -24.0 && gain <= 24.0))
        return "gain must be -24..24 dB";
    if (!(q >= 0.1 && q <= 20.0))
        return "q must be 0.1..20";
    b->enabled = true;
    b->type = (audio_eq_type_t)type;
    b->freq_hz = (float)freq;
    b->gain_db = (float)gain;
    b->q = (float)q;
    return NULL;
}

static void push_band(lua_State *L, const audio_eq_band_t *b)
{
    lua_createtable(L, 0, 4);
    lua_pushstring(L, eq_type_names[b->type]);
    lua_setfield(L, -2, "type");
    lua_pushnumber(L, b->freq_hz);
    lua_setfield(L, -2, "freq");
    lua_pushnumber(L, b->gain_db);
    lua_setfield(L, -2, "gain");
    lua_pushnumber(L, b->q);
    lua_setfield(L, -2, "q");
}

static int l_eq(lua_State *L)
{
    audio_status_t st;
    audio_player_status(&st);
    if (lua_isnoneornil(L, 1)) /* read */
    {
        audio_eq_band_t cur[AUDIO_EQ_BANDS];
        audio_player_get_eq(cur);
        lua_newtable(L);
        int k = 0;
        for (int i = 0; i < AUDIO_EQ_BANDS; i++)
            if (cur[i].enabled)
            {
                push_band(L, &cur[i]);
                lua_rawseti(L, -2, ++k);
            }
        return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Unsigned n = lua_rawlen(L, 1);
    if (n > AUDIO_EQ_BANDS)
        return fail(L, "at most 5 EQ bands");
    audio_eq_band_t bands[AUDIO_EQ_BANDS];
    char buf[96], msg[128];
    for (lua_Unsigned i = 1; i <= n; i++)
    {
        lua_rawgeti(L, 1, (lua_Integer)i);
        if (!lua_istable(L, -1))
            return fail(L, "each EQ band must be a table { freq=, gain=, q=, type= }");
        lua_getfield(L, -1, "freq");
        lua_getfield(L, -2, "gain");
        lua_getfield(L, -3, "q");
        lua_getfield(L, -4, "type");
        if (lua_type(L, -4) != LUA_TNUMBER)
            return fail(L, "EQ band needs a numeric freq");
        const char *ts = lua_isnil(L, -1) ? "peak" : lua_tostring(L, -1);
        if (!ts)
            return fail(L, "type must be a string");
        int type = eq_type_from(ts);
        lua_Number freq = lua_tonumber(L, -4);
        lua_Number gain = lua_isnil(L, -3) ? 0.0 : lua_tonumber(L, -3);
        lua_Number q = lua_isnil(L, -2) ? ((type == AUDIO_EQ_PEAK || type == AUDIO_EQ_NOTCH) ? 1.0 : 0.707) : lua_tonumber(L, -2);
        const char *e = eq_check(&bands[i - 1], freq, gain, q, type, st.out_rate, buf, sizeof(buf));
        if (e)
        {
            snprintf(msg, sizeof(msg), "band %d: %s", (int)i, e);
            return fail(L, msg);
        }
        lua_pop(L, 5);
    }
    if (audio_player_set_eq(bands, (int)n) != 0)
        return fail(L, "invalid EQ settings");
    lua_pushboolean(L, 1);
    return 1;
}

static int l_eq_band(lua_State *L)
{
    lua_Integer idx = luaL_checkinteger(L, 1);
    if (idx < 1 || idx > AUDIO_EQ_BANDS)
        return fail(L, "band index must be 1..5");
    audio_eq_band_t b;
    memset(&b, 0, sizeof(b));
    if (lua_isnil(L, 2) || lua_isnone(L, 2)) /* disable */
    {
        b.enabled = false;
        b.type = AUDIO_EQ_PEAK;
        b.freq_hz = 1000.0f;
        b.q = 1.0f;
        audio_player_set_eq_band((int)idx - 1, &b);
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_Number freq = luaL_checknumber(L, 2);
    lua_Number gain = luaL_optnumber(L, 3, 0.0);
    const char *ts = luaL_optstring(L, 5, "peak");
    int type = eq_type_from(ts);
    lua_Number q = luaL_optnumber(L, 4, (type == AUDIO_EQ_PEAK || type == AUDIO_EQ_NOTCH) ? 1.0 : 0.707);
    audio_status_t st;
    audio_player_status(&st);
    char buf[96];
    const char *e = eq_check(&b, freq, gain, q, type, st.out_rate, buf, sizeof(buf));
    if (e)
        return fail(L, e);
    audio_player_set_eq_band((int)idx - 1, &b);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_eq_reset(lua_State *L) { (void)L; audio_player_eq_reset(); return 0; }

/* ------------------------------------------------------------------------------ status / wait */

static int l_status(lua_State *L)
{
    audio_status_t st;
    audio_player_status(&st);
    lua_createtable(L, 0, 12);
    lua_pushstring(L, state_names[st.state]);
    lua_setfield(L, -2, "state");
    lua_pushstring(L, st.uri);
    lua_setfield(L, -2, "uri");
    if (st.codec[0])
    {
        lua_pushstring(L, st.codec);
        lua_setfield(L, -2, "codec");
    }
    if (st.error[0])
    {
        lua_pushstring(L, st.error);
        lua_setfield(L, -2, "error");
    }
    lua_pushinteger(L, (lua_Integer)st.position_ms);
    lua_setfield(L, -2, "position");
    if (st.duration_ms >= 0)
    {
        lua_pushinteger(L, (lua_Integer)st.duration_ms);
        lua_setfield(L, -2, "duration");
    }
    if (st.src_rate > 0)
    {
        lua_pushinteger(L, st.src_rate);
        lua_setfield(L, -2, "sample_rate");
        lua_pushinteger(L, st.src_channels);
        lua_setfield(L, -2, "channels");
    }
    lua_pushinteger(L, st.out_rate);
    lua_setfield(L, -2, "out_rate");
    lua_pushnumber(L, st.volume);
    lua_setfield(L, -2, "volume");
    lua_pushboolean(L, st.mono);
    lua_setfield(L, -2, "mono");
    lua_pushinteger(L, (lua_Integer)st.buffered_bytes);
    lua_setfield(L, -2, "buffered");
    return 1;
}

static int l_get_vu(lua_State *L)
{
    float l = 0, r = 0;
    audio_player_get_vu(&l, &r);
    lua_pushnumber(L, l);
    lua_pushnumber(L, r);
    return 2;
}

static int l_wait(lua_State *L)
{
    lua_Number t = luaL_optnumber(L, 1, -1);
    char err[96];
    audio_end_t e = audio_player_wait(t < 0 ? -1 : (int64_t)t, err, sizeof(err));
    switch (e)
    {
    case AUDIO_END_FINISHED: lua_pushstring(L, "finished"); return 1;
    case AUDIO_END_STOPPED: lua_pushstring(L, "stopped"); return 1;
    case AUDIO_END_ERROR: lua_pushstring(L, "error"); lua_pushstring(L, err); return 2;
    default: lua_pushnil(L); return 1;
    }
}

static int l_codecs(lua_State *L)
{
    lua_newtable(L);
    int k = 0;
    for (int c = AUDIO_CODEC_WAV; c <= AUDIO_CODEC_M4A; c++)
        if (audio_decoder_supported((audio_codec_t)c))
        {
            lua_pushstring(L, audio_codec_name((audio_codec_t)c));
            lua_rawseti(L, -2, ++k);
        }
    return 1;
}

void audio_register(lua_State *L)
{
    audio_player_vm_reset(); /* a new VM starts from the defaults (the device would have rebooted) */
    static const luaL_Reg fns[] = {{"play", l_play},     {"stop", l_stop},     {"pause", l_pause},     {"resume", l_resume},
                                   {"volume", l_volume}, {"mono", l_mono},     {"rate", l_rate},       {"eq", l_eq},
                                   {"eq_band", l_eq_band}, {"eq_reset", l_eq_reset}, {"status", l_status}, {"getVU", l_get_vu}, {"wait", l_wait},
                                   {"codecs", l_codecs}, {NULL, NULL}};
    luaL_newlib(L, fns);
    lua_setglobal(L, "audio");
}
