# The Windows PE image

The checks that boot Windows use a WinPE image with three things added to the stock one: the virtio drivers, so the guest can see the disk and the console it is given; the WMI component, so what runs in it can query the machine; and a `startnet.cmd` that runs the demos and echoes their result.

This is the image the QEMU runs use. A machine of your own needs none of it: there the driver is loaded from a volume the firmware already boots.

## Prerequisites

`copype`, `dism` and the optional components come from the Windows ADK and its WinPE add-on. [Install both](https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install), from an administrative prompt, on the machine that builds the image. The optional component packages land in:

```
C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\
```

Commands below assume we are working with 26100 PE addon.

Change "26100" with the build you are using.

## Working files

```cmd
REM In admin "Deployment and Imaging Tools Environment" prompt
copype arm64 C:\winpe\26100\arm64
```

`copype` occasionally fails to find `efisys_EX.bin`. Copying `efisys.bin` beside it under that name is the whole fix.

## Mount the image

```cmd
dism /mount-wim /wimfile:C:\winpe\26100\arm64\media\sources\boot.wim /mountdir:C:\winpe\26100\arm64\mount
```

Everything below is added to that mount point, and is committed back by the unmount at the end.

## Add the WMI component

```cmd
dism /add-package /image:C:\winpe\26100\arm64\mount ^
  /packagepath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\WinPE-NetFx.cab"
dism /add-package /image:C:\winpe\26100\arm64\mount ^
  /packagepath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\WinPE-PlatformId.cab"
dism /add-package /image:C:\winpe\26100\arm64\mount ^
  /packagepath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\WinPE-WMI.cab"
dism /add-package /image:C:\winpe\26100\arm64\mount ^
  /packagepath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\WinPE-PowerShell.cab"
dism /add-package /image:C:\winpe\26100\arm64\mount ^
  /packagepath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\WinPE-Scripting.cab"
dism /add-package /image:C:\winpe\26100\arm64\mount ^
  /packagepath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\WinPE-x64-Support.cab"
REM optional
dism /add-package /image:C:\winpe\26100\arm64\mount ^
  /packagepath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\WinPE-FontSupport-ZH-CN.cab"
```

Most components are split in two: a language-neutral `.cab` in `WinPE_OCs\`, and a language-specific one under a locale folder. Add the locale part as well, or the component is not usable:

```cmd
dism /add-package /image:C:\winpe\26100\arm64\mount ^
  /packagepath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\en-us\WinPE-NetFx_en-us.cab"
dism /add-package /image:C:\winpe\26100\arm64\mount ^
  /packagepath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\en-us\WinPE-PowerShell_en-us.cab"
dism /add-package /image:C:\winpe\26100\arm64\mount ^
  /packagepath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\en-us\WinPE-Scripting_en-us.cab"
dism /add-package /image:C:\winpe\26100\arm64\mount ^
  /packagepath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\en-us\WinPE-WMI_en-us.cab"
dism /add-package /image:C:\winpe\26100\arm64\mount ^
  /packagepath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs\en-us\WinPE-x64-Support_en-us.cab"
```

Which components there are, what each needs first, and what they are for is in [WinPE: Add packages (Optional Components Reference)](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-add-packages--optional-components-reference). Some have prerequisites that have to go in before them, in order; `WinPE-WMI` is itself a prerequisite of several.

List what ended up in the image to check:

```cmd
dism /get-packages /image:C:\winpe\26100\arm64\mount
```

## Add the virtio drivers

With the driver ISO mounted at `E:`:

```pwsh
$imagePath = "C:\winpe\26100\arm64\mount"
$drivers = "E:\"

foreach ($d in "Balloon","NetKVM","pvpanic","viofs","vioinput","viomem",
               "viorng","vioscsi","vioserial","viosock","viostor") {
    dism "/image:$imagePath" /add-driver "/driver:$drivers\$d\w11\ARM64" /recurse
}
```

## `startnet.cmd`

`startnet.cmd` is what the image runs once it has booted. Replace its content with

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

It lands in the committed image, which is the only copy that matters:

```bash
wimlib-imagex extract boot.wim 1 'Windows/System32/startnet.cmd' --dest-dir=/tmp/check
cat /tmp/check/startnet.cmd
```

## Unmount

```cmd
dism /unmount-wim /mountdir:C:\winpe\26100\arm64\mount /commit
```

## A bootable disk

What is left is a directory of files. The firmware wants a disk with an EFI system partition on it:

```pwsh
$winpeDir = "C:\winpe\26100\arm64"
$vhdName = "$winpeDir\winpe.vhdx"

$vhd = New-VHD -Path $vhdName -SizeBytes 10GB -Dynamic
$vhd = Mount-VHD -Path $vhdName -PassThru; $vhd
$disk = Initialize-Disk -Number $vhd.DiskNumber -PartitionStyle GPT -PassThru; $disk
$part = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter; $part
Format-Volume -Partition $part -FileSystem FAT32 -NewFileSystemLabel "ESP"

New-Item -Path "$($part.DriveLetter):\EFI\Microsoft\Boot" -ItemType Directory
Copy-Item -Path "$winpeDir\media\EFI\Microsoft\Boot\BCD" -Destination "$($part.DriveLetter):\EFI\Microsoft\Boot"
Copy-Item -Path "$winpeDir\bootbins\bootmgfw.efi" -Destination "$($part.DriveLetter):\EFI\Microsoft\Boot"
New-Item -Path "$($part.DriveLetter):\EFI\Boot" -ItemType Directory
Copy-Item -Path "$winpeDir\media\EFI\Boot\bootaa64.efi" -Destination "$($part.DriveLetter):\EFI\Boot"
New-Item -Path "$($part.DriveLetter):\sources" -ItemType Directory
Copy-Item -Path "$winpeDir\media\sources\boot.wim" -Destination "$($part.DriveLetter):\sources"
New-Item -Path "$($part.DriveLetter):\boot" -ItemType Directory
Copy-Item -Path "$winpeDir\media\boot\boot.sdi" -Destination "$($part.DriveLetter):\boot"

Dismount-VHD -Path $vhdName
```

QEMU runs raw and qcow2 images, not VHDX, so convert it:

```bash
qemu-img convert -f vhdx -O qcow2 winpe_26100.vhdx winpe_26100.qcow2
```

Point the runs at it with `WIN_DISK=`.
