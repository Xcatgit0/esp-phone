---@param proc Process
return function(proc)
    wifi.mode("sta")
    if emu == nil then
    wifi.sta.config({
        ssid = "Labunma",
        password = "01082502"
    })
    else
        wifi.sta.config({
        ssid = "emu",
        password = ""
    })
    end
    collectgarbage("collect")
    wifi.start()
end