# Emulating the target

The original target is a HiSilicon Kunpeng 920, almost a Cortex-A76 with RCpc fused off. QEMU has no model for it, so the runs use the A76 with RCpc removed:

```
-cpu cortex-a76-nolrcpc
```

`docs/qemu-a76-nolrcpc.patch` adds that model. It is against [QEMU](https://gitlab.com/qemu-project/qemu) `v11.1.0-779-gd2e570cc0f`, and applies from the top of the tree:

```sh
git apply /path/to/docs/qemu-a76-nolrcpc.patch
```

Then build as usual. Only `qemu-system-aarch64` is needed: the runs use TCG, so no acceleration and no host of the same architecture.

## The firmware

Most distro-packaged OVMF/AAVMF builds do not come with EL2, and this project needs to validate its EL2 behavior, so we need a firmware with EL2. Building one with AT-F is a chain of its own, so the easiest way is to use Linaro's prebuilt one. It can be downloaded from:

<https://web.archive.org/web/20221013124832/https://snapshots.linaro.org/components/kernel/leg-virt-tianocore-edk2-upstream/latest/QEMU-KERNEL-AARCH64/RELEASE_GCC5/QEMU_EFI.fd>

It comes as `QEMU_FW`, defaulting to `../winemu/linaro_ovmf.fd`. That is outside this tree: it is a firmware, not something this project builds.

## What the patch changes

**The CPU model.** One field of `ID_AA64ISAR1` is cleared: the LRCPC field, which advertises the RCpc loads (`ldapr`, `ldaprh`, `ldaprb`) and stores. A guest that reads it -- Windows does -- is told the instructions are absent, and traps when it runs one. Everything else is the A76's, LSE included, which the target has.

The MIDR stays the A76's, so a guest that identifies the processor by it sees an A76 with a feature missing, not an unknown part.

**The model is registered as a valid CPU type** for `virt`, so the machine takes `-cpu cortex-a76-nolrcpc` instead of refusing the name.
