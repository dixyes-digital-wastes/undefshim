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
| `driverInplaceRewrite` | `true` | Replace them in the drivers the kernel loads once it is running, at the moment each one is loaded. Off means every driver RCpc load keeps taking the exception, which is slower and always correct. |

`imageInplaceRewrite` covers the images the boot loads. It does not cover anything loaded later, which is what `driverInplaceRewrite`, `el0InplaceRewrite` and the trap path are for. None of them can make the machine wrong by being off: what an unreplaced load costs is an exception, and the exception path is there to answer it.

`driverInplaceRewrite` takes over the call the kernel makes inside its own driver loader, so the replacement happens between an image being mapped and any of its code running. That is the same moment the kernel's own text was rewritten in, and it is the last one available: a driver does not exist at the boot, and by the time it does run the kernel's integrity check is watching its text.

Neither of the replacement keys can make the machine wrong by being off. The kernel's own text is never written at run time whatever they say: a modified function or `.pdata` is what its integrity check reports, which is why the replacement is made during the window the kernel's own loader holds an image in rather than while it runs.

## `[stats]`

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | Keep a table of the addresses traps were taken at, in the pool. It costs a lookup per trap. |

## `[kernel]`

| Key | Default | Meaning |
|---|---|---|
| `descriptorBaseRVA` | none | The RVA of the variable holding the kernel's page table base. It is read out of the kernel's own image, so this is needed only for a build whose shape the locator does not recognise |

This is per build. A value from another build is checked against the hardware before it is used and refused when it does not fit, so a wrong one costs a rewrite rather than the machine.

## `[patch]`

| Key | Default | Meaning |
|---|---|---|
| `dir` | `"usPatch"` | Directory of patch list files, relative to the volume `us.toml` was read from. An empty string turns the feature off. |

The driver reads every file in that directory as a list, and applies it to the kernel image. A list names the build it applies to. The driver refuses a list that names another build, writes nothing, and says so in the serial log. So one directory can hold lists for several builds, and each boot applies the list for the build it runs. The driver skips a subdirectory.

The driver refuses the volume root as a directory. It also refuses a file larger than 512 kB as a list.

### The list format

```text
USPATCHV1
peFile ntoskrnl                                   # optional
textSHA256Hash 3f2a...c9                          # optional
pdbUUID 1713AA50-9114-C4C6-99D3-46F086029846-1    # optional
0x711e64 2a0003f3d63f01e0 2a0003f352800000        # address, bytes there, bytes to write
```

`USPATCHV1` must be the first token. A `#` starts a comment that runs to the end of the line. Whitespace and comments may stand between any two tokens. So the parser reads the grammar, and not the shape of the lines.

Each of the three matchers is optional, and each may appear once. Every matcher in a list must agree with the image before the driver writes one site. The driver refuses the whole list when it carries no matcher, or two copies of one.

| Matcher | What the driver compares |
|---|---|
| `peFile` | The image's name, without case and without the extension, so `ntoskrnl` and `NTOSKRNL.EXE` are the same image |
| `textSHA256Hash` | The SHA-256 of the image's `.text`, with the bytes a relocation writes left out. The loader writes those, so they differ from boot to boot. 64 hexadecimal digits |
| `pdbUUID` | The program database's GUID and age, as the image's own CodeView record states them. The age follows the last field, after a hyphen |

`pdbUUID` takes the GUID in either of two orders, and both name the same build. Debuggers write `1713AA50-9114-C4C6-99D3-46F086029846`, and so do `llvm-pdbutil` and the symbol server. The image's own record holds the same sixteen bytes in the other order, `50aa1317-1491-c6c4-99d3-46f086029846`. The list must state the age in that record. That age is not always the age of the file the symbol server serves.

A site is an RVA, then two or three runs of hexadecimal digits. The runs are the bytes that have to be there, the bytes to write, and an optional mask. A field is one to sixteen bytes, written in pairs of digits, upper or lower case.

The digits of a field are a value, so the bytes land in reverse. For example, `2a0003f3d63f01e0` at `0x711e64` means the eight bytes `e0 01 3f d6 f3 03 00 2a` there. Those bytes are `blr x15`, and then `mov w19, w0`.

