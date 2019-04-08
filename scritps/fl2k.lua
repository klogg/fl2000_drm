-- Copyright 2019 Artem Mygaiev <joculator@gmail.com>

-- Here goes Dissector

fl2k_proto = Proto("fl2k", "FL2000 Protocol")

InterfaceClass = {
    UNKNOWN = 0xFFFF,
    AV = 0x10,
}

TransferType = {
    INTERRUPT = 1,
    CONTROL = 2,
    BULK = 3,
}

ControlTransferStage = {
    SETUP = 0,
    DATA = 1,
    STATUS = 2,
}

local f_transfer = Field.new("usb.transfer_type")
local f_stage = Field.new("usb.control_stage")
local f_data_len = Field.new("usb.data_len")
local op_types = {
    [0x40] = "RD",
    [0x41] = "WR",
}

local f = fl2k_proto.fields
-- Control related
f.f_reg_op = ProtoField.uint8("fl2k.reg_op", "Register operation", base.HEX, op_types)
f.f_reg_addr = ProtoField.uint16("fl2k.reg_addr", "Register address", base.HEX)
f.f_reg_value = ProtoField.uint32("fl2k.reg_value", "Register value", base.HEX)
-- Interrupt related
f.f_irq = ProtoField.uint32("fl2k.irq", "Interrupt", base.DEC)
-- Bulk related
f.f_fb = ProtoField.bytes("fl2k.fb", "Framebuffer", base.SPACE)

function fl2k_proto.dissector(tvb, pinfo, tree)
    local t_fl2k = tree:add(fl2k_proto, tvb())
    local transfer = f_transfer()

    if (transfer.value == TransferType.CONTROL) then

        local stage = f_stage()
        pinfo.cols["info"]:set("FL2000 Registers")
        if (stage.value == ControlTransferStage.SETUP) then
            -- For future use
            local reg_op = tvb(0, 1):uint()
            local reg_addr = tvb(3, 2):le_uint()

            t_fl2k:add(f.f_reg_op, tvb(0, 1))
            t_fl2k:add_le(f.f_reg_addr, tvb(3, 2))

            if (reg_addr == 0x8020) then
                if (reg_op == 0x40) then
                    pinfo.cols["info"]:append(" I2C Check")
                else
                    pinfo.cols["info"]:append(" I2C Operation")
                end
            elseif (reg_addr == 0x8024) then
                pinfo.cols["info"]:append(" I2C Read Data")
            elseif (reg_addr == 0x8028) then
                pinfo.cols["info"]:append(" I2C Write Data")
            else
                if (reg_op == 0x40) then
                    pinfo.cols["info"]:append(" Read")
                else
                    pinfo.cols["info"]:append(" Write")
                end
            end

        elseif (stage.value == ControlTransferStage.DATA) then
            -- For future use
            local reg_value = tvb(0, 4):le_uint()

            t_fl2k:add_le(f.f_reg_value, tvb(0, 4))

        elseif (stage == ControlTransferStage.STATUS) then
        -- Do nothing
        end

    elseif (transfer.value == TransferType.INTERRUPT) then
        pinfo.cols["info"]:set("FL2000 Interrupt")
        t_fl2k:add_le(f.f_irq, tvb(0, 1))

    elseif (transfer.value == TransferType.BULK) then
        pinfo.cols["info"]:set("FL2000 Data")
        -- TODO: is it possible to parse it as image?
        t_fl2k:add_le(f.f_fb, tvb())
    else
        pinfo.cols["info"]:set("FL2000 WTF???")
    end
end

usb_control_table = DissectorTable.get("usb.control")
usb_control_table:add(InterfaceClass.UNKNOWN, fl2k_proto)

usb_interrupt_table = DissectorTable.get("usb.interrupt")
usb_interrupt_table:add(InterfaceClass.AV, fl2k_proto)

usb_bulk_table = DissectorTable.get("usb.bulk")
usb_bulk_table:add(InterfaceClass.AV, fl2k_proto)

-- Here we go with Tap

fl2k_tap = Listener.new("usb", "fl2k")

