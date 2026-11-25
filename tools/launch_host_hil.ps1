[CmdletBinding()]
param(
    [string]$HostExe = "D:\CSLearning\IR_dll\optmeas-usb\build\实验\修复验证\Release\FPA测试上位机.exe",
    [string]$WorkDir = "",
    [string]$QtRuntimeDir = "",
    [ValidateSet("dcn1000-serial", "pcn7lt-tcp")]
    [string]$BlackbodyMode = "dcn1000-serial",
    [int]$BlackbodyTcpPort = 21010,
    [switch]$SmokeTestOnly,
    [switch]$LogIo
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not (Test-Path $HostExe)) {
    throw "Host executable not found: $HostExe"
}
$HostExe = (Resolve-Path $HostExe).Path
$HostReleaseDir = Split-Path -Parent $HostExe
$HostBuildRoot = Split-Path -Parent $HostReleaseDir
$HostBinReleaseDir = Join-Path $HostBuildRoot "bin\Release"

if ([string]::IsNullOrWhiteSpace($WorkDir)) {
    $WorkDir = Join-Path $RepoRoot "实验\host-hil-workdir"
}
$WorkDir = [System.IO.Path]::GetFullPath($WorkDir)
$ConfigDir = Join-Path $WorkDir "config"
New-Item -ItemType Directory -Path $ConfigDir -Force | Out-Null

function Find-Qt515Runtime {
    param([string]$ExplicitDir)
    $candidates = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($ExplicitDir)) {
        $candidates.Add($ExplicitDir)
    }
    $packagedRuntime = Join-Path (Split-Path -Parent $RepoRoot) "FPA测试上位机-v1-0-0-win64\FPA测试上位机"
    if (Test-Path $packagedRuntime) { $candidates.Add($packagedRuntime) }
    Get-ChildItem "C:\Qt" -Directory -Filter "5.15*" -ErrorAction SilentlyContinue | ForEach-Object {
        $msvcBin = Join-Path $_.FullName "msvc2019_64\bin"
        if (Test-Path $msvcBin) { $candidates.Add($msvcBin) }
    }
    foreach ($dir in $candidates) {
        if (-not (Test-Path $dir)) { continue }
        $core = Join-Path $dir "Qt5Core.dll"
        $gui = Join-Path $dir "Qt5Gui.dll"
        $widgets = Join-Path $dir "Qt5Widgets.dll"
        $opengl = Join-Path $dir "Qt5OpenGL.dll"
        $svg = Join-Path $dir "Qt5Svg.dll"
        if (-not ((Test-Path $core) -and (Test-Path $gui) -and (Test-Path $widgets) -and (Test-Path $opengl) -and (Test-Path $svg))) {
            continue
        }
        $ver = (Get-Item $core).VersionInfo.FileVersion
        if ($ver -notlike "5.15.*") { continue }
        foreach ($plat in @(
            (Join-Path $dir "platforms"),
            (Join-Path $dir "plugins\platforms"),
            (Join-Path (Split-Path -Parent $dir) "plugins\platforms")
        )) {
            if (Test-Path (Join-Path $plat "qwindows.dll")) {
                return @{
                    BinDir       = $dir
                    PluginRoot   = Split-Path -Parent $plat
                    PlatformsDir = $plat
                    Version      = $ver
                }
            }
        }
    }
    throw "No complete Qt 5.15.x runtime (Qt5Core/Gui/Widgets/OpenGL/Svg + platforms\qwindows.dll) found."
}

function Find-OpenCvBinDir {
    foreach ($dir in @(
        "D:\CSLearning\3rd\opencv-4.14.0\opencv\build\x64\vc16\bin",
        "D:\CS\3rd\opencv\build\x64\vc16\bin"
    )) {
        if (Test-Path (Join-Path $dir "opencv_world4140.dll")) {
            return $dir
        }
    }
    $found = Get-ChildItem "D:\CSLearning\3rd" -Recurse -Filter "opencv_world4140.dll" -Depth 6 -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty DirectoryName
    if ($found) { return $found }
    throw "opencv_world4140.dll not found."
}

$qt = Find-Qt515Runtime -ExplicitDir $QtRuntimeDir
$opencvBin = Find-OpenCvBinDir
if (-not (Test-Path (Join-Path $HostBinReleaseDir "QtPropertyBrowser.dll"))) {
    throw "QtPropertyBrowser.dll not found under $HostBinReleaseDir"
}

$python = (Get-Command python.exe -ErrorAction Stop).Source
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

