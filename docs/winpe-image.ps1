#Requires -RunAsAdministrator
<#
.SYNOPSIS
Build the WinPE image the QEMU runs boot, and turn it into a disk.

.DESCRIPTION
Run this on the Windows machine that has the ADK and the WinPE add-on. The
script does every step in docs/winpe.md:

  1. Create the working files with copype.
  2. Add the optional components the image needs.
  3. Add the virtio drivers.
  4. Replace startnet.cmd.
  5. Commit the image and build a VHDX.

The image is left mounted if a step fails, so the script unmounts it on the
way out. The qcow2 conversion runs on the machine with qemu-img, which is
usually not this one. The script prints the command to run there.

.EXAMPLE
.\winpe-image.ps1 -DriverRoot E:\

.EXAMPLE
.\winpe-image.ps1 -Build 28000 -DriverRoot E:\ -NoDisk

.NOTES
Run this in the Deployment and Imaging Tools Environment, or add its
directory to PATH. The script also looks for copype and dism under the ADK
when PATH does not have them.
#>
[CmdletBinding()]
param(
    # The WinPE build. It is the ADK build, and the name of the directory.
    [string] $Build = '26100',

    [ValidateSet('arm64', 'amd64')]
    [string] $Arch = 'arm64',

    # Where the working files go. copype creates this directory.
    [string] $WinPeRoot,

    # The mounted image. It is created and removed by this script.
    [string] $MountDir,

    # The root of the mounted virtio-win ISO. Drivers are found under it.
    # Skip the step when this is empty.
    [string] $DriverRoot,

    # The locale of the language-specific packages.
    [string] $Locale = 'en-us',

    # Where the disk image goes.
    [string] $Vhdx,

    # Stop after the image is committed. Use this when the disk is built
    # somewhere else.
    [switch] $NoDisk,

    # Add WinPE-SecureStartup. Microsoft lists it as a prerequisite of
    # WinPE-PlatformID, and it brings the BitLocker and TPM tools.
    [switch] $WithSecureStartup
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $WinPeRoot) { $WinPeRoot = "C:\winpe\$Build\$Arch" }
if (-not $MountDir)  { $MountDir  = Join-Path $WinPeRoot 'mount' }
if (-not $Vhdx)      { $Vhdx      = Join-Path $WinPeRoot "winpe_$Build.vhdx" }

$mediaDir = Join-Path $WinPeRoot 'media'
$wim      = Join-Path $mediaDir 'sources\boot.wim'

# The optional components, in the order they have to be added. Microsoft
# lists a prerequisite for several of them, and an image that gets one out
# of order does not come out usable. A language-specific package follows
# its own component, because a component with only the first part does not
# work.
$components = @(
    'WinPE-WMI'         # nothing comes before it
    'WinPE-NetFX'       # after WinPE-WMI
    'WinPE-Scripting'   # nothing comes before it
    'WinPE-PowerShell'  # after WinPE-WMI, WinPE-NetFX and WinPE-Scripting
    'WinPE-PlatformID'  # after WinPE-WMI and WinPE-SecureStartup
    'WinPE-x64-Support' # nothing comes before it
)

function Write-Step {
    param([string] $Text)
    Write-Host ''
    Write-Host "==> $Text" -ForegroundColor Cyan
}

function Invoke-Tool {
    <#
    Run an external tool and stop on a nonzero exit. PowerShell does not
    treat an exit code as an error, so a step that failed would otherwise
    look like one that worked.
    #>
    param(
        [string] $Path,
        [string[]] $Arguments
    )
    Write-Verbose "$Path $($Arguments -join ' ')"
    & $Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Path failed with exit code $LASTEXITCODE"
    }
}

function Invoke-Dism {
    param([string[]] $Arguments)
    Invoke-Tool -Path $script:Dism -Arguments $Arguments
}

