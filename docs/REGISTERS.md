# Register references

Register semantics, coverage boundaries and unresolved layouts are documented
beside the definitions:

- [FL2000 registers](../fl2000_registers.h): controller status side effects,
  reset/detection controls, vendor USB fields and manual discrepancies.
- [IT6610/21/22 registers](../bridge/it66121_registers.h): shared bridge fields,
  variant limits and unresolved audio/AVI layouts.

Treat register additions as variant-specific hardware changes. Review their
reference material and test affected native-board and FL2000 paths before
changing definitions shared by those users.

## Reference material

The FL200DX memory-mapped register manual, sections 40.1.1.1 through
40.1.1.32, describes the controller VGA bank. The bridge reference is the ITE
IT66121/IT66120 HDMI Tx Register Definitions v1.0 (2011-11-29), available in
[the vendor register-list mirror][register-list]. The corresponding baseline
implementation is [Linux v7.0's bridge driver][upstream]. Documents for an
older chip revision do not prove compatibility with every supported variant.

[register-list]: https://www.scribd.com/document/1011001563/IT66121-Register-List-Release-V1-0
[upstream]: https://github.com/torvalds/linux/blob/v7.0/drivers/gpu/drm/bridge/ite-it66121.c