# Write isolated oven.json under $WorkDir\config (never touching optmeas-usb\config)
$ovenCfg = [ordered]@{
    schema_version = 1
    driver         = "gws_sten"
    transport      = "serial"
    connection     = [ordered]@{
        port      = "COM12"
        baud_rate = 9600
        data_bits = 8
        parity    = "none"
        stop_bits = 1
        delimiter = "crlf"
        address   = 1
    }
    safety         = [ordered]@{
        min_setpoint_c        = -45.0
        max_setpoint_c        = 70.0
        restore_temperature_c = 25.0
    }
    telemetry      = [ordered]@{
        period_ms     = 1000
        stale_ms      = 5000
        io_timeout_ms = 2000
    }
}
[System.IO.File]::WriteAllText((Join-Path $ConfigDir "oven.json"), (($ovenCfg | ConvertTo-Json -Depth 5) + "`n"), $utf8NoBom)

$serialMap = [ordered]@{
    power    = [ordered]@{ port = "COM11"; client_port = "COM10"; baud_rate = 115200; stop_bits = 1 }
    oven     = [ordered]@{ port = "COM13"; client_port = "COM12"; baud_rate = 9600;   stop_bits = 1 }
    dmm      = [ordered]@{ port = "COM15"; client_port = "COM14"; baud_rate = 115200; stop_bits = 1 }
    detector = [ordered]@{ port = "COM17"; client_port = "COM16"; baud_rate = 115200; stop_bits = 1 }
    cooler   = [ordered]@{ port = "COM19"; client_port = "COM18"; baud_rate = 9600;   stop_bits = 2 }
}
if ($BlackbodyMode -eq "dcn1000-serial") {
    $serialMap["blackbody"] = [ordered]@{ port = "COM21"; client_port = "COM20"; baud_rate = 9600; stop_bits = 1 }
} else {
    $serialMap["blackbody"] = [ordered]@{ host = "127.0.0.1"; tcp_port = $BlackbodyTcpPort }
}
$serialMapPath = Join-Path $WorkDir "serial_map.json"
[System.IO.File]::WriteAllText($serialMapPath, (($serialMap | ConvertTo-Json -Depth 4) + "`n"), $utf8NoBom)

$sdkStationPath = Join-Path $WorkDir "sdk_station.json"
$hostBbPath = Join-Path $ConfigDir "blackbody.json"
$rawSource = Join-Path $RepoRoot "examples\data\tc002a_640"
$simOutLog = Join-Path $WorkDir "virtual_station.stdout.log"
$simErrLog = Join-Path $WorkDir "virtual_station.stderr.log"
Remove-Item $simOutLog, $simErrLog -ErrorAction SilentlyContinue

$simArgs = @(
    "-B",
    (Join-Path $RepoRoot "simulators\ir_virtual_station.py"),
    "--mode", "serial-map",
    "--config", $serialMapPath,
    "--control-port", "0",
    "--emit-station-json", $sdkStationPath,
    "--emit-blackbody-config", $hostBbPath,
    "--raw-source", $rawSource
)
if ($LogIo) { $simArgs += "--log-io" }

$simProc = Start-Process -FilePath $python `
    -ArgumentList $simArgs `
    -WorkingDirectory $RepoRoot `
    -RedirectStandardOutput $simOutLog `
    -RedirectStandardError $simErrLog `
    -WindowStyle Hidden `
    -PassThru

$ctrlUrl = $null
$hostProc = $null
$oldPath = $env:PATH
$oldPluginPath = $env:QT_PLUGIN_PATH
$oldPlat = $env:QT_QPA_PLATFORM_PLUGIN_PATH

