-- Copyright 2019 Artem Mygaiev <joculator@gmail.com>

-- Here goes Dissector

fl2k_proto = Proto("fl2k", "FL2000 Protocol")

TransferType = {
    INTERRUPT = 1,
    CONTROL = 2,
    BULK = 3,
}

local op_types = {
    [0x40] = "RD",
    [0x41] = "WR",
}

local f = fl2k_proto.fields
-- Control related
f.f_reg_op = ProtoField.uint8("fl2k.reg_op", "Register operation", base.HEX)
f.f_reg_addr = ProtoField.uint16("fl2k.reg_addr", "Register address", base.HEX)
f.f_reg_value = ProtoField.uint32("fl2k.reg_value", "Register value", base.HEX)
-- Interrupt related
f.f_irq = ProtoField.uint32("fl2k.irq", "Interrupt", base.DEC)
-- Bulk related
f.f_fb = ProtoField.bytes("fl2k.fb", "Framebuffer", base.SPACE)

-- Extractors list
-- XXX: urb_type on Linux and control_stage on Windows
local f_urb_type = Field.new("usb.urb_type")
local f_control_stage = Field.new("usb.control_stage")
local f_transfer = Field.new("usb.transfer_type")
local f_frame_num = Field.new("frame.number")
local f_reg_op = Field.new("fl2k.reg_op")
local f_reg_addr = Field.new("fl2k.reg_addr")
local f_reg_value = Field.new("fl2k.reg_value")
local f_irq = Field.new("fl2k.irq")
local f_data_len = Field.new("usb.data_len")

function f_stage()
    local ControlURBType = {
        [83] = "SETUP",
        [67] = "CONTROL",
    }
    local ControlTransferStage = {
        [0] = "SETUP",
        [3] = "CONTROL",
    }
    local urb_type = f_urb_type()
    local control_stage = f_control_stage()
    if (urb_type) then
        return ControlURBType[urb_type.value]
    elseif (control_stage) then
        return ControlTransferStage[control_stage.value]
    end
end

function fl2k_proto.dissector(tvb, pinfo, tree)
    local t_fl2k = tree:add(fl2k_proto, tvb())
    local transfer = f_transfer()

    if (transfer.value == TransferType.CONTROL) then
        pinfo.cols["info"]:set("FL2000 Registers")
        if (f_stage() == "SETUP") then
            local reg_op = tvb(0, 1):uint()
            local reg_addr = tvb(3, 2):le_uint()

            t_fl2k:add_le(f.f_reg_op, tvb(0, 1))
            t_fl2k:add_le(f.f_reg_addr, tvb(3, 2))

            pinfo.cols["info"]:append(" " .. op_types[reg_op])
            if (op_types[reg_op] == "WR") then
                t_fl2k:add_le(f.f_reg_value, tvb(7, 4))
            end
        elseif (f_stage() == "CONTROL") then
            if (f_data_len().value > 0) then 
                t_fl2k:add_le(f.f_reg_value, tvb(0, 4))
            end
        end

    elseif (transfer.value == TransferType.INTERRUPT) then
        pinfo.cols["info"]:set("FL2000 Interrupt")
        t_fl2k:add_le(f.f_irq, tvb(0, 1))
    elseif (transfer.value == TransferType.BULK) then
        pinfo.cols["info"]:set("FL2000 Data")
        -- TODO: is it possible to parse it as image?
        t_fl2k:add_le(f.f_fb, tvb())
    else
        pinfo.cols["info"]:set("Unknown transfer")
    end
end

-- Register dissector with FL2000 / FrescoLogic IDs
ProductID = 0x1d5c2000
usb_product_table = DissectorTable.get("usb.product")
usb_product_table:add(ProductID, fl2k_proto)

-- XXX: It seems that 'usb.product' table does not always work. Keep option to register for interface tables 
--[[
InterfaceClass = {
    UNKNOWN = 0xFFFF,
    AV = 0x10,
}
usb_control_table = DissectorTable.get("usb.control")
usb_control_table:add(InterfaceClass.UNKNOWN, fl2k_proto)
usb_interrupt_table = DissectorTable.get("usb.interrupt")
usb_interrupt_table:add(InterfaceClass.AV, fl2k_proto)
usb_bulk_table = DissectorTable.get("usb.bulk")
usb_bulk_table:add(InterfaceClass.AV, fl2k_proto)
--]]

