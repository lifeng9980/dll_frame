[CmdletBinding()]
param(
    [string]$EmitSerialMap = "",
    [switch]$Elevate,
    [int]$ElevationTimeoutSec = 35
)

$ErrorActionPreference = "Stop"

$DesiredPairs = @(
    [ordered]@{ role = "power";     client = "COM10"; sim = "COM11"; baud_rate = 115200; stop_bits = 1 },
    [ordered]@{ role = "oven";      client = "COM12"; sim = "COM13"; baud_rate = 9600;   stop_bits = 1 },
    [ordered]@{ role = "dmm";       client = "COM14"; sim = "COM15"; baud_rate = 115200; stop_bits = 1 },
    [ordered]@{ role = "detector";  client = "COM16"; sim = "COM17"; baud_rate = 115200; stop_bits = 1 },
    [ordered]@{ role = "cooler";    client = "COM18"; sim = "COM19"; baud_rate = 9600;   stop_bits = 2 },
    [ordered]@{ role = "blackbody"; client = "COM20"; sim = "COM21"; baud_rate = 9600;   stop_bits = 1 }
)

$resolvedSerialMap = $null
if ($EmitSerialMap) {
    $resolvedSerialMap = [System.IO.Path]::GetFullPath($EmitSerialMap)
    if (Test-Path -LiteralPath $resolvedSerialMap) {
        throw "Serial map already exists and will not be overwritten: $resolvedSerialMap"
    }
    $parentDir = Split-Path -Parent $resolvedSerialMap
    if ($parentDir -and -not (Test-Path -LiteralPath $parentDir -PathType Container)) {
        throw "Serial map parent directory does not exist: $parentDir"
    }
}

function Find-SetupC {
    foreach ($candidate in @(
        "C:\Program Files (x86)\com0com\setupc.exe",
        "C:\Program Files\com0com\setupc.exe"
    )) {
        if (Test-Path $candidate) { return $candidate }
    }
    throw "com0com setupc.exe not found under Program Files."
}

