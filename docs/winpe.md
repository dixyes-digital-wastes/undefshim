# The Windows PE image

The checks that boot Windows use a WinPE image. This image has three additions
to the stock one:

- The virtio drivers. The guest needs them to see the disk and the console.
- The WMI component. What runs in the guest needs it to query the machine.
- A `startnet.cmd`. It runs the demos and prints their result.

The QEMU runs use this image. A machine of your own needs none of it. There the
firmware loads the driver from a volume it already boots.

`winpe-image.ps1` in this directory does every step below. Use it, or follow
the steps by hand to change one of them.

## Prerequisites

`copype` and `dism` come from the Windows ADK and its WinPE add-on. [Install
both](https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install)
on the machine that builds the image. Start an administrative prompt for the
build.

The commands below use the build 26100. Change the number for another build.
`winpe-image.ps1` reads the build from dism, so its paths follow the ADK that
is installed.

The optional components land in:

```
C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\
```

## Working files

Set the paths once. Every step below uses them.

```pwsh
$root  = 'C:\winpe\26100\arm64'
$media = "$root\media"
$mount = "$root\mount"
$wim   = "$media\sources\boot.wim"
```

Create the working files:

```pwsh
copype arm64 $root
```

`copype` sometimes fails to find `efisys_EX.bin`. `efisys.bin` beside it is the
same file. Copy it under the name `copype` wants:

```pwsh
$src = Get-ChildItem $media -Filter efisys.bin -Recurse | Select-Object -First 1
Copy-Item $src.FullName (Join-Path $src.DirectoryName 'efisys_EX.bin')
```

## Mount the image

```pwsh
dism /Mount-Image /ImageFile:$wim /Index:1 /MountDir:$mount
```

Every step below writes to `$mount`. The unmount at the end commits it.

## Add the optional components

Add each component to the mounted image:

```pwsh
$ocs = 'C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs'

dism /Add-Package /Image:$mount /PackagePath:"$ocs\WinPE-WMI.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\en-us\WinPE-WMI_en-us.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\WinPE-NetFX.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\en-us\WinPE-NetFX_en-us.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\WinPE-Scripting.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\en-us\WinPE-Scripting_en-us.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\WinPE-PowerShell.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\en-us\WinPE-PowerShell_en-us.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\WinPE-SecureStartup.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\en-us\WinPE-SecureStartup_en-us.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\WinPE-PlatformID.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\en-us\WinPE-PlatformID_en-us.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\WinPE-x64-Support.cab"
dism /Add-Package /Image:$mount /PackagePath:"$ocs\en-us\WinPE-x64-Support_en-us.cab"
```

**The order matters.** Several components have a prerequisite that must be in
the image first:

| Component | Needs first |
|---|---|
| `WinPE-NetFX` | `WinPE-WMI` |
| `WinPE-PowerShell` | `WinPE-WMI`, `WinPE-NetFX`, `WinPE-Scripting` |
| `WinPE-SecureStartup` | `WinPE-WMI` |
| `WinPE-PlatformID` | `WinPE-WMI`, `WinPE-SecureStartup` |

`WinPE-SecureStartup` brings the BitLocker and TPM tools with it. The image
needs it only because `WinPE-PlatformID` requires it.

Most components come in two parts: a language-neutral `.cab` in `WinPE_OCs\`,
and a language-specific one in a locale directory below it. Add both. A
component with only the first part does not work.

Which components exist, what each one needs first, and what each one does is in
[WinPE: Add packages (Optional Components Reference)](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-add-packages--optional-components-reference).

List what the image holds, to check:

```pwsh
dism /Get-Packages /Image:$mount
```

## Add the virtio drivers

Mount the driver ISO first. This example uses `E:`.

```pwsh
$driverRoot = 'E:\'

$dirs = Get-ChildItem $driverRoot -Directory | ForEach-Object {
    Join-Path $_.FullName 'w11\ARM64'
} | Where-Object { Test-Path $_ }

foreach ($dir in $dirs) {
    dism /Add-Driver /Image:$mount "/Driver:$dir" /Recurse
}
```

## `startnet.cmd`

The image runs `startnet.cmd` after it boots. Replace its content with:

```bat
@echo off
setlocal enabledelayedexpansion

set "target=entrypoint.cmd"

for %%d in (A B C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if exist %%d:\%target% (
    call %%d:\%target%
    echo %target%: !errorlevel!
    goto :end
    )
)

echo %target% not found

:end
pause
```

Write it to the mounted image:

```pwsh
$lines = @(
    '@echo off'
    'setlocal enabledelayedexpansion'
    ''
    'set "target=entrypoint.cmd"'
    ''
    'for %%d in (A B C D E F G H I J K L M N O P Q R S T U V W X Y Z) do ('
    '    if exist %%d:\%target% ('
    '    call %%d:\%target%'
    '    echo %target%: !errorlevel!'
    '    goto :end'
    '    )'
    ')'
    ''
    'echo %target% not found'
    ''
    ':end'
    'pause'
)
[IO.File]::WriteAllLines(
    "$mount\Windows\System32\startnet.cmd", $lines, [Text.ASCIIEncoding]::new())
```

The committed image holds the copy that matters. Read it back from there with
`wimlib-imagex`:

```bash
wimlib-imagex extract boot.wim 1 'Windows/System32/startnet.cmd' --dest-dir=/tmp/check
cat /tmp/check/startnet.cmd
```

## Unmount

```pwsh
dism /Unmount-Image /MountDir:$mount /Commit
```

## A bootable disk

What is left is a directory of files. The firmware wants a disk with an EFI
system partition on it.

`New-VHD` and `Mount-VHD` come from the Hyper-V module.

```pwsh
$vhdx = "$root\winpe.vhdx"

$vhd  = New-VHD -Path $vhdx -SizeBytes 10GB -Dynamic
$vhd  = Mount-VHD -Path $vhdx -PassThru
$disk = Initialize-Disk -Number $vhd.DiskNumber -PartitionStyle GPT -PassThru
$part = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter
$part | Format-Volume -FileSystem FAT32 -NewFileSystemLabel 'ESP' -Confirm:$false
$esp  = "$($part.DriveLetter):"

New-Item -ItemType Directory -Force -Path "$esp\EFI\Microsoft\Boot"
Copy-Item "$media\EFI\Microsoft\Boot\BCD" "$esp\EFI\Microsoft\Boot"
Copy-Item "$root\bootbins\bootmgfw.efi"   "$esp\EFI\Microsoft\Boot"

New-Item -ItemType Directory -Force -Path "$esp\EFI\Boot"
Copy-Item "$media\EFI\Boot\bootaa64.efi"  "$esp\EFI\Boot"

New-Item -ItemType Directory -Force -Path "$esp\sources"
Copy-Item "$media\sources\boot.wim"       "$esp\sources"

New-Item -ItemType Directory -Force -Path "$esp\boot"
Copy-Item "$media\boot\boot.sdi"          "$esp\boot"

Dismount-VHD -Path $vhdx
```

QEMU reads raw and qcow2 images, not VHDX. Convert it on the machine with
`qemu-img`:

```bash
qemu-img convert -f vhdx -O qcow2 winpe_26100.vhdx winpe_26100.qcow2
```

Point the runs at the result with `WIN_DISK=`.
