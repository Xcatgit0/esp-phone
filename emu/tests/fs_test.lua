local B = "/fs"
local function eq(a,b,m) if a~=b then error((m or "")..": got "..tostring(a).." want "..tostring(b),2) end end
local function fail(a,e) if a~=nil or type(e)~="string" then error("expected failure",2) end end
eq(fs.mkdir(B.."/a/b/c", true), true, "mkdir -p")
fail(fs.mkdir(B.."/a/b/c"))
eq(fs.write(B.."/a/x.txt","hello"), true, "write")
eq(fs.append(B.."/a/x.txt"," world"), true, "append")
eq(fs.read(B.."/a/x.txt"), "hello world", "read")
fail(fs.read(B.."/a/x.txt", 5))
fs.write(B.."/a/Z.bin", string.rep("\0\1",1000))
eq(#fs.read(B.."/a/Z.bin"), 2000, "binary")
local l, trunc = fs.list(B.."/a")
eq(#l, 3, "list n"); eq(l[1].name,"b","dir first"); eq(l[1].type,"dir"); eq(l[2].name,"x.txt"); eq(l[2].size,11); eq(l[3].name,"Z.bin"); eq(trunc,false)
local l2 = fs.list(B.."/a",{size=false}); eq(l2[1].size); eq(l2[1].type,"dir")
local l3,t3 = fs.list(B.."/a",{max=1}); eq(#l3,1); eq(t3,true,"truncated")
eq(fs.stat(B.."/a/x.txt").size, 11); eq(fs.stat(B.."/a").type,"dir")
eq(fs.exists(B.."/a/x.txt"),true); eq(fs.exists(B.."/nope"),false)
eq(fs.isdir(B.."/a"),true); eq(fs.isfile(B.."/a"),false); eq(fs.isfile(B.."/a/x.txt"),true)
eq(fs.copy(B.."/a/x.txt",B.."/a/b/y.txt"),true); eq(fs.read(B.."/a/b/y.txt"),"hello world")
eq(fs.rename(B.."/a/b/y.txt",B.."/a/b/c/z.txt"),true); eq(fs.exists(B.."/a/b/y.txt"),false)
fail(fs.remove(B.."/a"))
fail(fs.list(B.."/nope"))
fail(fs.read(B.."/a"))
-- path safety
fail(fs.read(B.."/../etc/passwd")); fail(fs.read("rel/path"))
fail(fs.write(B.."/a/../../x","1"))
eq(fs.exists(B.."/a/..b"),false)   -- '..b' is a normal name
fs.write(B.."/a/..b","1"); eq(fs.exists(B.."/a/..b"),true)
eq(fs.join("/fs","a","b.txt"),"/fs/a/b.txt"); eq(fs.join("/fs/","/a/"),"/fs/a/"); eq(fs.join("/fs","/a"),"/fs/a")
local r = fs.list("/"); local names={} for _,e in ipairs(r) do names[#names+1]=e.name end
print("root:", table.concat(names,","), "mounts:", table.concat(fs.mounts(),","))
eq(fs.remove(B.."/a", true), true, "rm -r"); eq(fs.exists(B.."/a"),false)
print("ALL OK")
