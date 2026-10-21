#ifndef LUA_FS_H
#define LUA_FS_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lua API `fs` (POSIX opendir/stat/mkdir/... over the ESP-IDF VFS: works for any mounted file system, /fs, /usb, ...).
 * Call once from API_INIT():  fs_register(L);
 *
 * Paths must be absolute ("/fs/music/a.mp3"), at most 255 characters, and may not contain ".." segments.
 * Every function returns  nil, "error text"  on failure.
 *
 *   fs.list(path [, opts])      -> { {name=, type="file"|"dir", size=, mtime=}, ... }
 *       opts = { size=true, sort=true, max=1000, hidden=true }   (size=false is much faster on a USB stick)
 *       fs.list("/") lists the mount points that exist (see FS_MOUNTS) as directories.
 *   fs.stat(path)               -> { type=, size=, mtime= }
 *   fs.exists(path)             -> bool
 *   fs.isdir(path) / fs.isfile(path) -> bool
 *   fs.mkdir(path [, parents])  -> true          parents=true creates missing parent directories
 *   fs.remove(path [, recursive]) -> true        a file, or a directory (empty, or recursive=true)
 *   fs.rename(from, to)         -> true
 *   fs.copy(from, to)           -> true          file -> file, overwrites
 *   fs.read(path [, max_bytes]) -> string        default limit FS_READ_DEFAULT_MAX (256 KB)
 *   fs.write(path, data)        -> true          (replaces the file)
 *   fs.append(path, data)       -> true
 *   fs.join(a, b, ...)          -> "a/b/..."     (pure string helper)
 *   fs.mounts()                 -> { "/fs", "/usb" }  mount points that currently exist
 */
void fs_register(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* LUA_FS_H */
