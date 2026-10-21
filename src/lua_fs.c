/* lua_fs.c - Lua API `fs`. See include/lua_fs.h. Same code on the device and in the emulator.
 * In the emulator emu_force.h defines EMU_FS_MAP and emu_fs_path() maps /fs and /usb onto host folders
 * (nothing outside them is reachable). */
#include <ctype.h>
#include <stdbool.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lua_fs.h"

#ifndef FS_MOUNTS
#define FS_MOUNTS {"/fs", "/usb"} /* shown by fs.list("/") and fs.mounts() when they exist; add "/sdcard" etc. */
#endif
#define FS_PATH_MAX 255
#define FS_READ_DEFAULT_MAX (256 * 1024)
#define FS_LIST_DEFAULT_MAX 1000
#define FS_RM_MAX_DEPTH 8

static const char *const k_mounts[] = FS_MOUNTS;
#define N_MOUNTS (sizeof(k_mounts) / sizeof(k_mounts[0]))

#ifdef EMU_FS_MAP
const char *emu_fs_path(const char *in, char *out, size_t n); /* emu_core.cpp; NULL = not reachable */
#endif

/* ------------------------------------------------------------------------------------------ paths */

/* Validate a Lua path and map it to the real one. Returns NULL (errno set) if rejected. */
static const char *real_path(const char *p, char *buf, size_t n)
{
    size_t len = strlen(p);
    if (len == 0 || p[0] != '/')
    {
        errno = EINVAL;
        return NULL;
    }
    if (len > FS_PATH_MAX)
    {
        errno = ENAMETOOLONG;
        return NULL;
    }
    for (const char *s = p; *s;) /* reject ".." segments */
    {
        if (s[0] == '.' && s[1] == '.' && (s[2] == '/' || s[2] == 0) && (s == p || s[-1] == '/'))
        {
            errno = EINVAL;
            return NULL;
        }
        const char *slash = strchr(s, '/');
        if (!slash)
            break;
        s = slash + 1;
    }
#ifdef EMU_FS_MAP
    const char *m = emu_fs_path(p, buf, n);
    if (!m)
        errno = ENOENT;
    return m;
#else
    (void)buf;
    (void)n;
    return p;
#endif
}

static int push_err(lua_State *L, int e)
{
    lua_pushnil(L);
    lua_pushstring(L, strerror(e));
    return 2;
}

