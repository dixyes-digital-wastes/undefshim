# Configuration

The driver reads `us.toml` from the first volume that provides one, preferring the volume it was loaded from. It is read once, before anything is armed.

Without a file the driver uses the defaults below and says so on the screen. Note that the serial port is one of the things the file names: there is no default port, so a machine with no `us.toml` prints nothing at all, and everything it has to say has to be read off the screen.

## `[uart]`

Where the log goes.

| Key | Values | Default | Meaning |
|---|---|---|---|
| `baseAddr` | address | none | The port's base address. **Leaving this out means no serial output.** |
| `type` | `"pl011"`, `"uart8250"` | `"pl011"` | PL011 is ARM's own, 32 bits per register. The 8250 is what PC-derived boards have. |
| `width` | `8`, `32` | `32` | Access size. An 8250 on a 32-bit bus spaces its byte-wide registers a word apart, so it wants `32`. |
| `color` | `true`, `false` | `true` | Whether the serial log carries ANSI colour escapes. The screen parses the same sequences and keeps its colours either way, so this only affects the log. |

The driver does not change an 8250's baud rate: the divisor depends on a clock the configuration does not state, and writing one would break a port the firmware already set up.

```toml
[uart]
baseAddr = 0x09000000
type = "pl011"
width = 32
color = true
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