-- Here we go with Tap

I2C_STATE = {
    IDLE = 0,
    WRITE1 = 1,
    WRITE2 = 2,
    WRITE3 = 3,
    READ1 = 4,
    READ2 = 5,
    READ3 = 6,
}

local i2c_state = I2C_STATE.IDLE
local i2c = {}      -- array of i2c operations
local i2c_idx = 1

local i2c_regs = {}

local ops = {}      -- array of register operations
local op_idx = 1
local op_i2c_start_idx = 0

local regs = {}     -- count register access statistics

local i2c_bank = {
    ["IT66121"] = 0,
}

local function check_i2c_bank(i2c_device, i2c_offset, reg_value)
    if (i2c_device == "IT66121" and (i2c_offset == 0x00C or i2c_offset == 0x10C)) then
        if (bit.band(reg_value, 0x01000000) ~= 0) then
            i2c_bank["IT66121"] = 0x100
        else
            i2c_bank["IT66121"] = 0x000
        end
    end
end

local function count_i2c_regs(i2c_device, i2c_offset, reg_op)
    if not i2c_regs[i2c_device] then
        i2c_regs[i2c_device] = {}
    end
    if not i2c_regs[i2c_device][i2c_offset] then
        i2c_regs[i2c_device][i2c_offset] = {["RD"] = 0, ["WR"] = 0}
    end
    if not i2c_regs[i2c_device][i2c_offset][reg_op] then
        i2c_regs[i2c_device][i2c_offset][reg_op] = 1
    else
        i2c_regs[i2c_device][i2c_offset][reg_op] = i2c_regs[i2c_device][i2c_offset][reg_op] + 1
    end
end

local function analyze_i2c(op)
    local ret
    local i2c_devices = {
        [0x4C] = "IT66121",
    }
    local i2c_done = bit.band(op.reg_value, 0x80000000)
    if op.reg_addr == 0x8020 and op.reg_op == "RD" then
        if i2c_state == I2C_STATE.IDLE and i2c_done ~= 0 then
            i2c[i2c_idx] = {} -- start read operation
            i2c_state = I2C_STATE.READ1
            ret = 0 -- suspected i2c flow
        elseif i2c_state == I2C_STATE.READ2 and i2c_done == 0 then
            -- do nothing, we are still waiting for i2c to complete
        elseif i2c_state == I2C_STATE.READ2 and i2c_done ~= 0 then
            i2c_state = I2C_STATE.READ3
        elseif i2c_state == I2C_STATE.WRITE1 and i2c_done ~= 0 then
            i2c_state = I2C_STATE.WRITE2
        elseif i2c_state == I2C_STATE.WRITE3 and i2c_done == 0 then
            -- do nothing, we are still waiting for i2c to complete
        elseif i2c_state == I2C_STATE.WRITE3 and i2c_done ~= 0 then
            i2c_state = I2C_STATE.IDLE
            ret = i2c_idx -- full i2c flow recovered
            i2c_idx = i2c_idx + 1 -- finish write operation
        else
            i2c_state = I2C_STATE.IDLE
        end
    elseif op.reg_addr == 0x8020 and op.reg_op == "WR" then
        local i2c_device = i2c_devices[bit.band(op.reg_value, 0x7F)]
        local i2c_op = bit.band(bit.rshift(op.reg_value, 7), 0x01)
        local i2c_offset = bit.band(bit.rshift(op.reg_value, 8), 0xFF) + i2c_bank[i2c_device]
        if i2c_state == I2C_STATE.READ1 and i2c_op == 1 then
            i2c[i2c_idx].i2c_op = "I2C RD"
            i2c[i2c_idx].i2c_device = i2c_device
            i2c[i2c_idx].i2c_offset = i2c_offset
            i2c_state = I2C_STATE.READ2
            count_i2c_regs(i2c_device, i2c_offset, "RD")
        elseif i2c_state == I2C_STATE.WRITE2 and i2c_op == 0 then
            i2c[i2c_idx].i2c_op = "I2C WR"
            i2c[i2c_idx].i2c_device = i2c_device
            i2c[i2c_idx].i2c_offset = i2c_offset
            i2c_state = I2C_STATE.WRITE3
            count_i2c_regs(i2c_device, i2c_offset, "WR")
            -- Check if we actually switch bank now
            check_i2c_bank(i2c_device, i2c_offset, i2c[i2c_idx].i2c_data)
        else
            i2c_state = I2C_STATE.IDLE
        end
    elseif op.reg_addr == 0x8024 and op.reg_op == "RD" and i2c_state == I2C_STATE.READ3 then
        i2c[i2c_idx].i2c_data = op.reg_value
        i2c_state = I2C_STATE.IDLE
        ret = i2c_idx -- full i2c flow recovered
        i2c_idx = i2c_idx + 1 -- finish read operation
    elseif op.reg_addr == 0x8028 and op.reg_op == "WR" and i2c_state == I2C_STATE.IDLE then
        i2c[i2c_idx] = {} -- start write operation
        i2c[i2c_idx].i2c_data = op.reg_value
        i2c_state = I2C_STATE.WRITE1
        ret = 0 -- suspected i2c flow
    else
        i2c_state = I2C_STATE.IDLE
    end
    return ret
