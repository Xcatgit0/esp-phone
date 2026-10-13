---@param proc Process
return function(proc)
    wifi.mode("sta")
    wifi.sta.config({
        ssid = "Labunma",
        password = "01082502"
    })
    collectgarbage("collect")
    wifi.start()
end