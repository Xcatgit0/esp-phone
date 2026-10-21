---@class ProcessManager
local process = {}

---@class Process
---@field name string
---@field source string
---@field status number
---@field c? thread
---@field waketime number
---@field delay fun(self: Process, ms: number)
---@field poll fun(self: Process, ids: integer[]): any, any
---@field pbutton table<number, boolean>
---@field polling boolean
---@field cpuTime integer
---@field m ProcessManager
---@field runState "foreground" | "background"
---@field canDraw fun(self:Process): boolean
---@field signal integer
---@field receiveSignal fun(self:Process): integer
---@type Process[]
process.procs = {}
process.PROCESS_READY      = 0
process.PROCESS_RUNNING    = 1
process.PROCESS_SLEEP      = 2
process.PROCESS_WAIT       = 3
process.PROCESS_STOPPED    = 4
process.PROCESS_DEAD       = 5
--- normal (none signal)
process.SIG_NON         = 0
--- terminate
process.SIG_TERM        = 1
--- kill
process.SIG_KILL        = 2
--- attention
process.SIG_ATTEN       = 3
---@param proc Process
---@return integer
function process.receiveSignal(proc)
    local sig = proc.signal
    proc.signal = process.SIG_NON
    return sig
end
---@param name string?
---@param file string
---@param background boolean?
---@return Process
function process.create(name, file,background)
    background = background or false
    ---@type Process
local proc = {
    signal = process.SIG_NON,
    m = process,
    name = name or "-",
    source = file,
    status = process.PROCESS_READY,
    c = nil,
    waketime = 0,
    polling = false,
    receiveSignal = function(self) return process.receiveSignal(self) end,
    pbutton = {
        [0] = false,
        [1] = false,
        [2] = false,
        [3] = false,
        [4] = false
    },
    runState = (background and "background") or "foreground",
    cpuTime = 0,
    ---@param ms number
    delay = function(self, ms)
        ms = math.max(0, ms or 0)

        self.waketime = time.millis() + ms
        self.status = process.PROCESS_WAIT
        self.polling = false

        return coroutine.yield()
    end,

    ---@param ids integer[]
    poll = function(self, ids)
        self.pbutton = {
            [0] = false,
            [1] = false,
            [2] = false,
            [3] = false,
            [4] = false
        }

        for _, id in ipairs(ids) do
            self.pbutton[id] = true
        end

        self.waketime = 0
        self.status = process.PROCESS_WAIT
        self.polling = true
        return coroutine.yield()
    end,
    canDraw = function(self)
        return self.runState=="foreground"
    end
}

    function proc:delay(ms)

        ms = math.max(0, ms or 0)

        self.waketime = time.millis() + ms
        self.status = process.PROCESS_WAIT
        self.polling = false

        return coroutine.yield()
    end


    function proc:poll(ids)

        self.pbutton = {
            [0] = false,
            [1] = false,
            [2] = false,
            [3] = false,
            [4] = false
        }

        for _, id in ipairs(ids) do
            self.pbutton[id] = true
        end

        self.waketime = 0
        self.status = process.PROCESS_WAIT
        self.polling = true

        return coroutine.yield()
    end


    local code = readfile(file)

    if not code then
        proc.status = process.PROCESS_DEAD
        table.insert(process.procs, proc)
        return proc
    end


    local func, err = load(code,"@"..file)

    if not func then
        error(err)
    end


    proc.c = coroutine.create(function()
        func()(proc)
        proc.status = process.PROCESS_DEAD
    end)

    proc.status = process.PROCESS_RUNNING

    local ok, resumeErr = coroutine.resume(proc.c)

    if not ok then

        proc.status = process.PROCESS_DEAD

        print(
            "Process error [" ..
            proc.name ..
            "]: " ..
            tostring(resumeErr)
        )

    elseif coroutine.status(proc.c) == "dead" then

        proc.status = process.PROCESS_DEAD
    end


    table.insert(process.procs, proc)

    return proc
end

