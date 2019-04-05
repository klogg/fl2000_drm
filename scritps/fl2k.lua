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

fl2k_tap = Listener.new("usb", "fl2k && (usb.transfer_type == 2) && (usb.control_stage != 3)")

local pretty = require 'pl.pretty'

local f_reg_op = Field.new("fl2k.reg_op")
local f_reg_addr = Field.new("fl2k.reg_addr")
local f_reg_value = Field.new("fl2k.reg_value")

I2C_STATE = {
    IDLE = 0,   -- regular
    WRITE = 1,  -- prepared for write operation (wrote to I2C_WR_DATA)
    READ = 2,   -- prepared for read operation (programmed I2C_CTRL)
}
local i2c_devices = {
    [0x4C] = "IT66121",
}
local state = I2C_STATE.IDLE
local i2c = {}
local i2c_idx = 1

local function analyze_i2c(op)
    local res = false
    --[[ Programming I2C operation ]]--
    if op.reg_addr == 0x8020 and op.reg_op == "WR" and bit.band(op.reg_value, 0x80000000) == 0 then
        local i2c_address = bit.band(op.reg_value, 0x7F)
        local i2c_op = bit.band(bit.rshift(op.reg_value, 7), 0x01)
        local i2c_offset = bit.band(bit.rshift(op.reg_value, 8), 0xFF)

        if i2c_op == 0 and state == I2C_STATE.WRITE then
            state = I2C_STATE.IDLE
            i2c[i2c_idx].i2c_op = "I2C WR"
            i2c[i2c_idx].i2c_device = i2c_devices[i2c_address]
            i2c[i2c_idx].i2c_offset = i2c_offset
            i2c_idx = i2c_idx + 1 -- finish write operation
            res = true

        elseif i2c_op == 1 and state == I2C_STATE.IDLE then -- Read I2C
            state = I2C_STATE.READ
            i2c[i2c_idx] = {} -- start read operation
            i2c[i2c_idx].i2c_op = "I2C RD"
            i2c[i2c_idx].i2c_device = i2c_devices[i2c_address]
            i2c[i2c_idx].i2c_offset = i2c_offset
            res = true
        end

        --[[ Write I2C data ]]--
    elseif op.reg_addr == 0x8028 and op.reg_op == "WR" then
        if state == I2C_STATE.IDLE then
            state = I2C_STATE.WRITE
            i2c[i2c_idx] = {} -- start write operation
            i2c[i2c_idx].i2c_data = op.reg_value
            res = true
        end

        --[[ Read I2C data ]]--
    elseif op.reg_addr == 0x8024 and op.reg_op == "RD" then
        if state == I2C_STATE.READ then
            state = I2C_STATE.IDLE
            i2c[i2c_idx].i2c_data = op.reg_value
            i2c_idx = i2c_idx + 1 -- finish read operation
            res = true
        end

        --[[ Checking I2C operation status ]]--
    elseif op.reg_addr == 0x8020 and op.reg_op == "RD" then
        if state ~= I2C_STATE.IDLE and bit.band(op.reg_value, 0x80000000) ~= 0 then
            res = true
        end
    end
    return res
end

local regs = {}     -- count register access statistics

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

local ops = {}      -- array of register operations
local op_idx = 1

local function log_ops_setup(reg_addr, reg_op)
    if reg_op and reg_addr then
        ops[op_idx] = {}
        ops[op_idx].reg_op = reg_op
        ops[op_idx].reg_addr = reg_addr
        count_regs(reg_addr, reg_op)
    end
end

local function log_ops_data(reg_value)
    if reg_value then
        ops[op_idx].reg_value = reg_value
        ops[op_idx].i2c = analyze_i2c(ops[op_idx])
        op_idx = op_idx + 1
    end
end

function fl2k_tap.packet(pinfo, tvb, tapinfo)
    local stage = f_stage()
    if (stage.value == ControlTransferStage.SETUP) then
        local reg_addr = f_reg_addr().value
        local reg_op = op_types[f_reg_op().value]
        log_ops_setup(reg_addr, reg_op)
    elseif (stage.value == ControlTransferStage.DATA) then
        local reg_value = f_reg_value().value
        log_ops_data(reg_value)
    end
end

function fl2k_tap.draw()
    print ("========= Register Statistics =========")
    pretty.dump(regs)

    print ("=========== Operations list ===========")
    pretty.dump(ops)

    print ("=========== I2C oprtations ============")
    idx = 1
    while (i2c[idx] ~= nil)
    do
        print(string.format("%s %s: 0x%02X : 0x%08X", i2c[idx].i2c_op, i2c[idx].i2c_device, i2c[idx].i2c_offset, i2c[idx].i2c_data))
        idx = idx + 1
    end
end