function Find-Tool {
    <#
    Find a tool on PATH, then under the ADK. copype and dism are only on
    PATH inside the Deployment and Imaging Tools Environment, and a plain
    administrator prompt is the more likely way to start this.
    #>
    param(
        [string] $Name,
        [string] $AdkDir
    )
    $found = Get-Command $Name -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }

    if ($AdkDir -and (Test-Path $AdkDir)) {
        $hit = Get-ChildItem -Path $AdkDir -Filter "$Name.*" -Recurse `
                             -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$adkRoot   = Join-Path ${env:ProgramFiles(x86)} `
                       'Windows Kits\10\Assessment and Deployment Kit'
$toolsDir  = Join-Path $adkRoot 'Deployment Tools'
$ocsDir    = Join-Path (Join-Path $adkRoot 'Windows Preinstallation Environment') `
                       "$Arch\WinPE_OCs"

$script:Dism = Find-Tool -Name 'dism' -AdkDir $toolsDir
if (-not $script:Dism) {
    throw 'No dism. Install the Windows ADK, or run this in the ' +
          'Deployment and Imaging Tools Environment.'
}
$copype = Find-Tool -Name 'copype' -AdkDir $toolsDir
if (-not $copype) {
    throw 'No copype. Install the WinPE add-on, or run this in the ' +
          'Deployment and Imaging Tools Environment.'
}
if (-not (Test-Path $ocsDir)) {
    throw "No optional components at $ocsDir. Install the WinPE add-on."
}

$mounted  = $false
$vhdOn    = $false

try {
    # -- 1. the working files --------------------------------------------

    Write-Step "Create the working files at $WinPeRoot"
    if (Test-Path $mediaDir) {
        Write-Host "They are already there. Removing $WinPeRoot first."
        Remove-Item -Recurse -Force $WinPeRoot
    }
    Invoke-Tool -Path $copype -Arguments @($Arch, $WinPeRoot)

    # copype looks for efisys_EX.bin and does not always find it. The file
    # beside it is the same one, so a copy under the name copype wants is
    # the whole fix.
    $wanted = Get-ChildItem -Path $mediaDir -Filter 'efisys_EX.bin' -Recurse `
                            -ErrorAction SilentlyContinue
    if (-not $wanted) {
        $source = Get-ChildItem -Path $mediaDir -Filter 'efisys.bin' -Recurse `
                                -ErrorAction SilentlyContinue |
                  Select-Object -First 1
        if ($source) {
            Write-Host "Copying $($source.FullName) to efisys_EX.bin"
            Copy-Item $source.FullName `
                      (Join-Path $source.DirectoryName 'efisys_EX.bin')
        }
    }

    # -- 2. the optional components --------------------------------------

    Write-Step 'Mount the image'
    New-Item -ItemType Directory -Force -Path $MountDir | Out-Null
    Invoke-Dism @('/Mount-Image', "/ImageFile:$wim", '/Index:1',
                  "/MountDir:$MountDir")
    $mounted = $true

    Write-Step 'Add the optional components'
    $queue = [System.Collections.Generic.List[string]]::new()
    foreach ($name in $components) {
        # WinPE-PlatformID is the one component that needs SecureStartup,
        # and a component has to be in the image before the one that needs
        # it.
        if ($WithSecureStartup -and $name -eq 'WinPE-PlatformID') {
            $queue.Add('WinPE-SecureStartup')
        }
        $queue.Add($name)
    }
    foreach ($name in $queue) {
        $neutral = Join-Path $ocsDir "$name.cab"
        if (-not (Test-Path $neutral)) {
            throw "$name.cab is not in $ocsDir"
        }
        Write-Host "  $name"
        Invoke-Dism @('/Add-Package', "/Image:$MountDir",
                      "/PackagePath:$neutral")

        # Most components are split in two. The second part is the same name
        # with the locale on it, and without it the component does not work.
        $localeCab = Join-Path $ocsDir "$Locale\$($name)_$Locale.cab"
        if (Test-Path $localeCab) {
            Invoke-Dism @('/Add-Package', "/Image:$MountDir",
                          "/PackagePath:$localeCab")
        }
    }
    Write-Host ''
    Invoke-Dism @('/Get-Packages', "/Image:$MountDir")

    # -- 3. the drivers --------------------------------------------------

    if ($DriverRoot) {
        if (-not (Test-Path $DriverRoot)) {
            throw "No driver media at $DriverRoot"
        }
        Write-Step "Add the drivers from $DriverRoot"
        # The layout says which drivers are the Windows 11 arm64 ones, so
        # the directories are found rather than named. A driver media that
        # is laid out differently is one this cannot read, and saying so is
        # better than adding the wrong package.
        $driverDirs = Get-ChildItem -Path $DriverRoot -Directory `
                                    -ErrorAction SilentlyContinue |
                      ForEach-Object {
                          Join-Path $_.FullName "w11\$Arch"
                      } |
                      Where-Object { Test-Path $_ }
        if (-not $driverDirs) {
            throw "No *\w11\$Arch driver directories under $DriverRoot"
        }
        foreach ($dir in $driverDirs) {
            $drvName = Split-Path (Split-Path $dir -Parent) -Parent
            Write-Host "  $(Split-Path $drvName -Leaf)"
            Invoke-Dism @('/Add-Driver', "/Image:$MountDir",
                          "/Driver:$dir", '/Recurse')
        }
    } else {
        Write-Step 'No -DriverRoot, so the drivers are not added'
    }

    # -- 4. startnet.cmd -------------------------------------------------

    Write-Step 'Replace startnet.cmd'
    # cmd.exe wants CRLF, and WriteAllLines uses the platform separator.
    $startNet = @(
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
    $startNetPath = Join-Path $MountDir 'Windows\System32\startnet.cmd'
    [IO.File]::WriteAllLines($startNetPath, $startNet, [Text.ASCIIEncoding]::new())
    Write-Host "Wrote $startNetPath"

    # -- 5. commit -------------------------------------------------------

    Write-Step 'Unmount and commit'
    Invoke-Dism @('/Unmount-Image', "/MountDir:$MountDir", '/Commit')
    $mounted = $false

    # -- 6. a disk -------------------------------------------------------

    if ($NoDisk) {
        Write-Step '-NoDisk, so no disk is built'
        Write-Host "The files are at $mediaDir"
        return
    }

    Write-Step "Build a disk at $Vhdx"
    if (-not (Get-Command New-VHD -ErrorAction SilentlyContinue)) {
        throw 'No New-VHD. Install the Hyper-V module, or pass -NoDisk ' +
              'and build the disk by hand.'
    }
    if (Test-Path $Vhdx) { Remove-Item -Force $Vhdx }

    $vhd  = New-VHD -Path $Vhdx -SizeBytes 10GB -Dynamic
    $vhd  = Mount-VHD -Path $Vhdx -PassThru
    $vhdOn = $true
    $disk = Initialize-Disk -Number $vhd.DiskNumber -PartitionStyle GPT -PassThru
    $part = New-Partition -DiskNumber $disk.Number -UseMaximumSize `
                          -AssignDriveLetter
    $part | Format-Volume -FileSystem FAT32 -NewFileSystemLabel 'ESP' `
                          -Confirm:$false
    $esp = "$($part.DriveLetter):"

    # The firmware wants an EFI system partition, and the boot manager reads
    # its own database and the image from it.
    $layout = @(
        @{ From = "$mediaDir\EFI\Microsoft\Boot\BCD";  To = "$esp\EFI\Microsoft\Boot" }
        @{ From = "$WinPeRoot\bootbins\bootmgfw.efi";  To = "$esp\EFI\Microsoft\Boot" }
        @{ From = "$mediaDir\EFI\Boot\bootaa64.efi";   To = "$esp\EFI\Boot" }
        @{ From = "$mediaDir\sources\boot.wim";        To = "$esp\sources" }
        @{ From = "$mediaDir\boot\boot.sdi";           To = "$esp\boot" }
    )
    foreach ($item in $layout) {
        New-Item -ItemType Directory -Force -Path $item.To | Out-Null
        Copy-Item -Path $item.From -Destination $item.To
    }

    Dismount-VHD -Path $Vhdx
    $vhdOn = $false

    Write-Step 'Done'
    Write-Host "The disk is $Vhdx"
    Write-Host 'QEMU does not read VHDX. Convert it on the machine with qemu-img:'
    Write-Host ''
    Write-Host "    qemu-img convert -f vhdx -O qcow2 winpe_$Build.vhdx winpe_$Build.qcow2"
    Write-Host ''
    Write-Host 'Then point the runs at it with WIN_DISK=.'
}
finally {
    # A step that failed leaves the image mounted, and a mounted image
    # cannot be mounted again. The discard keeps the mount point usable.
    if ($mounted) {
        Write-Warning 'A step failed. Discarding the mounted image.'
        & $script:Dism /Unmount-Image "/MountDir:$MountDir" /Discard |
            Out-Host
    }
    # An attached VHD holds a drive letter and the file, so it goes back
    # even when the copy into it failed.
    if ($vhdOn) {
        Write-Warning 'A step failed. Detaching the disk.'
        Dismount-VHD -Path $Vhdx -ErrorAction SilentlyContinue
    }
}
