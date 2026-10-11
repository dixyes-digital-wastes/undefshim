# Checks that boot the image

`make check` covers the logic that runs on the host. Nothing here does: each
script builds a volume, starts QEMU with it, reads what came out, and says
whether that is what was expected. They are the only evidence that anything
actually works, which is why most of them are slow.

    make check-qemu                 every check that does not need Windows
    make check-one CHECK=driver_boot   one of them

Two external inputs decide what a run can do, both of them overridable:

| Variable | What it is | Without it |
|---|---|---|
| `WIN_DISK` | a Windows image to boot, the same build as `PLAN_CORPUS` | the checks that need Windows report `[SKIP]` |
| `PLAN_CORPUS` | a directory of images to compare the plan against, `../windowsthings/26100pe` | `plan_case` and the two tools in `tests/tools` report `[SKIP]` |

`../undefshim-ex2-docs/design.md` §9 says which environment proves what. The
short version: this directory is the QEMU side, `scripts/` is what a person
runs against a machine when the question is not one a check can ask, and
`tests/unit/` is what needs no machine at all.

## The checks

| Check | What it establishes |
|---|---|
| `driver_boot` | The driver comes up on its own, reaches `M4 setup`, and reports |
| `config_cases` | A configuration is found, read and applied; a broken one is refused; a range error is caught; no file at all uses the defaults |
| `payload_case` | The blob is placed in memory the OS keeps, runs there, and says so |
| `boot_hook` | `LoadImage` is hooked and the images of a real boot are recognized |
| `patch_case` | The debug patch table writes bytes into a real image |
| `plan_case` | The plan the driver builds matches the plan the host builds from the same images |
| `breakpoint_case` | The injected patch is where a debugger can break on it |
| `vector_case` | A synchronous exception reaches the payload and the kernel carries on |
| `fake_case` | The whole exception path, end to end, against a stand-in kernel and without Windows |
| `screen_case` | What the driver draws on the screen |

## How a check reports

`check.sh` is sourced rather than run, and it is what makes the output of
several scripts read as one report:

    [PASS] driver_boot: the driver brings itself up
    [FAIL] payload_case: the placement report is not parseable
    [SKIP] boot_hook: WIN_DISK is not set

A check sets `CHECK` to its own name before sourcing it, so a script it calls
reports under the check's name rather than its own. `77` is the exit status for
"nothing was asked of me", which is not a failure, and `make check-qemu` treats
it as a pass.

## What is here besides the checks

| File | What it is |
|---|---|
| `build_esp.sh` | Builds the bootable FAT volume every check boots: the UEFI shell, the driver, and a configuration |
| `run.sh`, `plain.sh` | Run the image under QEMU and print its log as text |
| `check.sh` | The reporter above |
| `logtext.py`, `pool_dump.py` | Readers: the log as text with the escapes taken out, and the pool by QMP |
| `screen.py` | Reads the screen over QMP and says what is on it |
| `screen_case.sh`, `serial_monitor.py` | The QMP side of the screen and serial checks |
| `demo_disk.sh` | The volume the demo programs run from, for a WinPE boot |
| `run24h2.sh`, `windows_case.sh` | The full Windows side, which needs `WIN_DISK` |
| `control.nsh`, `startup.nsh`, `entrypoint.cmd`, `fake_startup.nsh`, `hooktest.nsh` | The shell scripts that run inside the guest |
| `venv/` | The interpreter `screen.py`'s recogniser needs. Not in the repository, and the checks that only look at colours work without it |

The tools under `scripts/` put this directory on their import path to reach the
readers, which is why the readers live here rather than with them.