#define REAL(var, L, idx)                                  \
    char var##_buf[FS_PATH_MAX + 1 + 600];                 \
    const char *var = real_path(luaL_checkstring(L, idx), var##_buf, sizeof(var##_buf)); \
    if (!var)                                              \
        return push_err(L, errno)

static const char *type_name(mode_t m) { return S_ISDIR(m) ? "dir" : "file"; }

static void push_stat_fields(lua_State *L, const struct stat *st)
{
    lua_pushstring(L, type_name(st->st_mode));
    lua_setfield(L, -2, "type");
    lua_pushinteger(L, S_ISDIR(st->st_mode) ? 0 : (lua_Integer)st->st_size);
    lua_setfield(L, -2, "size");
    lua_pushinteger(L, (lua_Integer)st->st_mtime);
    lua_setfield(L, -2, "mtime");
}

/* ------------------------------------------------------------------------------------------ list */

typedef struct
{
    char *name;
    bool dir;
    long size;
    long mtime;
} entry_t;

static int entry_cmp(const void *a, const void *b)
{
    const entry_t *x = (const entry_t *)a, *y = (const entry_t *)b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1; /* folders first */
    return strcasecmp(x->name, y->name);
}

static bool opt_bool(lua_State *L, int t, const char *key, bool def)
{
    if (t == 0 || lua_isnoneornil(L, t))
        return def;
    lua_getfield(L, t, key);
    bool v = lua_isnil(L, -1) ? def : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

static int l_list(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    int ot = lua_istable(L, 2) ? 2 : 0;
    bool want_size = opt_bool(L, ot, "size", true);
    bool sort = opt_bool(L, ot, "sort", true);
    bool hidden = opt_bool(L, ot, "hidden", true);
    long max = FS_LIST_DEFAULT_MAX;
    if (ot)
    {
        lua_getfield(L, ot, "max");
        if (lua_isinteger(L, -1) && lua_tointeger(L, -1) > 0)
            max = (long)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    if (strcmp(path, "/") == 0) /* virtual root: the mount points that exist */
    {
        lua_newtable(L);
        int k = 0;
        for (size_t i = 0; i < N_MOUNTS; i++)
        {
            char buf[FS_PATH_MAX + 1 + 600];
            const char *rp = real_path(k_mounts[i], buf, sizeof(buf));
            struct stat st;
            if (!rp || stat(rp, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            lua_newtable(L);
            lua_pushstring(L, k_mounts[i] + 1);
            lua_setfield(L, -2, "name");
            lua_pushstring(L, "dir");
            lua_setfield(L, -2, "type");
            lua_pushinteger(L, 0);
            lua_setfield(L, -2, "size");
            lua_pushinteger(L, (lua_Integer)st.st_mtime);
            lua_setfield(L, -2, "mtime");
            lua_rawseti(L, -2, ++k);
        }
        return 1;
    }

    REAL(dir, L, 1);
    DIR *d = opendir(dir);
    if (!d)
        return push_err(L, errno);

    size_t cap = 32, n = 0;
    entry_t *v = (entry_t *)malloc(cap * sizeof(entry_t));
    if (!v)
    {
        closedir(d);
        return luaL_error(L, "out of memory");
    }
    bool truncated = false;
    const size_t base = strlen(dir);
    struct dirent *de;
    while ((de = readdir(d)) != NULL)
    {
        const char *nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
            continue;
        if (!hidden && nm[0] == '.')
            continue;
        if ((long)n >= max)
        {
            truncated = true;
            break;
        }
        if (n == cap)
        {
            entry_t *nv = (entry_t *)realloc(v, cap * 2 * sizeof(entry_t));
            if (!nv)
            {
                truncated = true;
                break;
            }
            v = nv;
            cap *= 2;
        }
        entry_t e = {.name = strdup(nm), .dir = false, .size = 0, .mtime = 0};
        if (!e.name)
        {
            truncated = true;
            break;
        }
        bool known_dir = false;
#ifdef DT_DIR
        if (de->d_type == DT_DIR)
            known_dir = true;
#endif
        if (want_size || !known_dir)
        {
            char full[FS_PATH_MAX + 1 + 600];
            if (base + 1 + strlen(nm) < sizeof(full))
            {
                snprintf(full, sizeof(full), "%s%s%s", dir, (base && dir[base - 1] == '/') ? "" : "/", nm);
                struct stat st;
                if (stat(full, &st) == 0)
                {
                    e.dir = S_ISDIR(st.st_mode);
                    e.size = e.dir ? 0 : (long)st.st_size;
                    e.mtime = (long)st.st_mtime;
                }
            }
        }
        else
            e.dir = true;
        v[n++] = e;
    }
    closedir(d);
    if (sort && n > 1)
        qsort(v, n, sizeof(entry_t), entry_cmp);

    lua_newtable(L);
    for (size_t i = 0; i < n; i++)
    {
        lua_newtable(L);
        lua_pushstring(L, v[i].name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, v[i].dir ? "dir" : "file");
        lua_setfield(L, -2, "type");
        if (want_size)
        {
            lua_pushinteger(L, v[i].size);
            lua_setfield(L, -2, "size");
            lua_pushinteger(L, v[i].mtime);
            lua_setfield(L, -2, "mtime");
        }
        lua_rawseti(L, -2, (lua_Integer)i + 1);
        free(v[i].name);
    }
    free(v);
    lua_pushboolean(L, truncated);
    return 2; /* list, truncated */
}

/* ------------------------------------------------------------------------------------------ stat & co */

static int l_stat(lua_State *L)
{
    REAL(p, L, 1);
    struct stat st;
    if (stat(p, &st) != 0)
        return push_err(L, errno);
    lua_newtable(L);
    push_stat_fields(L, &st);
    return 1;
}

static int l_exists(lua_State *L)
{
    char buf[FS_PATH_MAX + 1 + 600];
    const char *p = real_path(luaL_checkstring(L, 1), buf, sizeof(buf));
    struct stat st;
    lua_pushboolean(L, p && stat(p, &st) == 0);
    return 1;
}

static int is_kind(lua_State *L, bool dir)
{
    char buf[FS_PATH_MAX + 1 + 600];
    const char *p = real_path(luaL_checkstring(L, 1), buf, sizeof(buf));
    struct stat st;
    lua_pushboolean(L, p && stat(p, &st) == 0 && (S_ISDIR(st.st_mode) == dir));
    return 1;
}
static int l_isdir(lua_State *L) { return is_kind(L, true); }
static int l_isfile(lua_State *L) { return is_kind(L, false); }

static int l_mounts(lua_State *L)
{
    lua_newtable(L);
    int k = 0;
    for (size_t i = 0; i < N_MOUNTS; i++)
    {
        char buf[FS_PATH_MAX + 1 + 600];
        const char *rp = real_path(k_mounts[i], buf, sizeof(buf));
        struct stat st;
        if (rp && stat(rp, &st) == 0 && S_ISDIR(st.st_mode))
        {
            lua_pushstring(L, k_mounts[i]);
            lua_rawseti(L, -2, ++k);
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------ mkdir / remove / rename */

static int l_mkdir(lua_State *L)
{
    REAL(p, L, 1);
    bool parents = lua_toboolean(L, 2);
    if (!parents)
    {
        if (mkdir(p, 0777) != 0)
            return push_err(L, errno);
        lua_pushboolean(L, 1);
        return 1;
    }
    char tmp[FS_PATH_MAX + 1 + 600];
    snprintf(tmp, sizeof(tmp), "%s", p);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = 0;
    /* skip the mount point itself: it is not ours to create */
    for (char *s = tmp + 1; *s; s++)
    {
        if (*s != '/')
            continue;
        *s = 0;
        struct stat st;
        if (stat(tmp, &st) != 0 && mkdir(tmp, 0777) != 0 && errno != EEXIST)
            return push_err(L, errno);
        *s = '/';
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return push_err(L, errno);
    lua_pushboolean(L, 1);
    return 1;
}

static int rm_tree(const char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return errno;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0 ? 0 : errno;
    if (depth >= FS_RM_MAX_DEPTH)
        return ELOOP;
    DIR *d = opendir(path);
    if (!d)
        return errno;
    char *child = (char *)malloc(FS_PATH_MAX + 1 + 600);
    if (!child)
    {
        closedir(d);
        return ENOMEM;
    }
    int rc = 0;
    struct dirent *de;
    size_t base = strlen(path);
    while (rc == 0 && (de = readdir(d)) != NULL)
    {
        const char *nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
            continue;
        if (base + 1 + strlen(nm) >= FS_PATH_MAX + 600)
        {
            rc = ENAMETOOLONG;
            break;
        }
        snprintf(child, FS_PATH_MAX + 600, "%s%s%s", path, (base && path[base - 1] == '/') ? "" : "/", nm);
        rc = rm_tree(child, depth + 1);
    }
    free(child);
    closedir(d);
    if (rc == 0 && rmdir(path) != 0)
        rc = errno;
    return rc;
}

static int l_remove(lua_State *L)
{
    REAL(p, L, 1);
    bool recursive = lua_toboolean(L, 2);
    struct stat st;
    if (stat(p, &st) != 0)
        return push_err(L, errno);
    int rc;
    if (!S_ISDIR(st.st_mode))
        rc = unlink(p) == 0 ? 0 : errno;
    else if (recursive)
        rc = rm_tree(p, 0);
    else
        rc = rmdir(p) == 0 ? 0 : errno;
    if (rc)
        return push_err(L, rc);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_rename(lua_State *L)
{
    REAL(a, L, 1);
    REAL(b, L, 2);
    if (rename(a, b) != 0)
        return push_err(L, errno);
    lua_pushboolean(L, 1);
    return 1;
}

/* ------------------------------------------------------------------------------------------ file contents */

static int l_copy(lua_State *L)
{
    REAL(a, L, 1);
    REAL(b, L, 2);
    FILE *in = fopen(a, "rb");
    if (!in)
        return push_err(L, errno);
    FILE *out = fopen(b, "wb");
    if (!out)
    {
        int e = errno;
        fclose(in);
        return push_err(L, e);
    }
    char *buf = (char *)malloc(4096);
    if (!buf)
    {
        fclose(in);
        fclose(out);
        return luaL_error(L, "out of memory");
    }
    int err = 0;
    size_t n;
    while ((n = fread(buf, 1, 4096, in)) > 0)
        if (fwrite(buf, 1, n, out) != n)
        {
            err = errno ? errno : EIO;
            break;
        }
    if (!err && ferror(in))
        err = EIO;
    free(buf);
    fclose(in);
    if (fclose(out) != 0 && !err)
        err = EIO;
    if (err)
        return push_err(L, err);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_read(lua_State *L)
{
    REAL(p, L, 1);
    lua_Integer max = luaL_optinteger(L, 2, FS_READ_DEFAULT_MAX);
    if (max < 0)
        max = 0;
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
        return push_err(L, EISDIR);
    FILE *f = fopen(p, "rb");
    if (!f)
        return push_err(L, errno);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    char chunk[512];
    lua_Integer total = 0;
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
    {
        total += (lua_Integer)n;
        if (total > max)
        {
            fclose(f);
            lua_pushnil(L); /* (the partial buffer on the stack is dropped by the return) */
            lua_pushstring(L, "file too big (raise max_bytes)");
            return 2;
        }
        luaL_addlstring(&b, chunk, n);
    }
    bool bad = ferror(f);
    fclose(f);
    if (bad)
        return push_err(L, EIO);
    luaL_pushresult(&b);
    return 1;
}

static int write_file(lua_State *L, const char *mode)
{
    REAL(p, L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    FILE *f = fopen(p, mode);
    if (!f)
        return push_err(L, errno);
    int err = 0;
    if (len && fwrite(data, 1, len, f) != len)
        err = errno ? errno : EIO;
    if (fclose(f) != 0 && !err)
        err = errno ? errno : EIO;
    if (err)
        return push_err(L, err);
    lua_pushboolean(L, 1);
    return 1;
}
static int l_write(lua_State *L) { return write_file(L, "wb"); }
static int l_append(lua_State *L) { return write_file(L, "ab"); }

static int l_join(lua_State *L)
{
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++)
    {
        size_t len;
        const char *s = luaL_checklstring(L, i, &len);
        if (i > 1)
        {
            /* exactly one '/' between parts */
            if (luaL_bufflen(&b) > 0 && luaL_buffaddr(&b)[luaL_bufflen(&b) - 1] == '/')
                while (len && *s == '/')
                {
                    s++;
                    len--;
                }
            else if (len == 0 || *s != '/')
                luaL_addchar(&b, '/');
        }
        luaL_addlstring(&b, s, len);
    }
    luaL_pushresult(&b);
    return 1;
}

void fs_register(lua_State *L)
{
    static const luaL_Reg fns[] = {{"list", l_list},     {"stat", l_stat},     {"exists", l_exists}, {"isdir", l_isdir},
                                   {"isfile", l_isfile}, {"mkdir", l_mkdir},   {"remove", l_remove}, {"rename", l_rename},
                                   {"copy", l_copy},     {"read", l_read},     {"write", l_write},   {"append", l_append},
                                   {"join", l_join},     {"mounts", l_mounts}, {NULL, NULL}};
    luaL_newlib(L, fns);
    lua_setglobal(L, "fs");
}