local f_reg_op = Field.new("fl2k.reg_op")
local f_reg_addr = Field.new("fl2k.reg_addr")
local f_reg_value = Field.new("fl2k.reg_value")
local f_irq = Field.new("fl2k.irq")

local i2c_devices = {
    [0x4C] = "IT66121",
}

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

local function count_i2c_regs(i2c_device, reg_addr, reg_op)
    if i2c_device and reg_addr and reg_op then
        if not i2c_regs[i2c_device] then
            i2c_regs[i2c_device] = {}
        end
        if not i2c_regs[i2c_device][reg_addr] then
            i2c_regs[i2c_device][reg_addr] = {}
        end
        if not i2c_regs[i2c_device][reg_addr][reg_op] then
            i2c_regs[i2c_device][reg_addr][reg_op] = 1
        else
            i2c_regs[i2c_device][reg_addr][reg_op] = i2c_regs[i2c_device][reg_addr][reg_op] + 1
        end
    end
end

local function analyze_i2c(op)
    local res
    local i2c_done = bit.band(op.reg_value, 0x80000000)
    if op.reg_addr == 0x8020 and op.reg_op == "RD" then
        if i2c_state == I2C_STATE.IDLE and i2c_done ~= 0 then
            i2c[i2c_idx] = {} -- start read operation
            i2c_state = I2C_STATE.READ1
            res = 0 -- suspected i2c flow
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
            res = i2c_idx -- full i2c flow recovered
            i2c_idx = i2c_idx + 1 -- finish write operation
        else
            i2c_state = I2C_STATE.IDLE
        end
    elseif op.reg_addr == 0x8020 and op.reg_op == "WR" then
        local i2c_address = i2c_devices[bit.band(op.reg_value, 0x7F)]
        local i2c_op = bit.band(bit.rshift(op.reg_value, 7), 0x01)
        local i2c_offset = bit.band(bit.rshift(op.reg_value, 8), 0xFF)
        if i2c_state == I2C_STATE.READ1 and i2c_op == 1 then
            i2c[i2c_idx].i2c_op = "I2C RD"
            i2c[i2c_idx].i2c_device = i2c_address
            i2c[i2c_idx].i2c_offset = i2c_offset
            i2c_state = I2C_STATE.READ2
            count_i2c_regs(i2c_address, i2c_offset, "RD")
        elseif i2c_state == I2C_STATE.WRITE2 and i2c_op == 0 then
            i2c[i2c_idx].i2c_op = "I2C WR"
            i2c[i2c_idx].i2c_device = i2c_address
            i2c[i2c_idx].i2c_offset = i2c_offset
            i2c_state = I2C_STATE.WRITE3
            count_i2c_regs(i2c_address, i2c_offset, "WR")
        else
            i2c_state = I2C_STATE.IDLE
        end
    elseif op.reg_addr == 0x8024 and op.reg_op == "RD" and i2c_state == I2C_STATE.READ3 then
        i2c[i2c_idx].i2c_data = op.reg_value
        i2c_state = I2C_STATE.IDLE
        res = i2c_idx -- full i2c flow recovered
        i2c_idx = i2c_idx + 1 -- finish read operation
    elseif op.reg_addr == 0x8028 and op.reg_op == "WR" and i2c_state == I2C_STATE.IDLE then
        i2c[i2c_idx] = {} -- start write operation
        i2c[i2c_idx].i2c_data = op.reg_value
        i2c_state = I2C_STATE.WRITE1
        res = 0 -- suspected i2c flow
    else
        i2c_state = I2C_STATE.IDLE
    end
    return res
end

local function count_regs(reg_addr, reg_op)
    if reg_addr and reg_op then
        if not regs[reg_addr] then
            regs[reg_addr] = {}
        end
        if not regs[reg_addr][reg_op] then
            regs[reg_addr][reg_op] = 1
        else
            regs[reg_addr][reg_op] = regs[reg_addr][reg_op] + 1
        end
    end
end

