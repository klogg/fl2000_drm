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
    [0x40] = "Read",
    [0x41] = "Write",
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

function fl2k_proto.dissector(buffer, pinfo, tree)
    local t_fl2k = tree:add(fl2k_proto, buffer())
    local transfer = f_transfer()

    if (transfer.value == TransferType.CONTROL) then

        local stage = f_stage()
        pinfo.cols["info"]:set("FL2000 Registers")
        if (stage.value == ControlTransferStage.SETUP) then
            -- For future use
            local reg_op = buffer(0, 1):uint()
            local reg_addr = buffer(3, 2):le_uint()

            t_fl2k:add(f.f_reg_op, buffer(0, 1))

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
                t_fl2k:add_le(f.f_reg_addr, buffer(3, 2))
            end

        elseif (stage.value == ControlTransferStage.DATA) then
            -- For future use
            local reg_value = buffer(0, 4):le_uint()

            t_fl2k:add_le(f.f_reg_value, buffer(0, 4))

        elseif (stage == ControlTransferStage.STATUS) then
            -- Do nothing
        end

    elseif (transfer.value == TransferType.INTERRUPT) then
        pinfo.cols["info"]:set("FL2000 Interrupt")
        t_fl2k:add_le(f.f_irq, buffer(0, 1))

    elseif (transfer.value == TransferType.BULK) then
        pinfo.cols["info"]:set("FL2000 Data")
        -- TODO: is it possible to parse it as image?
        t_fl2k:add_le(f.f_fb, buffer())

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