$setupc = Find-SetupC
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    if ($Elevate) {
        Write-Output "Requesting Windows UAC elevation to configure com0com virtual COM pairs..."
        $childArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
        if ($resolvedSerialMap) {
            $childArgs += @("-EmitSerialMap", "`"$resolvedSerialMap`"")
        }
        $proc = Start-Process -FilePath "powershell.exe" -ArgumentList $childArgs -Verb RunAs -PassThru
        if (-not $proc.WaitForExit($ElevationTimeoutSec * 1000)) {
            throw "Timed out after $ElevationTimeoutSec seconds waiting for elevated setup_com0com_pairs.ps1."
        }
        if ($proc.ExitCode -ne 0) {
            throw "Elevated com0com setup failed with exit code $($proc.ExitCode)."
        }
        Get-ItemProperty "HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM" | Format-List
        exit 0
    }
    throw "Administrator privileges are required to run setupc.exe. Re-run from an elevated PowerShell or pass -Elevate."
}

function Invoke-SetupC {
    param([string[]]$SetupArgs)
    $output = & $setupc @SetupArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "setupc $($SetupArgs -join ' ') failed (exit=$LASTEXITCODE):`n$($output -join "`n")"
    }
    return $output
}

function Get-Com0ComPorts {
    param([string[]]$Lines)
    $ports = [ordered]@{}
    foreach ($line in $Lines) {
        if ($line -match '^\s*(CNC[AB]\d+)\s+(.*)$') {
            $devId = $matches[1]
            $attrs = $matches[2].Trim()
            $portName = "-"
            if ($attrs -match 'PortName=([^\s,]+)') {
                $portName = $matches[1]
            }
            $ports[$devId] = [ordered]@{
                id        = $devId
                port_name = $portName
                raw       = $attrs
            }
        }
    }
    return $ports
}

function Get-NonCom0ComSerialPorts {
    $reg = Get-ItemProperty "HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM" -ErrorAction SilentlyContinue
    $used = @{}
    if ($null -eq $reg) { return $used }
    foreach ($prop in $reg.PSObject.Properties) {
        if ($prop.Name -like "PS*") { continue }
        if ($prop.Name -like "\Device\com0com*") { continue }
        $used[$prop.Value.ToString().ToUpperInvariant()] = $prop.Name
    }
    return $used
}

$setupDir = Split-Path -Parent $setupc
Push-Location $setupDir
try {
    $externalPorts = Get-NonCom0ComSerialPorts
    foreach ($pair in $DesiredPairs) {
        foreach ($name in @($pair.client, $pair.sim)) {
            if ($externalPorts.ContainsKey($name)) {
                throw "Requested virtual port $name conflicts with existing device $($externalPorts[$name])."
            }
        }
    }

    $currentLines = Invoke-SetupC @("list")
    $current = Get-Com0ComPorts $currentLines
    $needsUpdate = $false

    # Reuse unnamed CNCA*/CNCB* slots first (e.g. default CNCA0/CNCB0 created by installer)
    $reusableIndices = New-Object System.Collections.Generic.List[string]
    foreach ($key in $current.Keys) {
        if ($key -match '^CNCA(\d+)$') {
            $idx = $matches[1]
            $bKey = "CNCB$idx"
            if ($current.Contains($bKey)) {
                $aName = $current[$key].port_name
                $bName = $current[$bKey].port_name
                if (($aName -eq "-" -or $aName -eq "COM#") -and ($bName -eq "-" -or $bName -eq "COM#")) {
                    $reusableIndices.Add($idx)
                }
            }
        }
    }

    foreach ($pair in $DesiredPairs) {
        $existingA = $current.Values | Where-Object { $_.port_name.ToUpperInvariant() -eq $pair.client }
        $existingB = $current.Values | Where-Object { $_.port_name.ToUpperInvariant() -eq $pair.sim }
        if ($existingA -and $existingB) {
            if ($existingA.raw -notmatch 'EmuOverrun=yes') {
                Invoke-SetupC @("--no-update", "change", $existingA.id, "EmuOverrun=yes") | Out-Null
                $needsUpdate = $true
            }
            if ($existingB.raw -notmatch 'EmuOverrun=yes') {
                Invoke-SetupC @("--no-update", "change", $existingB.id, "EmuOverrun=yes") | Out-Null
                $needsUpdate = $true
            }
            continue
        }
        if ($existingA -or $existingB) {
            throw "Partial port conflict for pair $($pair.client)<->$($pair.sim); inspect 'setupc list' manually."
        }

        $clientSpec = "PortName=$($pair.client),EmuOverrun=yes"
        $simSpec = "PortName=$($pair.sim),EmuOverrun=yes"
        if ($reusableIndices.Count -gt 0) {
            $idx = $reusableIndices[0]
            $reusableIndices.RemoveAt(0)
            Invoke-SetupC @("--no-update", "change", "CNCA$idx", $clientSpec) | Out-Null
            Invoke-SetupC @("--no-update", "change", "CNCB$idx", $simSpec) | Out-Null
        } else {
            Invoke-SetupC @("--no-update", "install", $clientSpec, $simSpec) | Out-Null
        }
        $needsUpdate = $true
    }

    if ($needsUpdate) {
        Write-Output "Applying com0com driver configuration (setupc update)..."
        Invoke-SetupC @("update") | Out-Null
    }

    $finalLines = Invoke-SetupC @("list")
    Write-Output "=== Final com0com port configuration ==="
    $finalLines | ForEach-Object { Write-Output $_ }

    $finalPorts = Get-Com0ComPorts $finalLines
    foreach ($pair in $DesiredPairs) {
        $clientDevices = @($finalPorts.Values | Where-Object { $_.port_name -ieq $pair.client })
        $simDevices = @($finalPorts.Values | Where-Object { $_.port_name -ieq $pair.sim })
        if ($clientDevices.Count -ne 1 -or $simDevices.Count -ne 1) {
            throw "Expected exactly one configured com0com endpoint for $($pair.client)<->$($pair.sim)."
        }
        $a = $clientDevices[0]
        $b = $simDevices[0]
        if ($a.id -match '^CNCA(\d+)$') {
            $pairNumber = $matches[1]
        } else {
            throw "The client endpoint $($pair.client) is not a CNCA device."
        }
        $aId = $a.id
        $bId = $b.id
        $expectedBId = "CNCB$pairNumber"
        if ($bId -ine $expectedBId) {
            throw "The configured endpoints $($pair.client) and $($pair.sim) are not opposite ends of one com0com pair."
        }
        if ($a.port_name -ine $pair.client -or $b.port_name -ine $pair.sim -or
            $a.raw -notmatch 'EmuOverrun=yes' -or $b.raw -notmatch 'EmuOverrun=yes') {
            throw "Final com0com pair $aId/$bId does not match $($pair.client)<->$($pair.sim) with EmuOverrun=yes."
        }
    }

    if ($resolvedSerialMap) {
        $mapObj = [ordered]@{}
        foreach ($pair in $DesiredPairs) {
            $mapObj[$pair.role] = [ordered]@{
                port        = $pair.sim
                client_port = $pair.client
                baud_rate   = $pair.baud_rate
                stop_bits   = $pair.stop_bits
            }
        }
        $jsonText = ($mapObj | ConvertTo-Json -Depth 4) + "`n"
        $encoding = [System.Text.UTF8Encoding]::new($false)
        $bytes = $encoding.GetBytes($jsonText)
        $stream = [System.IO.File]::Open(
            $resolvedSerialMap,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None
        )
        try {
            $stream.Write($bytes, 0, $bytes.Length)
        } finally {
            $stream.Dispose()
        }
        Write-Output "Wrote serial map: $resolvedSerialMap"
    }
} finally {
    Pop-Location
}
