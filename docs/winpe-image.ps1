#Requires -RunAsAdministrator
<#
Build the WinPE image the QEMU runs boot, and turn it into a disk.

Run this in the Deployment and Imaging Tools Environment, so that copype and
dism are on PATH.

    .\winpe-image.ps1 -Build 26100 -DriverRoot E:\
    .\winpe-image.ps1 -Build 26100 -DriverRoot E:\ -NoDisk
#>
param(
    # The ADK build. It names the working directory and the disk, and it has
    # to be the build of the ADK that is installed.
    [Parameter(Mandatory)]
    [string] $Build,

    # The root of the mounted virtio-win ISO. The driver step is skipped
    # when this is empty.
    [string] $DriverRoot,

    # Stop after the image is committed, for a disk that is built elsewhere.
    [switch] $NoDisk,

    # Add WinPE-PlatformID, which is what a PowerShell script reads the
    # machine's platform identifier with. The image does not need it. It
    # brings WinPE-SecureStartup, and with it the BitLocker and TPM tools.
    [switch] $WithPlatformId
)

$ErrorActionPreference = 'Stop'

$root  = "C:\winpe\$Build\arm64"
$media = "$root\media"
$mount = "$root\mount"
$wim   = "$media\sources\boot.wim"
$vhdx  = "$root\winpe_$Build.vhdx"
$ocs   = "${env:ProgramFiles(x86)}\Windows Kits\10\Assessment and " +
         "Deployment Kit\Windows Preinstallation Environment\arm64\WinPE_OCs"

# dism reports a failure through its exit code, and PowerShell does not look
# at that. Every call goes through here so that one cannot pass unnoticed.
function Invoke-Dism {
    dism @args
    if ($LASTEXITCODE -ne 0) {
        throw "dism $($args -join ' ') failed with $LASTEXITCODE"
    }
}

# copype looks for efisys_EX.bin and does not always find it. efisys.bin
# beside it is the same file.
function Fix-Efisys {
    if (Get-ChildItem $media -Filter efisys_EX.bin -Recurse) { return }
    $src = Get-ChildItem $media -Filter efisys.bin -Recurse |
           Select-Object -First 1
    if ($src) { Copy-Item $src.FullName "$($src.DirectoryName)\efisys_EX.bin" }
}

function Write-StartNet {
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
    # WriteAllLines uses the environment's line ending, and cmd.exe reads CRLF.
    [IO.File]::WriteAllLines("$mount\Windows\System32\startnet.cmd", $lines,
                             [Text.ASCIIEncoding]::new())
}

function New-BootDisk {
    $vhd  = New-VHD -Path $vhdx -SizeBytes 10GB -Dynamic
    $vhd  = Mount-VHD -Path $vhd -PassThru
    $disk = Initialize-Disk -Number $vhd.DiskNumber -PartitionStyle GPT -PassThru
    $part = New-Partition -DiskNumber $disk.Number -UseMaximumSize `
                          -AssignDriveLetter
    $part | Format-Volume -FileSystem FAT32 -NewFileSystemLabel 'ESP' `
                          -Confirm:$false
    $esp = "$($part.DriveLetter):"

    $items = @(
        @{ From = "$media\EFI\Microsoft\Boot\BCD"; To = "$esp\EFI\Microsoft\Boot" }
        @{ From = "$root\bootbins\bootmgfw.efi";   To = "$esp\EFI\Microsoft\Boot" }
        @{ From = "$media\EFI\Boot\bootaa64.efi";  To = "$esp\EFI\Boot" }
        @{ From = "$media\sources\boot.wim";       To = "$esp\sources" }
        @{ From = "$media\boot\boot.sdi";          To = "$esp\boot" }
    )
    foreach ($item in $items) {
        New-Item -ItemType Directory -Force $item.To | Out-Null
        Copy-Item $item.From $item.To
    }
    Dismount-VHD -Path $vhdx
}

# The optional components in the order they go in. Several need one of the
# others first, and a component that comes after its own prerequisite is a
# component that works.
$packages = @(
    'WinPE-WMI'
    'WinPE-NetFX'         # needs WinPE-WMI
    'WinPE-Scripting'
    'WinPE-PowerShell'    # needs WinPE-WMI, WinPE-NetFX, WinPE-Scripting
    'WinPE-x64-Support'
)

# WinPE-PlatformID is what a PowerShell script reads the machine's platform
# identifier with, and nothing in this image does that. It is the only
# reason WinPE-SecureStartup is here, and SecureStartup brings the BitLocker
# and TPM tools with it. Both go in only when they are asked for, in the
# order the prerequisite requires.
if ($WithPlatformId) {
    $packages += 'WinPE-SecureStartup' # needs WinPE-WMI
    $packages += 'WinPE-PlatformID'    # needs WinPE-WMI, WinPE-SecureStartup
}

if (Test-Path $media) { Remove-Item -Recurse -Force $root }
copype arm64 $root
Fix-Efisys

New-Item -ItemType Directory -Force $mount | Out-Null
Invoke-Dism /Mount-Image /ImageFile:$wim /Index:1 /MountDir:$mount

$mounted = $true
try {
    foreach ($name in $packages) {
        $neutral = "$ocs\$name.cab"
        if (-not (Test-Path $neutral)) { throw "$neutral is not there" }

        # Most components are two packages, and the second one carries the
        # language. Not every component has one: WinPE-PlatformID has none,
        # and the part that is there is a component that works.
        $cabs = @($neutral, "$ocs\en-us\${name}_en-us.cab") |
                Where-Object { Test-Path $_ }

        foreach ($cab in $cabs) {
            Invoke-Dism /Add-Package /Image:$mount /PackagePath:$cab
        }
    }

    if ($DriverRoot) {
        $dirs = Get-ChildItem $DriverRoot -Directory |
                ForEach-Object { Join-Path $_.FullName 'w11\ARM64' } |
                Where-Object { Test-Path $_ }
        if (-not $dirs) { throw "no *\w11\ARM64 under $DriverRoot" }
        foreach ($dir in $dirs) {
            Invoke-Dism /Add-Driver /Image:$mount /Driver:$dir /Recurse
        }
    }

    Write-StartNet
    Invoke-Dism /Unmount-Image /MountDir:$mount /Commit
    $mounted = $false
}
finally {
    # A failed step leaves the image mounted, and a mounted image cannot be
    # mounted again.
    if ($mounted) {
        Write-Warning 'a step failed, discarding the image'
        dism /Unmount-Image /MountDir:$mount /Discard
    }
}

if ($NoDisk) {
    Write-Host "the files are at $media"
    return
}

New-BootDisk

Write-Host "the disk is $vhdx"
Write-Host "QEMU does not read VHDX, so convert it where qemu-img is:"
Write-Host "    qemu-img convert -f vhdx -O qcow2 winpe_$Build.vhdx winpe_$Build.qcow2"
