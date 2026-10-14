-- Headless integration test of the Wireshark analysis callbacks.
-- Run with a FL2000 capture that includes enumeration and register traffic:
-- tshark -X lua_script:tests/fl2k_headless.lua \
--   -X lua_script1:scripts/fl2k.lua -r capture.pcapng -q
-- Require HEADLESS_GUI_CALLBACKS_PASS; TShark may exit zero after Lua errors.
-- Only TextWindow and menu/retap entry points are replaced. Packet dissection,
-- field extraction and listener delivery use the real Wireshark Lua API.
local test_windows, test_listeners = {}, {}
local test_menu
local original_new = Listener.new
Listener.new = function(...)
    local listener = original_new(...)
    local proxy = {raw = listener, remove = function() listener:remove() end}
    setmetatable(proxy, {__newindex = function(self, key, value)
        listener[key] = value
        rawset(self, key, value)
    end})
    test_listeners[#test_listeners + 1] = proxy
    return proxy
end
gui_enabled = function() return true end
register_menu = function(name, callback) test_menu = callback end
retap_packets = function() end
TextWindow = {new = function(title)
    local win = {title = title, text = '', buttons = {}}
    function win:clear() self.text = '' end
    function win:append(text) self.text = self.text .. text end
    function win:set_atclose(callback) self.close = callback end
    function win:add_button(label, callback)
        self.buttons[#self.buttons + 1] = {label = label, callback = callback}
    end
    test_windows[#test_windows + 1] = win
    return win
end}

assert(arg and arg[1], 'pass the driver path using -X lua_script1:path')
dofile(arg[1])

assert(test_menu, 'analysis menu was not registered')
test_menu()
local tap = assert(test_listeners[1])
local original_draw = tap.draw
local done = false
tap.draw = function()
    if done then return end
    done = true
    original_draw()
    local operations = test_windows[1]
    print('HEADLESS_OPERATIONS_BYTES=' .. #operations.text)
    assert(#operations.text > 0, 'no operations decoded')
    for _, button in ipairs(operations.buttons) do button.callback() end
    for i = 2, #test_windows do
        print('HEADLESS_STATS=' .. test_windows[i].title .. ':' .. #test_windows[i].text)
    end
    original_draw()
    assert(#operations.buttons == 2, 'redraw duplicates statistics buttons')
    tap.reset()
    original_draw()
    assert(operations.text == '', 'capture reset retains operations')
    operations.buttons[1].callback()
    local stats = test_windows[#test_windows].text
    assert(stats == 'Register address\tRD\tWR\n', 'capture reset retains register statistics')
    operations.buttons[2].callback()
    assert(test_windows[#test_windows].text == 'Register address\tRD\tWR\n', 'capture reset retains I2C statistics')
    print('HEADLESS_GUI_CALLBACKS_PASS')
end

tap.raw.draw = tap.draw
