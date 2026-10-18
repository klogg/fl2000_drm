# Userspace bridge enumeration

The installed components have separate responsibilities:

- `/usr/libexec/fl2000-enumerate`: POSIX shell enumeration logic.
- `/etc/fl2000/enumerator.conf`: bridge identities and parameters.
- `/usr/lib/systemd/system/fl2000-enumerate@.service`: per-adapter activation.
- `/usr/lib/udev/rules.d/70-fl2000.rules`: udev matching and service request.

The rule matches the I2C adapter and its FL2000 USB ancestor, rather than the
initial USB event. This ensures the I2C bus exists before enumeration begins.
Systemd fills the empty service instance with the escaped full sysfs path. The
unit binds its lifetime to that device and passes the unescaped path to the
program. Concurrent manual invocations are serialized with `flock`.

## Configuration format

The configuration is data, not a sourced shell script. Blank lines and
comments starting with `#` are ignored. Whitespace separates fields; commas
separate addresses and identification checks. Both settings are required:

```text
adapter_name FL2000 bridge I2C bus
bind_timeout 10
```

The binding timeout must be between 1 and 20 seconds. Each bridge record has:

```text
bridge TYPE MODULE DRIVER ADDRESSES IDENTITY
```

- `TYPE` is the I2C device identifier accepted by `new_device`.
- `MODULE` is the kernel module name, without parameters or a `.ko` suffix.
- `DRIVER` is the expected bound I2C driver name.
- `ADDRESSES` lists seven-bit addresses, such as `0x4c,0x4d`.
- `IDENTITY` lists `REGISTER:MASK:VALUE` checks separated by commas.

Every address, register, mask, and value uses two-digit hexadecimal notation.
All identification checks must match. The mask applies to both the register
value and the expected value, allowing revision bits to be ignored. Use only
read-only identification registers whose reads have no destructive effects.
The shipped [configuration](../userspace/enumerator.conf) supplies IT66121
data. No bridge-specific constants are embedded in the enumeration program.
Candidate records describe how to identify and instantiate supported hardware;
they do not adapt a bridge driver. A new provider must implement the component
and IRQ integration described in [the architecture](ARCHITECTURE.md). The
FL2000 kernel driver publishes adapter software-node properties before
registering the I2C bus. The supplied bridge reads explicit transport, wiring,
reset and interrupt electrical settings; it does not identify the controller.
See [adapter properties](ARCHITECTURE.md) for their meanings and defaults.
These properties are a local module interface, not upstream firmware bindings.
Native firmware-described IT6610/21/22 boards retain their ordinary graph,
power/reset, and IRQ setup when adapter properties are absent.

Enumerator records select bridge candidates and driver names. They do not set
adapter properties, program pixel timing, or supply IRQ numbers. `new_device`
creates an I2C client without a firmware graph; the bridge reads hardware
properties and waits for its adapter IRQ domain before registering a
component. This software-node path needs no Device Tree or OF support. It
obtains its IRQ from that domain at bind time. Native OF graphs retain their
existing attachment path. Changing controller wiring or transport constraints
requires updating the kernel-side device description. If no candidate matches,
enumeration fails; it does not select native VGA or create a bridge-free DRM
display.

The program uses combined I2C transfers through `i2ctransfer`, rather than
SMBus address scanning. It checks only configured addresses. Existing
configured clients are handled first, and occupied addresses are never probed.
A module is loaded and a client instantiated only after its identity matches.

## Binding and removal

An existing client of the expected type is reused. A registered driver needs
no module load, including built-in drivers with no `/sys/module` entry. Loaded
modules are also left alone. An existing bound client is accepted without
another identification read. An unbound client with a registered driver gets
one explicit reprobe through the I2C bus `drivers_probe` attribute. Restarting
the service can therefore retry an ordinary probe failure.

Success requires binding to the configured driver. Module-loading errors fail
enumeration. A client that remains unbound at the deadline is preserved for
deferred probing or a later driver load. The program does not replace clients
or try another type at an occupied address. Rejected identification candidates
never create clients. Adapter removal cancels the service; the I2C core owns
client removal.

After success, the service exits; no enumeration daemon remains running.
Normal driver binding events and DRM hotplug notifications handle subsequent
operation. Removing the USB adapter removes its I2C clients in the kernel.

## Installation and diagnostics

Install with `make install-enumerator`; use `DESTDIR` for a package staging
root. An existing `/etc/fl2000/enumerator.conf` is preserved. Reload udev
rules after installation. Reconnecting the adapter then activates enumeration
automatically. For an already-connected adapter, start its service explicitly
(use `restart` to retry an earlier failure):

```sh
adapter=$(readlink -f /sys/bus/i2c/devices/i2c-0)
unit=$(systemd-escape --path --template=fl2000-enumerate@.service "$adapter")
sudo systemctl start "$unit"
sudo journalctl -u "$unit"
```

Use the actual FL2000 bus number in place of `i2c-0`. For a direct diagnostic
run:

```sh
sudo /usr/libexec/fl2000-enumerate /sys/bus/i2c/devices/i2c-0
```

Runtime tools are POSIX shell, awk, base utilities, `modprobe` (kmod), `flock`
(util-linux) and `i2ctransfer` (i2c-tools). Automatic activation additionally
requires udev and systemd; direct execution does not. Python is used only by
repository tests.