local function log_ops_setup(reg_addr, reg_op)
    ops[op_idx] = {}
    ops[op_idx].type = TransferType.CONTROL
    ops[op_idx].reg_op = reg_op
    ops[op_idx].reg_addr = reg_addr
end

local function log_ops_data(reg_value)
    local is_i2c
    ops[op_idx].reg_value = reg_value
    is_i2c = analyze_i2c(ops[op_idx])
    ops[op_idx].i2c = nil
    if is_i2c == 0 then -- start detected
        op_i2c_start_idx = op_idx
    elseif is_i2c ~= nil then -- finish detected
        local i = op_idx
        while i >= op_i2c_start_idx do
             ops[i].i2c = is_i2c
             i = i - 1
        end
    end
    op_idx = op_idx + 1
end

local function log_ops_interrupt(intr_data)
    ops[op_idx] = {}
    ops[op_idx].type = TransferType.INTERRUPT
    ops[op_idx].intr_data = intr_data
    op_idx = op_idx + 1
end

local function log_ops_bulk(fb_size)
    ops[op_idx] = {}
    ops[op_idx].type = TransferType.BULK
    ops[op_idx].fb_size = fb_size
    op_idx = op_idx + 1
end

local function sort_table(s_table)
    local ordered_keys = {}
    for k in pairs(s_table) do
        table.insert(ordered_keys, k)
    end
    table.sort(ordered_keys)
    return ordered_keys
end

function fl2k_tap.packet(pinfo, tvb, tapinfo)
    local transfer = f_transfer()
    if (transfer.value == TransferType.CONTROL) then
        local stage = f_stage()
        if (stage.value == ControlTransferStage.SETUP) then
            local reg_addr = f_reg_addr().value
            local reg_op = op_types[f_reg_op().value]
            log_ops_setup(reg_addr, reg_op)
            count_regs(reg_addr, reg_op)
        elseif (stage.value == ControlTransferStage.DATA) then
            local reg_value = f_reg_value().value
            log_ops_data(reg_value)
        end
    elseif (transfer.value == TransferType.INTERRUPT) then
        local intr_data = f_irq().value
        log_ops_interrupt(intr_data)
    elseif (transfer.value == TransferType.BULK) then
        local fb_size = f_data_len().value
        log_ops_bulk(fb_size)
    end
end

function fl2k_tap.draw()
    local ordered_keys = {}
    print ("========= FL2000 register Statistics =========")
    ordered_keys = sort_table(regs)
    for i = 1, #ordered_keys do
        print(string.format("0x%04X", ordered_keys[i]), regs[ordered_keys[i]]["RD"], regs[ordered_keys[i]]["WR"])
    end
    print ("========= IT66121 register Statistics =========")
    ordered_keys = sort_table(i2c_regs["IT66121"])
    for i = 1, #ordered_keys do
        print(string.format("0x%02X", ordered_keys[i]), i2c_regs["IT66121"][ordered_keys[i]]["RD"], i2c_regs["IT66121"][ordered_keys[i]]["WR"])
    end
    print ("=========== Operations list ===========")
    op_idx = 1
    i2c_idx = nil
    while (ops[op_idx])
    do
        if ops[op_idx].type == TransferType.CONTROL then
            if (ops[op_idx].i2c == nil) then -- this is pure reg operation
                print(string.format("REG %s 0x%04X : 0x%08X", ops[op_idx].reg_op, ops[op_idx].reg_addr, ops[op_idx].reg_value))
            elseif (ops[op_idx].i2c ~= i2c_idx) then -- this is an i2c operation
                i2c_idx = ops[op_idx].i2c
                print(string.format("%s %s: 0x%02X : 0x%08X", i2c[i2c_idx].i2c_op, i2c[i2c_idx].i2c_device, i2c[i2c_idx].i2c_offset, i2c[i2c_idx].i2c_data))
            end
        elseif ops[op_idx].type == TransferType.INTERRUPT then
            print("INTERRUPT", ops[op_idx].intr_data)
        elseif ops[op_idx].type == TransferType.BULK then
            print("FRAME", ops[op_idx].fb_size)
        end
        op_idx = op_idx + 1
    end
end
