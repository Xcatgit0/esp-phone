---@param proc Process
return function(proc)
    local delay = function(ms) proc:delay(ms) end
    local touch = require "touch"
    local ok, key = requestFC(proc)
    local isOwnedKey = true
    if not ok then error("cannot request full screen") end
    local bg = false

    math.randomseed(time.millis())

    -- State: "menu", "game", "settings"
    local state = "menu"

    -- Settings configuration
    local speed_idx = 2 -- 1: Slow (200ms), 2: Normal (130ms), 3: Fast (80ms)
    local speeds = {200, 130, 80}
    local speed_names = {"Slow", "Normal", "Fast"}

    local grid_idx = 2 -- 1: 8px, 2: 10px, 3: 16px
    local grid_sizes = {8, 10, 16, 20, 25}
    local size_names = {"8px", "10px", "16px","20px","25px"}

    -- Game variables (เปลี่ยนจาก apple เดี่ยว เป็น apples ตารางเก็บแอปเปิ้ลหลายลูก)
    local snake, dir, next_dir, apples, score, spawn_timer, last_tick

    local function init_game()
        local gsz = grid_sizes[grid_idx]
        local cols = math.floor(320 / gsz)
        local rows = math.floor((240 - 25) / gsz)
        local start_x = math.floor(cols / 2)
        local start_y = math.floor(rows / 2)
        
        snake = {
            {x = start_x, y = start_y},
            {x = start_x - 1, y = start_y},
            {x = start_x - 2, y = start_y}
        }
        dir = "RIGHT"
        next_dir = "RIGHT"
        score = 0
        
        -- เริ่มต้นเกมด้วยแอปเปิ้ล 2 ลูก
        apples = {
            {x = math.random(0, cols - 1), y = math.random(0, rows - 1)},
            {x = math.random(0, cols - 1), y = math.random(0, rows - 1)}
        }
        
        spawn_timer = time.millis()
        last_tick = time.millis()
    end

    -- Menu background slithering snake variables
    local menu_snake = { {x=5, y=5}, {x=4, y=5}, {x=3, y=5}, {x=2, y=5} }
    local menu_dir = "RIGHT"
    local menu_tick = time.millis()
    proc:delay(250)

    while (proc.m.receiveSignal(proc) == proc.m.SIG_NON) do
        if proc.signal == proc.m.SIG_TERM or proc.signal == proc.m.SIG_KILL then
            proc.status = proc.m.PROCESS_DEAD
            coroutine.yield()
        end

        bg = not proc:canDraw()
        if bg then giveFC(key); isOwnedKey = false end
        if not isOwnedKey then ok, key = requestFC(proc); isOwnedKey = true end

        if proc:canDraw() then
            local current_time = time.millis()
            local rx, ry = touch.read()

            if state == "menu" then
                -- Update menu background snake animation
                if current_time - menu_tick > 150 then
                    menu_tick = current_time
                    local head = {x = menu_snake[1].x, y = menu_snake[1].y}
                    local cols_m = math.floor(320 / 12)
                    local rows_m = math.floor(240 / 12)

                    if menu_dir == "RIGHT" then head.x = head.x + 1
                    elseif menu_dir == "LEFT" then head.x = head.x - 1
                    elseif menu_dir == "UP" then head.y = head.y - 1
                    elseif menu_dir == "DOWN" then head.y = head.y + 1 end

                    if head.x < 0 or head.x >= cols_m or head.y < 0 or head.y >= rows_m or math.random() < 0.15 then
                        local dirs = {"UP", "DOWN", "LEFT", "RIGHT"}
                        menu_dir = dirs[math.random(1, #dirs)]
                    else
                        table.insert(menu_snake, 1, head)
                        table.remove(menu_snake)
                    end
                end

                -- Handle Menu Touch Interactions
                if rx and ry then
                    if rx >= 80 and rx <= 240 and ry >= 90 and ry <= 125 then
                        -- PLAY
                        init_game()
                        cursor.visible(false)
                        state = "game"
                        delay(250)
                    elseif rx >= 80 and rx <= 240 and ry >= 135 and ry <= 170 then
                        -- SETTINGS
                        cursor.visible(true)
                        state = "settings"
                        delay(250)
                    elseif rx >= 80 and rx <= 240 and ry >= 180 and ry <= 215 then
                        -- QUIT (Terminate process)
                        proc.status = proc.m.PROCESS_DEAD
                        giveFC(key)
                        coroutine.yield()
                    end
                end

                -- Draw Menu UI
                gfx.rect(0, 0, 320, 240, gfx.rgb(0x101018), true)
                for _, seg in ipairs(menu_snake) do
                    gfx.rect(seg.x * 12, seg.y * 12, 11, 11, gfx.rgb(0x1C3A27), true)
                end

                gfx.text(95, 35, "SNAKE GAME", gfx.rgb(0x00FF88), gfx.rgb(0x101018))

                -- Play Button
                gfx.rect(80, 90, 160, 35, gfx.rgb(0x203040), true)
                gfx.rect(80, 90, 160, 35, gfx.rgb(0x00AAFF), false)
                gfx.text(138, 100, "PLAY", gfx.rgb(0xFFFFFF), gfx.rgb(0x203040))

                -- Settings Button
                gfx.rect(80, 135, 160, 35, gfx.rgb(0x203040), true)
                gfx.rect(80, 135, 160, 35, gfx.rgb(0x00AAFF), false)
                gfx.text(125, 145, "SETTINGS", gfx.rgb(0xFFFFFF), gfx.rgb(0x203040))

                -- Quit Button
                gfx.rect(80, 180, 160, 35, gfx.rgb(0x402020), true)
                gfx.rect(80, 180, 160, 35, gfx.rgb(0xFF4444), false)
                gfx.text(138, 190, "QUIT", gfx.rgb(0xFFFFFF), gfx.rgb(0x402020))

                gfx.push()

            elseif state == "settings" then
                if rx and ry then
                    if rx >= 160 and rx <= 280 and ry >= 70 and ry <= 100 then
                        speed_idx = (speed_idx % 3) + 1
                        delay(200)
                    elseif rx >= 160 and rx <= 280 and ry >= 120 and ry <= 150 then
                        grid_idx = (grid_idx % 5) + 1
                        delay(200)
                    elseif rx >= 100 and rx <= 220 and ry >= 185 and ry <= 220 then
                        state = "menu"
                        delay(200)
                    end
                end

                gfx.rect(0, 0, 320, 240, gfx.rgb(0x101018), true)
                gfx.text(110, 25, "SETTINGS", gfx.rgb(0xFFFFFF), gfx.rgb(0x101018))

                gfx.text(30, 78, "Speed:", gfx.rgb(0xAAAAAA), gfx.rgb(0x101018))
                gfx.rect(160, 70, 120, 30, gfx.rgb(0x203040), true)
                gfx.rect(160, 70, 120, 30, gfx.rgb(0x00AAFF), false)
                gfx.text(192, 78, speed_names[speed_idx], gfx.rgb(0xFFFFFF), gfx.rgb(0x203040))

                gfx.text(30, 128, "Sprite Size:", gfx.rgb(0xAAAAAA), gfx.rgb(0x101018))
                gfx.rect(160, 120, 120, 30, gfx.rgb(0x203040), true)
                gfx.rect(160, 120, 120, 30, gfx.rgb(0x00AAFF), false)
                gfx.text(195, 128, size_names[grid_idx], gfx.rgb(0xFFFFFF), gfx.rgb(0x203040))

                gfx.rect(100, 185, 120, 35, gfx.rgb(0x303030), true)
                gfx.rect(100, 185, 120, 35, gfx.rgb(0x888888), false)
                gfx.text(142, 195, "BACK", gfx.rgb(0xFFFFFF), gfx.rgb(0x303030))

                gfx.push()

            elseif state == "game" then
                local gsz = grid_sizes[grid_idx]
                local cols = math.floor(320 / gsz)
                local rows = math.floor((240 - 25) / gsz)

                -- Control input (Touch menu return)
                if rx and ry then
                    if ry < 25 then
                        state = "menu"
                        cursor.visible(true)
                        proc:delay(250)
                    end
                end

                -- Second Joystick control using cursor.raw(2) (-1 to 1 range)
                local jx, jy = cursor.raw(1)
                if jx ~= nil and jy ~= nil then
                    if jx < -0.5 and dir ~= "LEFT" then next_dir = "RIGHT"
                    elseif jx > 0.5 and dir ~= "RIGHT" then next_dir = "LEFT"
                    elseif jy < -0.5 and dir ~= "DOWN" then next_dir = "UP"
                    elseif jy > 0.5 and dir ~= "UP" then next_dir = "DOWN" end
                end

                -- Spawn Apple เพิ่มขึ้นทุก ๆ 3.5 วินาที (3500 ms) โดยจำกัดสูงสุดไม่เกิน 7 ลูก
                if current_time - spawn_timer > 2000 then
                    spawn_timer = current_time
                    if #apples < 7 then
                        table.insert(apples, {x = math.random(0, cols - 1), y = math.random(0, rows - 1)})
                    end
                end

                -- Snake Movement Tick
                if current_time - last_tick >= speeds[speed_idx] then
                    last_tick = current_time
                    dir = next_dir

                    local head = {x = snake[1].x, y = snake[1].y}
                    if dir == "RIGHT" then head.x = head.x + 1
                    elseif dir == "LEFT" then head.x = head.x - 1
                    elseif dir == "UP" then head.y = head.y - 1
                    elseif dir == "DOWN" then head.y = head.y + 1 end

                    -- Wall Collision or Self Collision check
                    local hit = (head.x < 0 or head.x >= cols or head.y < 0 or head.y >= rows)
                    if not hit then
                        for _, seg in ipairs(snake) do
                            if seg.x == head.x and seg.y == head.y then
                                hit = true
                                break
                            end
                        end
                    end

                    if hit then
                        state = "menu"
                        cursor.visible(true)
                        proc:delay(500)
                    else
                        table.insert(snake, 1, head)
                        
                        -- ตรวจสอบการกินแอปเปิ้ล (เช็คทุกๆ ลูกในตาราง apples)
                        local ate_apple = false
                        for i = #apples, 1, -1 do
                            if head.x == apples[i].x and head.y == apples[i].y then
                                score = score + 1
                                table.remove(apples, i)
                                ate_apple = true
                            end
                        end

                        -- ถ้าไม่ได้กินแอปเปิ้ล ให้ตัดหางออกตามปกติ (เคลื่อนที่) แต่ถ้ากิน งูจะยาวขึ้น
                        if not ate_apple then
                            table.remove(snake)
                        end
                    end
                end

                -- Render Game
                gfx.rect(0, 0, 320, 240, gfx.rgb(0x101018), true)

                -- Status Bar
                gfx.rect(0, 0, 320, 25, gfx.rgb(0x1E1E28), true)
                gfx.text(10, 5, "Score: " .. score, gfx.rgb(0x00FF88), gfx.rgb(0x1E1E28))
                gfx.text(240, 5, "[ MENU ]", gfx.rgb(0x00AAFF), gfx.rgb(0x1E1E28))

                -- Board Area
                gfx.rect(0, 25, 320, 215, gfx.rgb(0x15151C), true)

                -- Draw All Apples
                for _, ap in ipairs(apples) do
                    gfx.rect(ap.x * gsz, 25 + ap.y * gsz, gsz - 1, gsz - 1, gfx.rgb(0xFF3333), true)
                end

                -- Draw Snake
                for i, seg in ipairs(snake) do
                    local col = (i == 1) and gfx.rgb(0x00FF88) or gfx.rgb(0x00AA55)
                    gfx.rect(seg.x * gsz, 25 + seg.y * gsz, gsz - 1, gsz - 1, col, true)
                end

                gfx.push()
            end

            proc:delay(16.667)
        else
            proc:delay(16.667)
        end
    end
    giveFC(key)
end