end

local function count_regs(reg_addr, reg_op)
    if not regs[reg_addr] then
        regs[reg_addr] = {["RD"] = 0, ["WR"] = 0}
    end
    if not regs[reg_addr][reg_op] then
        regs[reg_addr][reg_op] = 1
    else
        regs[reg_addr][reg_op] = regs[reg_addr][reg_op] + 1
    end
end

local function log_ops_setup(frame_num, reg_addr, reg_op)
    ops[op_idx] = {}
    ops[op_idx].num = frame_num
    ops[op_idx].type = TransferType.CONTROL
    ops[op_idx].reg_op = reg_op
    ops[op_idx].reg_addr = reg_addr
    count_regs(reg_addr, reg_op)
end

local function log_ops_data(reg_value)
    local is_i2c
    ops[op_idx].reg_value = reg_value
    ops[op_idx].i2c = nil
    is_i2c = analyze_i2c(ops[op_idx])
    if (is_i2c == 0) then -- start detected
        op_i2c_start_idx = op_idx
    elseif (is_i2c ~= nil) then -- finish detected
        local i = op_idx
        while (i >= op_i2c_start_idx) do
             ops[i].i2c = is_i2c
             i = i - 1
        end
    end
    op_idx = op_idx + 1
end

local function log_ops_interrupt(frame_num, intr_data)
    ops[op_idx] = {}
    ops[op_idx].num = frame_num
    ops[op_idx].type = TransferType.INTERRUPT
    ops[op_idx].intr_data = intr_data
    op_idx = op_idx + 1
end

local function log_ops_bulk(frame_num, fb_size)
    ops[op_idx] = {}
    ops[op_idx].num = frame_num
    ops[op_idx].type = TransferType.BULK
    ops[op_idx].fb_size = fb_size
    op_idx = op_idx + 1
end

local function sort_table(s_table)
    local ordered_keys = {}
    if s_table == nil then
        return {}
    end
    for k in pairs(s_table) do
        table.insert(ordered_keys, k)
    end
    table.sort(ordered_keys)
    return ordered_keys
end

local function reg_stats()
    local win = TextWindow.new("FL2000 Register Statistics")
    local ordered_keys = sort_table(regs)
    win:clear()
    win:append("Register address\tRD\tWR\n")
    for i = 1, #ordered_keys do
        win:append(string.format("0x%04X\t\t%d\t%d\n", ordered_keys[i], regs[ordered_keys[i]]["RD"], regs[ordered_keys[i]]["WR"]))
    end
end

local function i2c_stats(device_id)
    local win = TextWindow.new(device_id .. " Register Statistics")
    local ordered_keys = sort_table(i2c_regs[device_id])
    win:clear()
    win:append("Register address\tRD\tWR\n")
    for i = 1, #ordered_keys do
        win:append(string.format("0x%03X\t\t%d\t%d\n", ordered_keys[i], i2c_regs[device_id][ordered_keys[i]]["RD"], i2c_regs[device_id][ordered_keys[i]]["WR"]))
    end
