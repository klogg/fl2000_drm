# Register definition audit

Audited against the supplied FL200DX memory-mapped register manual,
sections 40.1.1.1-40.1.1.32, the ITE IT66121/IT66120 HDMI Tx Register
Definitions v1.0 (2011-11-29), and Linux v7.0's IT66121 definitions.
Unused definitions are retained for future development.

## FL2000

All 32 VGA register offsets in the supplied manual are represented. Every
non-reserved field mask in those tables is represented, including the pattern
generator, full I2C data words, combined PLL pre-divider byte, and the complete
16-bit control register at 0x8088. Reserved fields and test-only register
contents are not assigned invented meanings. The USB bank at 0x0070/0x0078
is outside this manual's register tables and remains vendor-derived.

The audit corrected the LBUF labels: overflow is bit 9 and underflow is bit 8
of 0x8000. Existing interrupt processing handles their combined mask, so this
label correction does not change the values read or written by the driver.

The following ambiguities remain explicit:

- The manual describes 0x802c bits 15:8 as one pre-divider byte. The vendor
  programming model splits this byte into prescaler and function fields.
  Both representations are retained; the audit does not change PLL encoding.
- The 0x8054 table assigns both BIAC status and current value to bits 15:0.
  The existing upper-word value definition is retained pending clarification.
- The manual describes status bits 8/9 as cleared by writing bit 31, although
  bit 31 is also an EDID event. This audit does not change acknowledgment.
- Hardware tests found 0x803c bit 4 behaves opposite to the documented
  read-to-clear enable. The tested configuration remains unchanged.
- The 0x8078 register's printed address range overlaps the next register.
  Its 32-bit offset is retained as 0x8078.

## IT66121 and shared bridge variants

The [vendor register list](https://www.scribd.com/document/1011001563/IT66121-Register-List-Release-V1-0)
was checked alongside the
[Linux v7.0 driver](https://github.com/torvalds/linux/blob/v7.0/drivers/gpu/drm/bridge/ite-it66121.c).
All 217 numeric definitions shared with that driver retain identical values.
The additions describe existing control-register fields: the full bank selector,
input color/clock/delay selection, input FIFO controls, CSC selection, AFE drive
and clock selection, interrupt output enable/reference-clock controls, and CTS
interrupt step selection. They do not alter the driver's register writes.

This header is **not a complete silicon register catalogue**. The vendor list
also describes HDCP controls and authentication data, clock/status fields,
CSC matrix coefficients, pattern/sync/DE generation, additional interrupt
status/mask/clear banks, packet-generator controls, packet payload layouts,
extended interrupt controls, and a separate CEC slave register map. Those
blocks are only partially represented or absent. Future support should add
definitions from the relevant device revision rather than infer them from
neighboring addresses. CEC offsets belong to their own I2C slave, not the HDMI
banked regmap.

The vendor document predates the current upstream driver and does not establish
identical layouts for every supported chip variant. For example, it exposes
an audio CTS counter across 0x135-0x137 while upstream retains counter registers
at 0x1a0-0x1a2. It leaves some CSC bits unnamed that upstream uses for dithering
and filtering. Such differences are recorded rather than changing shared
upstream definitions on the strength of one older document. Existing chip,
bus-width, chaining, and register-programming behavior is preserved.

The AVI layout also needs separate investigation: both the vendor list and
existing definitions place DB1-DB5 at 0x158-0x15c, checksum at 0x15d, and
DB6-DB13 at 0x15e-0x165. The current bridge, like Linux v7.0, writes 13 payload
bytes contiguously from 0x158 and then writes checksum at 0x15d. Under that
layout, payload byte 6 is overwritten and byte 13's register is not written.
This audit records the inconsistency without changing shared programming.
Resolve it with a revision-specific programming reference and an independent
packet-layout regression before changing the write sequence.

The previously indexed programming-guide PDF mirror returned HTTP 404 during
this audit; the register-list document was used directly instead.
