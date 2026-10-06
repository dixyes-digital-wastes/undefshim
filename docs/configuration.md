# Configuration

The driver reads `us.toml` from the first volume that provides one, preferring the volume it was loaded from. It is read once, before anything is armed.

Without a file the driver uses the defaults below and says so on the screen. The serial port is found rather than named, so a machine whose firmware describes its console prints without being told anything.

## `[uart]`

Where the log goes.

The default is to ask the firmware. That is right on a machine whose tables describe its console, and silently wrong on one whose tables do not: the driver has nothing to print on, so it prints nothing, and finding out why means reading the screen. A board that carries neither an SPCR nor a DBG2 needs its port named here, and the address is then the only thing that has to be stated.

| Key | Values | Default | Meaning |
|---|---|---|---|
| `type` | `"acpi"`, `"pl011"`, `"uart8250"`, `"off"` | `"acpi"` | What the port is. `acpi` looks for the firmware's own description of it; `pl011` and `uart8250` say what it is and need a `baseAddr`; `off` means no serial output at all. |
| `table` | `"SPCR"`, `"DBG2"`, `"DSDT"` | `"SPCR"` | Which table to look in, for `acpi`. SPCR is what a firmware writes to describe the console it uses; DBG2 is its list of debug devices; the DSDT is the whole machine, and the only one of the three that states which of its devices is the port. |
| `path` | a device path | none | For `acpi` with `table = "DSDT"`: the device to read the port from, such as `_SB.COM0`. Written without the leading backslash, because a value in this file cannot contain one; the driver puts it back. |
| `baseAddr` | address | none | For `type = "pl011"` and `type = "uart8250"`: where the port is. Refused with `acpi`, which is what finds one. |
| `width` | `8`, `32` | `32` | Access size. An 8250 on a 32-bit bus spaces its byte-wide registers a word apart, so it wants `32`. Taken from the table when the port is found and nothing is written here. |
| `clock` | Hz | `0` | What drives the port. Zero leaves the line settings alone. |
| `baud` | a line rate | `0` | What the far end expects. Zero leaves the line settings alone. |
| `color` | `true`, `false` | `true` | Whether the serial log carries ANSI colour escapes. The screen parses the same sequences and keeps its colours either way, so this only affects the log. |

The line settings are programmed only when both `clock` and `baud` are given. A divisor is a function of the clock, and nothing in ACPI states one: SPCR and DBG2 carry an address, a width and a line rate, and the DSDT carries an address. A port the firmware brought up is a port that already works, so the default is to leave it as it is, and a machine that has to be told where its port is has to be told these too. This is also why an 8250's baud rate is never changed.

```toml
[uart]
type = "acpi"
table = "SPCR"
color = true
```

```toml
# A machine whose firmware describes no console, or describes it wrongly
[uart]
type = "pl011"
baseAddr = 0x94080000
clock = 200000000
baud = 115200
```

## `[log]`

How much is printed.

| Value | Printed |
|---|---|
| `"off"` | Nothing |
| `"error"` | Failures only |
| `"warn"` | Failures and things that are wrong but survivable |
| `"info"` | The above, plus the milestones and the state of each stage (default) |
| `"verbose"` | The above, plus the details a curious reader wants: addresses, counts, where each stub went |
| `"debug"` | Everything, including the progress marks and the hashes the patch lists are matched by |

Each line is `tag: message`, and the tag is coloured by the level it was written at. The level decides whether a line is written at all, so a machine set to `error` does not format the lines it is not going to print.

The other key is `showLicenses`, on by default. The driver carries the text of its own licence and of every third-party one it includes, and prints them at boot: a binary is what gets distributed, so the notices that have to travel with it have nowhere else to be. They are not a kind of log line, which is why they have a key of their own rather than being left to `debug`, but they are written at `info`, so a machine set to `warn` or below does not print them.

They go to the serial port only. A licence is a document and the screen is a status display: the screen shows the last of what was written, so a text this long would push the boot's own report off it, on exactly the machines that have no port to put that report anywhere else.

## `[ldapr]`

What is done to RCpc loads (`ldapr`, `ldaprb`, `ldaprh`) that the hardware cannot execute.

| Key | Default | Meaning |
|---|---|---|
| `imageInplaceRewrite` | `true` | Replace each one in ntoskrnl and winload with an ordinary acquire load before either runs. This is what makes the kernel usable: without it every RCpc load takes an exception. |
| `el0InplaceRewrite` | `true` | Also replace them in user-mode pages, at the moment one traps. Off means every user-mode RCpc load keeps taking the exception, which is slower and always correct. |

`imageInplaceRewrite` covers the images the boot loads. It does not cover anything loaded later, which is what `el0InplaceRewrite` and the trap path are for. Neither key can make the machine wrong by being off: what an unreplaced load costs is an exception, and the exception path is there to answer it.

The kernel's own text is never written at run time whatever these say: a modified function or `.pdata` is what its integrity check reports.

## `[stats]`

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | Keep a table of the addresses traps were taken at, in the pool. It costs a lookup per trap. |

## `[kernel]`

| Key | Default | Meaning |
|---|---|---|
| `descriptorBaseRVA` | none | The RVA of the variable holding the kernel's page table base, for the build this configuration is for. Optional: without it the kernel's own page table entries are not edited. |

This is per build. A value from another build is checked against the hardware before it is used and refused when it does not fit, so a wrong one costs a rewrite rather than the machine.

## `[patch]`

| Key | Default | Meaning |
|---|---|---|
| `dir` | `"usPatch"` | Directory of patch list files, relative to the volume `us.toml` was read from. An empty string turns the feature off. |

Each `.txt` in the directory is a list of places to write bytes, with the bytes already there, so a file that does not match its image is refused rather than applied. `tests/deploy/enumerate_sites.py` writes one from an image. The lists are not kept in the project tree.

The volume root is refused as a directory.

## `[debug]`

Switches that take a piece of the mechanism out of the way, for bringing a machine up. Every one is `false` unless it is written, and one named `notX` means the driver does X unless the switch is set: the whole of the mechanism is what a file that does not mention this table gets.

A key that has to be turned on for the machine to work does not belong here. That is what this table used to be, and it meant a shim that was loaded but not armed could look exactly like a machine that did not need one.

| Key | Default | Meaning |
|---|---|---|
| `notArmVectors` | `false` | Take the exception vectors over. Off leaves the loads the rewrite did not reach to the kernel, which cannot carry them out. |
| `notArmVectorsEl1t` | `false` | Also take the EL1t synchronous slot. Needed because Windows takes synchronous exceptions with SP_EL0 selected, which arrives at a different offset of the table; off is known to bugcheck very early. |
| `notArmHandover` | `false` | Take over the branch winload jumps into the kernel with. That branch is what tells the other stubs where the payload ended up. |
| `notVamap` | `false` | Register the notification that runs during `SetVirtualAddressMap`, which is when the payload learns the addresses it will have once the kernel's page tables are in force. The handover stub publishes them earlier; this is for a machine where that turns out not to be enough. |
| `spxStack` | `false` | Let the stub push on the SPx vector as well. This is how it was found out which vector Windows arrives on. |

Each key is read from its own section. A key under the wrong one is not read at all, which is indistinguishable from not being there, so a switch that appears to do nothing is usually in the wrong table.

The driver prints every key it read at `verbose`, including the ones the file did not mention, so the configuration a machine is running with can be read back rather than worked out.