end

local function menuable_tap()
    if not gui_enabled() then return end

    local win = TextWindow.new("Operations")

    -- XXX: When was registering Listener for "usb" some packets were not captured
    local fl2k_tap = Listener.new(nil, "fl2k")

    local function remove()
        -- this way we remove the listener that otherwise will remain running indefinitely
        fl2k_tap:remove();
    end

    -- we tell the window to call the remove() function when closed
    win:set_atclose(remove)

    -- this function will be called whenever a reset is needed
    -- e.g. when reloading the capture file
    function fl2k_tap.reset()
        win:clear()

        i2c_state = I2C_STATE.IDLE
        i2c = {}
        i2c_idx = 1

        i2c_regs = {}

        ops = {}
        op_idx = 1
        op_i2c_start_idx = 0
    end

    function fl2k_tap.packet(pinfo, tvb, tapinfo)
        local transfer = f_transfer().value
        local frame_num = f_frame_num().value
        if (transfer == TransferType.CONTROL) then
            local data_len = f_data_len().value
            if (f_stage() == "SETUP") then
                local reg_addr = f_reg_addr().value
                local reg_op = op_types[f_reg_op().value]
                log_ops_setup(frame_num, reg_addr, reg_op)
                if (reg_op == "WR") then
                    local reg_value = f_reg_value().value
                    log_ops_data(reg_value)
                end
            elseif (f_stage() == "CONTROL") then
                if (f_data_len().value > 0) then 
                    local reg_value = f_reg_value().value
                    log_ops_data(reg_value)
                end
            end
        elseif (transfer == TransferType.INTERRUPT) then
            local intr_data = f_irq().value
            log_ops_interrupt(frame_num, intr_data)
        elseif (transfer == TransferType.BULK) then
            local fb_size = f_data_len().value
            log_ops_bulk(frame_num, fb_size)
        end
    end

    function fl2k_tap.draw()
        local draw_op_idx = 1
        local draw_i2c_idx = nil
        win:clear()
        while (ops[draw_op_idx])
        do
            if ops[draw_op_idx].type == TransferType.CONTROL then
                if (ops[draw_op_idx].i2c == nil) then -- this is pure reg operation
                    win:append(string.format("[%06d] REG %s 0x%04X : 0x%08X\n", ops[draw_op_idx].num, ops[draw_op_idx].reg_op, ops[draw_op_idx].reg_addr, ops[draw_op_idx].reg_value))
                elseif (ops[draw_op_idx].i2c ~= draw_i2c_idx) then -- this is a new i2c operation
                    draw_i2c_idx = ops[draw_op_idx].i2c
                    win:append(string.format("[%06d] %s %s: 0x%03X : 0x%08X\n", ops[draw_op_idx].num, i2c[draw_i2c_idx].i2c_op, i2c[draw_i2c_idx].i2c_device, i2c[draw_i2c_idx].i2c_offset, i2c[draw_i2c_idx].i2c_data))
                end
            elseif ops[draw_op_idx].type == TransferType.INTERRUPT then
                win:append(string.format("[%06d] Interrupt - status 0x%04X\n", ops[draw_op_idx].num, ops[draw_op_idx].intr_data))
            elseif ops[draw_op_idx].type == TransferType.BULK then
                win:append(string.format("[%06d] Data Frame - size %d\n", ops[draw_op_idx].num, ops[draw_op_idx].fb_size))
            end
            draw_op_idx = draw_op_idx + 1
        end
        win:add_button("FL2000 Register Statistics", function() reg_stats() end)
        win:add_button("IT66121 Register Statistics", function() i2c_stats("IT66121") end)
    end

    -- Ensure that all existing packets are processed.
    retap_packets()
end

-- using this function we register our function
-- to be called when the user selects the Tools->Test->Packets menu
register_menu("FL2000 Analysis", menuable_tap, MENU_TOOLS_UNSORTED)