try {
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        if ($simProc.HasExited) {
            $errText = if (Test-Path $simErrLog) { Get-Content $simErrLog -Raw } else { "" }
            throw "ir_virtual_station.py exited unexpectedly (exit=$($simProc.ExitCode)):`n$errText"
        }
        if (Test-Path $simOutLog) {
            $readyLine = Get-Content $simOutLog -ErrorAction SilentlyContinue |
                Where-Object { $_ -match '^VIRTUAL_STATION_READY=(.+)$' } |
                Select-Object -First 1
            if ($readyLine -and (Test-Path $hostBbPath)) {
                $readyJson = ($readyLine -split '=', 2)[1] | ConvertFrom-Json
                $ctrlUrl = $readyJson.control_url
                break
            }
        }
        Start-Sleep -Milliseconds 150
    }
    if (-not $ctrlUrl) {
        throw "Timed out waiting for VIRTUAL_STATION_READY from ir_virtual_station.py."
    }

    Write-Output "=== Host HIL Virtual Station Ready ==="
    Write-Output "HostExe        : $HostExe"
    Write-Output "WorkDir        : $WorkDir"
    Write-Output "Qt 5.15 Bin    : $($qt.BinDir) (v$($qt.Version))"
    Write-Output "Qt Plugin Root : $($qt.PluginRoot)"
    Write-Output "OpenCV Bin     : $opencvBin"
    Write-Output "Control URL    : $ctrlUrl"
    if ($BlackbodyMode -eq "dcn1000-serial") {
        Write-Output "Blackbody      : dcn1000 serial COM20 (sim COM21)"
    } else {
        Write-Output "Blackbody      : pcn7lt tcp 127.0.0.1:$BlackbodyTcpPort"
    }
    Write-Output "Power/Oven/DMM : COM10 (IT6332A) / COM12 (GWS STEN) / COM14 (DMM6500)"
    Write-Output "Detector/Cooler: COM16 (Detector) / COM18 (Cooler 9600 8N2)"

    # Keep $opencvBin (v4.14.0) and $HostReleaseDir (current qwt.dll) ahead of $qt.BinDir
    $env:PATH = (@($HostReleaseDir, $HostBinReleaseDir, $opencvBin, $qt.BinDir) -join ";") + ";" + $oldPath
    $env:QT_PLUGIN_PATH = $qt.PluginRoot
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $qt.PlatformsDir

    $hostProc = Start-Process -FilePath $HostExe `
        -ArgumentList @("--label", "HIL-VirtualStation") `
        -WorkingDirectory $WorkDir `
        -PassThru

    if ($SmokeTestOnly) {
        $winCheck = ""
        $winDeadline = (Get-Date).AddSeconds(8)
        while ((Get-Date) -lt $winDeadline) {
            if ($hostProc.HasExited) {
                throw "FPA host exited early during smoke test (exit=$($hostProc.ExitCode))."
            }
            $checkJson = @'
import ctypes, json, sys
target_pid = int(sys.argv[1])
label = sys.argv[2]
user32 = ctypes.windll.user32
wins = []
@ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
def cb(hwnd, _):
    pid = ctypes.c_ulong()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if pid.value == target_pid and user32.IsWindowVisible(hwnd):
        cls = ctypes.create_unicode_buffer(256)
        txt = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, cls, 256)
        user32.GetWindowTextW(hwnd, txt, 256)
        if cls.value.startswith("Qt") and label in txt.value:
            wins.append({"hwnd": int(hwnd), "cls": cls.value, "title": txt.value})
    return True
user32.EnumWindows(cb, 0)
print(json.dumps(wins, ensure_ascii=True))
'@ | & $python -B - $hostProc.Id "HIL-VirtualStation"
            $parsedWins = $checkJson | ConvertFrom-Json
            if ($parsedWins.Count -gt 0) {
                $winCheck = $checkJson.Trim()
                break
            }
            Start-Sleep -Milliseconds 300
        }
        if ([string]::IsNullOrWhiteSpace($winCheck)) {
            throw "FPA host GUI MainWindow (Qt5152QWindowIcon) was not visible within timeout."
        }
        $state = Invoke-RestMethod -Uri "$ctrlUrl/state" -Method Get -TimeoutSec 3
        if (-not $state.blackbody -or -not $state.power -or -not $state.oven -or -not $state.cooler) {
            throw "Virtual station /state missing expected device sections."
        }
        foreach ($win in $parsedWins) {
            @'
import ctypes, sys
ctypes.windll.user32.PostMessageW(int(sys.argv[1]), 0x0010, 0, 0)
'@ | & $python -B - $win.hwnd
        }
        if (-not $hostProc.WaitForExit(5000)) {
            $hostProc.Kill()
            throw "FPA host did not exit cleanly after WM_CLOSE."
        }
        if ($hostProc.ExitCode -ne 0) {
            throw "FPA host exited with non-zero code after WM_CLOSE: $($hostProc.ExitCode)"
        }
        Write-Output ("Smoke state OK : host_pid=$($hostProc.Id) window=$winCheck blackbody=$($BlackbodyMode) cooler_mv=$($state.cooler.voltage_mv)")
        Write-Output "HOST_HIL_SMOKE=PASS"
    } else {
        Write-Output "FPA host GUI running (PID=$($hostProc.Id)). Close the GUI window to shut down virtual station."
        $hostProc.WaitForExit()
    }
} finally {
    $env:PATH = $oldPath
    $env:QT_PLUGIN_PATH = $oldPluginPath
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $oldPlat
    if ($hostProc -and -not $hostProc.HasExited) {
        try {
            $null = $hostProc.CloseMainWindow()
            if (-not $hostProc.WaitForExit(3000)) { $hostProc.Kill() }
        } catch {}
    }
    if ($ctrlUrl) {
        try { Invoke-RestMethod -Uri "$ctrlUrl/shutdown" -Method Post -Body "{}" -ContentType "application/json" -TimeoutSec 2 | Out-Null } catch {}
    }
    if ($simProc -and -not $simProc.HasExited) {
        if (-not $simProc.WaitForExit(3000)) {
            try { $simProc.Kill() } catch {}
        }
    }
}
