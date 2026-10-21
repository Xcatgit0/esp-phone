local browse = require "browse"
---@param proc Process
return function(proc)
    local ok, key = requestFC(proc)
    for k,v in pairs(browse(proc)) do
        print(k,v)
    end
    giveFC(key)
end