A mask bit of zero means "do not care about this bit". With three fields the branch is this. The bytes at the address, masked, have to equal the first field, masked. Then the driver writes the second field. The driver leaves out a line whose fields cannot describe a write, and counts it. That is not a reason to refuse the file.

The driver writes nothing for a site whose bytes differ from the list. That is what makes a stale address harmless. It is also why a list can name a site in a section other than `.text`, without the driver having to know which section.

After each list the log carries one line, and one more line for each kind of site it could not use:

```text
patch: no-ci-break-24h2.txt applied 2 refused 0 out of range 0
patch:   first of them 0x8f25a0, the bytes there are not the ones the list names
```

When the driver refuses a list, it says why, and names the build it found:

```text
patch: no-ci-break-24h2.txt is for another build: pdb
patch:   the list asks for 1713aa50-9114-c4c6-99d3-46f086029846-1
patch:   this image is b5833c9b-5780-715e-9d55-92619e29c053-1, which a debugger writes 9B3C83B5-8057-5E71-9D55-92619E29C053-1
```

`scripts/enumerate_sites.py` writes a list of the RCpc loads in an image. Two lists ship with the project. `config/usPatch/no-ci-break-<build>.txt` quiets both code integrity front ends. `config/usPatch-modes/no-ci-page-<build>.txt` quiets only the per-page one. Put at most one list for a given site in the directory. A second list that names the same site sees the bytes the first one wrote, and leaves the site alone.

## `[debug]`

Switches that take a piece of the mechanism out of the way, for bringing a machine up. Every one is `false` unless it is written, and one named `notX` means the driver does X unless the switch is set: the whole of the mechanism is what a file that does not mention this table gets.

A key that has to be turned on for the machine to work does not belong here. That is what this table used to be, and it meant a shim that was loaded but not armed could look exactly like a machine that did not need one.

| Key | Default | Meaning |
|---|---|---|
| `notArmVectors` | `false` | Take the exception vectors over. Off leaves the loads the rewrite did not reach to the kernel, which cannot carry them out. |
| `notArmVectorsEl1t` | `false` | Also take the EL1t synchronous slot. Needed because Windows takes synchronous exceptions with SP_EL0 selected, which arrives at a different offset of the table; off is known to bugcheck very early. |
| `notArmHandover` | `false` | Take over the branch winload jumps into the kernel with. That branch is what tells the other stubs where the payload ended up. |
| `notVamap` | `false` | Register the notification that runs during `SetVirtualAddressMap`, which is when the payload learns the addresses it will have once the kernel's page tables are in force. The handover stub publishes them earlier; this is for a machine where that turns out not to be enough. |
| `el0RewriteReadOnlyPages` | `false` | Let the replacement of an EL0 load edit the descriptor of a read-only page to get into it. Such a page is an image's, and the edit reaches a page the integrity check hashes. Leave it off to boot a signed Windows: the site then keeps its RCpc load and the exception path carries the load out. |
| `spxStack` | `false` | Let the stub push on the SPx vector as well, which is how to find out which vector Windows arrives on. |
| `armOnLoad` | `false` | Arm as soon as a kernel is loaded, rather than when the plan that says which image the kernel is has been built. The one key here that turns something on. The stand-in kernel the fast test runs produces no plan, so the test needs this to exercise the exception path at all. Leave it off: arming on a name alone takes over the vectors of whatever image happens to be called that. |
| `panicAfter` | absent | Stop after the named milestone, printing a panic first. The name is compared whole against what the boot prints, so `M6.5 armed` and `M6.5 arm failed` are different keys. |
| `panicAfterTraps` | `0` | Stop on the exception with this number. Reached only once the kernel is running. |
| `panicAfterRewrites` | `0` | Stop on the rewrite attempt with this number, whether it succeeded or not. |

The panic keys are there because a stop only happens on a path that is already going wrong, so the code that reports one is never reached in ordinary use and a broken report would first be found at the worst possible moment. A stop taken this way prints to the serial port, to the screen, and into the pool, and then halts with every interrupt masked: the machine is meant to stay still, and a still screen with the message on it is the answer that was asked for.

Each key is read from its own section. A key under the wrong one is not read at all, which is indistinguishable from not being there, so a switch that appears to do nothing is usually in the wrong table.

The driver prints every key it read at `verbose`, including the ones the file did not mention, so the configuration a machine is running with can be read back rather than worked out.
