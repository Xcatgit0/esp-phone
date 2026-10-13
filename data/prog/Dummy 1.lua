---@param proc Process
return function(proc)
    local ok,key = requestFC(proc)
    if not ok then error("cannon request full screen") end
    print("y")
    keyboard(proc)
    print("y")
    giveFC(key)
end