function process.table()
    local totalTime = 0
    for i=1,#process.procs do
        totalTime = process.procs[i].cpuTime + totalTime
    end
    if totalTime == 0 then
    return {}
    end
    local tbl = {}
    for i=1,#process.procs do
        local proc = process.procs[i]
        tbl[proc.name] = ((proc.cpuTime/totalTime) * 100)
    end
    return tbl
end
local function is_vm_interrupted(err)
    return type(err) == "string"
        and err:match(":%d+:%s*emu:%s*VM interrupted %(reset%)$") ~= nil
end
function process.scheduler()
    cursor.poll_flush()
    while true do

        --------------------------------------------------
        -- Current time
        --------------------------------------------------

        local now = time.millis()

        local minTime = math.huge


        --------------------------------------------------
        -- Remove dead processes
        -- และหา wakeTime ที่ต่ำที่สุด
        --------------------------------------------------

        for i = #process.procs, 1, -1 do

            local proc = process.procs[i]

            if (proc.status == process.PROCESS_DEAD) or proc.signal == process.SIG_KILL then

                table.remove(process.procs, i)

            elseif
                proc.status == process.PROCESS_WAIT
                and not proc.polling
                and proc.waketime ~= 0
            then

                if proc.waketime < minTime then
                    minTime = proc.waketime
                end
            end
        end


        --------------------------------------------------
        -- absolute wakeTime
        --
        --        ↓
        --
        -- relative timeout
        --------------------------------------------------

        local timeout = math.huge

        if minTime ~= math.huge then
            timeout = math.max(0, minTime - now)
        end


        --------------------------------------------------
        -- รอ event / timeout
        --------------------------------------------------

        local change, d = cursor.poll(
            {0, 1, 2, 3, 4},
            timeout
        )


        --------------------------------------------------
        -- poll อาจใช้เวลาผ่านไป
        -- ต้องอ่านเวลาใหม่
        --------------------------------------------------

        now = time.millis()


        --------------------------------------------------
        -- ตรวจ process
        --------------------------------------------------

        for _, proc in ipairs(process.procs) do

            if proc.status == process.PROCESS_WAIT then

                --------------------------------------------------
                -- Cursor event
                --------------------------------------------------

                if
                    proc.polling
                    and change ~= nil
                    and proc.pbutton[change]
                then

                    proc.polling = false
                    proc.waketime = 0
                    proc.status = process.PROCESS_RUNNING
                    local beforeRun = time.millis()
                    local ok, err =
                        coroutine.resume(
                            proc.c,
                            change,
                            d
                        )
                    local cputime = time.millis() - beforeRun
                    proc.cpuTime = cputime
                    
                    if not ok then

                        proc.status = process.PROCESS_DEAD

                        print(
                            "Process error [" ..
                            proc.name ..
                            "]: " ..
                            tostring(err)
                        )
                        if proc.runState=="foreground" then
                            giveFC(0,true)
                        end
                        print(err)
                        if is_vm_interrupted(err) then return end
                    elseif coroutine.status(proc.c) == "dead" then

                        proc.status = process.PROCESS_DEAD
                    end
                end


                --------------------------------------------------
                -- delay timeout
                --------------------------------------------------

                if
                    not proc.polling
                    and proc.waketime ~= 0
                    and proc.waketime <= now
                then

                    proc.waketime = 0
                    proc.status = process.PROCESS_RUNNING
                    local beforeRun = time.millis()
                    local ok, err =
                        coroutine.resume(proc.c, 0)
                    local cputime = time.millis() - beforeRun
                    proc.cpuTime = cputime
                    --print(cputime)
                    if not ok then

                        proc.status = process.PROCESS_DEAD

                        print(
                            "Process error [" ..
                            proc.name ..
                            "]: " ..
                            tostring(err)
                        )
                        if proc.runState=="foreground" then
                            giveFC(0,true)
                        end
                        print(err)
                        if is_vm_interrupted(err) then return end
                    elseif coroutine.status(proc.c) == "dead" then

                        proc.status = process.PROCESS_DEAD
                    end
                end
            end
        end
    end
end


return process