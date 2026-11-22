[CmdletBinding()]
param(
    [string]$WorkRoot = "",
    [switch]$RequireClean,
    [string]$PythonExe = "",
    [switch]$RunCom0ComHil
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
try {
    [Console]::OutputEncoding = $Utf8NoBom
} catch {
}

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Format-WinArg([string]$Arg) {
    if ($null -eq $Arg) { return '""' }
    if ($Arg.Length -gt 0 -and $Arg -notmatch '[\s"]') {
        return $Arg
    }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('"')
    $backslashes = 0
    for ($i = 0; $i -lt $Arg.Length; $i++) {
        $ch = $Arg[$i]
        if ($ch -eq '\') {
            $backslashes++
        } elseif ($ch -eq '"') {
            [void]$sb.Append('\', $backslashes * 2 + 1)
            [void]$sb.Append('"')
            $backslashes = 0
        } else {
            if ($backslashes -gt 0) {
                [void]$sb.Append('\', $backslashes)
                $backslashes = 0
            }
            [void]$sb.Append($ch)
        }
    }
    if ($backslashes -gt 0) {
        [void]$sb.Append('\', $backslashes * 2)
    }
    [void]$sb.Append('"')
    return $sb.ToString()
}

function Invoke-External {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = "",
        [hashtable]$Environment = @{},
        [int[]]$AllowedExitCodes = @(0)
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = (($Arguments | ForEach-Object { Format-WinArg $_ }) -join " ")
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = $Utf8NoBom
    $psi.StandardErrorEncoding = $Utf8NoBom
    if ($WorkingDirectory) {
        $psi.WorkingDirectory = $WorkingDirectory
    }
    foreach ($key in $Environment.Keys) {
        $psi.EnvironmentVariables[$key] = [string]$Environment[$key]
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi
    if (-not $process.Start()) {
        throw "Failed to start process: $FilePath"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $code = $process.ExitCode
    $process.Dispose()

    if ($AllowedExitCodes -notcontains $code) {
        throw ("Command failed (exit={0}): {1} {2}`n--- STDOUT ---`n{3}`n--- STDERR ---`n{4}" -f `
            $code, $FilePath, $psi.Arguments, $stdout, $stderr)
    }
    return [pscustomobject]@{
        ExitCode = $code
        StdOut   = $stdout
        StdErr   = $stderr
    }
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw "Assertion failed: $Message"
    }
}

function Write-Utf8File([string]$Path, [string]$Content) {
    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Content, $Utf8NoBom)
}

function Get-FreeTcpPort {
    $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    $listener.Stop()
    return $port
}

function Find-VsInstallation {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -all -products * -latest `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($found -and (Test-Path $found)) {
            return $found.Trim()
        }
    }
    $fallback = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    if (Test-Path $fallback) {
        return $fallback
    }
    throw "Visual Studio 2022 BuildTools with VC x64 tools not found."
}

function Import-VsDevEnv([string]$VsPath) {
    $vcvars = Join-Path $VsPath "VC\Auxiliary\Build\vcvars64.bat"
    Assert-True (Test-Path $vcvars) "vcvars64.bat not found at $vcvars"
    $envDump = & cmd.exe /d /s /c "`"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialize MSVC x64 environment via $vcvars"
    }
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        }
    }
    $extraDirs = @(
        (Join-Path $VsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"),
        (Join-Path $VsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja")
    )
    $currentPath = [System.Environment]::GetEnvironmentVariable("PATH", "Process")
    foreach ($dir in $extraDirs) {
        if ((Test-Path $dir) -and ($currentPath -notlike "*$dir*")) {
            $currentPath = "$dir;$currentPath"
        }
    }
    [System.Environment]::SetEnvironmentVariable("PATH", $currentPath, "Process")
}

function Find-WindowsPython([string]$Override) {
    if ($Override) {
        Assert-True (Test-Path $Override) "Specified Python executable not found: $Override"
        return (Resolve-Path $Override).Path
    }
    $candidates = @(Get-Command python.exe -All -ErrorAction SilentlyContinue | ForEach-Object { $_.Source })
    foreach ($cand in $candidates) {
        if ($cand -match 'WindowsApps\\python\.exe$') { continue }
        if ($cand -match 'msys64') { continue }
        if (Test-Path $cand) { return $cand }
    }
    foreach ($cand in $candidates) {
        if ($cand -match 'WindowsApps\\python\.exe$') { continue }
        if (Test-Path $cand) { return $cand }
    }
    throw "Native Windows Python 3 executable not found on PATH."
}

function Assert-TraceabilityFields($Record, [string]$ExpectedVersion, [string]$Label) {
    Assert-True ($Record.station_version -eq $ExpectedVersion) "$Label station_version expected $ExpectedVersion, got $($Record.station_version)"
    Assert-True ($Record.analysis_version -eq $ExpectedVersion) "$Label analysis_version expected $ExpectedVersion, got $($Record.analysis_version)"
    Assert-True (-not [string]::IsNullOrWhiteSpace($Record.source_commit)) "$Label source_commit is empty"
    Assert-True ($Record.started_utc -match '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$') "$Label invalid started_utc: $($Record.started_utc)"
    Assert-True ($Record.finished_utc -match '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$') "$Label invalid finished_utc: $($Record.finished_utc)"
    Assert-True ([string]::CompareOrdinal($Record.started_utc, $Record.finished_utc) -le 0) "$Label started_utc > finished_utc"
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ExperimentDir = Join-Path $RepoRoot ([string]([char]0x5B9E) + [char]0x9A8C)
if (-not $WorkRoot) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $WorkRoot = Join-Path $ExperimentDir "verify-windows-$stamp"
}
$WorkRoot = [System.IO.Path]::GetFullPath($WorkRoot)
New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null

$cmakeText = [System.IO.File]::ReadAllText((Join-Path $RepoRoot "CMakeLists.txt"), $Utf8NoBom)
if ($cmakeText -notmatch 'project\(IRSDK VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Cannot parse PROJECT_VERSION from CMakeLists.txt"
}
$SdkVersion = $matches[1]
$ExpectedFileVersion = "$SdkVersion.0"

Write-Step "1. Setting up VS 2022 BuildTools x64 + CMake + Ninja + Python environment"
$vsPath = Find-VsInstallation
Import-VsDevEnv $vsPath
$pythonPath = Find-WindowsPython $PythonExe
$cmakePath = (Get-Command cmake.exe).Source
$ctestPath = (Get-Command ctest.exe).Source
$ninjaPath = (Get-Command ninja.exe).Source
$dumpbinPath = (Get-Command dumpbin.exe).Source
$clPath = (Get-Command cl.exe).Source

Write-Host "VS Path    : $vsPath"
Write-Host "cl.exe     : $clPath"
Write-Host "cmake.exe  : $cmakePath"
Write-Host "ninja.exe  : $ninjaPath"
Write-Host "python.exe : $pythonPath"
Write-Host "SDK Version: $SdkVersion (FileVersion $ExpectedFileVersion)"
Write-Host "WorkRoot   : $WorkRoot"

Write-Step "2. Configuring and building Release via CMake windows preset"
$res = Invoke-External -FilePath $cmakePath -WorkingDirectory $RepoRoot `
    -Arguments @("--preset", "windows", "-DPython3_EXECUTABLE=$pythonPath")
Write-Host $res.StdOut.Trim()
$res = Invoke-External -FilePath $cmakePath -WorkingDirectory $RepoRoot `
    -Arguments @("--build", "--preset", "windows")
if ($res.StdOut.Trim()) { Write-Host $res.StdOut.Trim() }

Write-Step "3. Running CTest via windows preset"
$res = Invoke-External -FilePath $ctestPath -WorkingDirectory $RepoRoot `
    -Arguments @("--preset", "windows", "--output-on-failure")
Write-Host $res.StdOut.Trim()

$BuildDir = Join-Path $ExperimentDir "build-windows"
$BuildBinDir = Join-Path $BuildDir "bin"
$StationDll = Join-Path $BuildBinDir "IR_station.dll"
$AnalysisDll = Join-Path $BuildBinDir "IR_analysis.dll"
$CliExe = Join-Path $BuildBinDir "ir_run_recipe.exe"
$AcceptExe = Join-Path $BuildBinDir "ir_capture_accept.exe"

Write-Step "3b. Checking incremental rebuild does not recompile or relink unchanged sources"
$beforeStationStamp = (Get-Item $StationDll).LastWriteTimeUtc
$beforeAnalysisStamp = (Get-Item $AnalysisDll).LastWriteTimeUtc
$beforeCliStamp = (Get-Item $CliExe).LastWriteTimeUtc
$beforeAcceptStamp = (Get-Item $AcceptExe).LastWriteTimeUtc
$incRes = Invoke-External -FilePath $cmakePath -WorkingDirectory $RepoRoot `
    -Arguments @("--build", "--preset", "windows")
$incOut = ($incRes.StdOut + "`n" + $incRes.StdErr).Trim()
if ($incOut) { Write-Host $incOut }
Assert-True ($incOut -notmatch '(?i)(\.cpp\b|\.rc\b|Creating library|Linking\b)') `
    "Incremental rebuild unexpectedly compiled or linked:`n$incOut"
Assert-True ((Get-Item $StationDll).LastWriteTimeUtc -eq $beforeStationStamp) "IR_station.dll timestamp changed on no-op rebuild"
Assert-True ((Get-Item $AnalysisDll).LastWriteTimeUtc -eq $beforeAnalysisStamp) "IR_analysis.dll timestamp changed on no-op rebuild"
Assert-True ((Get-Item $CliExe).LastWriteTimeUtc -eq $beforeCliStamp) "ir_run_recipe.exe timestamp changed on no-op rebuild"
Assert-True ((Get-Item $AcceptExe).LastWriteTimeUtc -eq $beforeAcceptStamp) "ir_capture_accept.exe timestamp changed on no-op rebuild"

Write-Step "4. Checking DLL/EXE dependents and exports via dumpbin"
foreach ($bin in @($StationDll, $AnalysisDll, $CliExe, $AcceptExe)) {
    Assert-True (Test-Path $bin) "Missing built binary: $bin"
    $dep = Invoke-External -FilePath $dumpbinPath -Arguments @("/nologo", "/dependents", $bin)
    $dllLines = @($dep.StdOut -split "`r?`n" | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() })
    Assert-True ($dllLines.Count -gt 0) "Failed to parse DLL dependents for $(Split-Path -Leaf $bin)"
    Write-Host ("  {0} -> {1}" -f (Split-Path -Leaf $bin), ($dllLines -join ", "))
    foreach ($dll in $dllLines) {
        Assert-True ($dll -notmatch '(?i)(qt\d|opencv|asio|nlohmann|fftw|vcomp)') "Forbidden runtime dependency on $(Split-Path -Leaf $bin): $dll"
    }
    if ($bin -eq $StationDll) {
        Assert-True (($dllLines -contains "IR_analysis.dll")) "IR_station.dll must depend on IR_analysis.dll"
    }
}
$stationExports = (Invoke-External -FilePath $dumpbinPath -Arguments @("/nologo", "/exports", $StationDll)).StdOut
foreach ($sym in @(
    "IR_StationVersion",
    "IR_FlushFrames",
    "IR_WaitBlackbodyStable",
    "IR_SetFrameQueueCapacity",
    "IR_TryReadLatestFrame",
    "IR_GetCaptureStats",
    "IR_ListSerialPorts",
    "IR_ListCaptureDevices",
    "IR_SetOven",
    "IR_ReadOven",
    "IR_StopOven",
    "IR_WaitOvenStable",
    "IR_SetDmmMode",
    "IR_ReadDmm",
    "IR_SetDetectorTiming",
    "IR_SetDetectorGpol",
    "IR_SetDetectorRegister",
    "IR_MeasureDetectorCurrent",
    "IR_ReadCooler",
    "IR_WaitPowerCurrentStable"
)) {
    Assert-True ($stationExports -match "\b$sym\b") "IR_station.dll must export $sym"
}
$analysisExports = (Invoke-External -FilePath $dumpbinPath -Arguments @("/nologo", "/exports", $AnalysisDll)).StdOut
foreach ($sym in @(
    "IR_AnalysisVersion",
    "IR_FrameStatisticsU16",
    "IR_ROIMeanU16",
    "IR_Noise3DBuildCalibDn",
    "IR_Noise3DComputeFramesDn",
    "IR_GbProcessBadPixels",
    "IR_GbEvaluateMaps",
    "IR_GjbProcessBadPixels",
    "IR_GjbEvaluateMaps",
    "IR_BuildTwoPointNucF64",
    "IR_ApplyAffineMapF64",
    "IR_FrameStatisticsNucU16",
    "IR_MapSummaryF64",
    "IR_SpatialLowHighPassF64",
    "IR_MapBinaryOpF64",
    "IR_ThresholdMaskF64",
    "IR_MaskCombine",
    "IR_ComputeRfft1D",
    "IR_ChannelSpectrumU16",
    "IR_GjbEvaluateBurstMetrics",
    "IR_FlickerDetectU16",
    "IR_Fft2dMagnitudeF64"
)) {
    Assert-True ($analysisExports -match "\b$sym\b") "IR_analysis.dll must export $sym"
}

Write-Step "5. Checking Windows VERSIONINFO FileVersion ($ExpectedFileVersion)"
$DllComments = @{}
foreach ($dll in @($StationDll, $AnalysisDll)) {
    $leaf = Split-Path -Leaf $dll
    $vi = (Get-Item $dll).VersionInfo
    Write-Host ("  {0}: FileVersion={1} ProductVersion={2} Comments={3}" -f `
        $leaf, $vi.FileVersion, $vi.ProductVersion, $vi.Comments)
    Assert-True ($vi.FileVersion -eq $ExpectedFileVersion) "$leaf FileVersion expected $ExpectedFileVersion, got $($vi.FileVersion)"
    Assert-True ($vi.ProductVersion -eq $ExpectedFileVersion) "$leaf ProductVersion expected $ExpectedFileVersion, got $($vi.ProductVersion)"
    Assert-True ($vi.Comments -match '^Source commit:\s+\S+') "$leaf Comments missing source commit: $($vi.Comments)"
    $DllComments[$leaf] = $vi.Comments
}

Write-Step "6. Installing Windows SDK prefix"
$InstallDir = Join-Path $WorkRoot "install-windows"
Invoke-External -FilePath $cmakePath -WorkingDirectory $RepoRoot `
    -Arguments @("--install", $BuildDir, "--prefix", $InstallDir) | Out-Null
Write-Host "  Installed to $InstallDir"

Write-Step "7. Packaging Windows-only SDK archive with hygiene verification"
$PackageDir = Join-Path $WorkRoot "package"
New-Item -ItemType Directory -Path $PackageDir -Force | Out-Null
$ZipPath = Join-Path $PackageDir "IR-SDK-$SdkVersion-Windows.zip"
$pkgArgs = @(
    "-B",
    (Join-Path $RepoRoot "tools\package_sdk.py"),
    "--windows-only",
    "--windows-install", $InstallDir,
    "--output", $ZipPath
)
if ($RequireClean) {
    $pkgArgs += "--require-clean"
}
$res = Invoke-External -FilePath $pythonPath -WorkingDirectory $RepoRoot -Arguments $pkgArgs
Write-Host $res.StdOut.Trim()
Assert-True (Test-Path $ZipPath) "Package archive not created: $ZipPath"
$ShaPath = "$ZipPath.sha256"
Assert-True (Test-Path $ShaPath) "Package SHA-256 sidecar not created: $ShaPath"
$expectedSha = (([System.IO.File]::ReadAllText($ShaPath, $Utf8NoBom)).Trim() -split '\s+')[0].ToLowerInvariant()
$actualSha = (Get-FileHash -Path $ZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
Assert-True ($actualSha -eq $expectedSha) "Archive SHA-256 mismatch: $actualSha vs $expectedSha"

Write-Step "8. Extracting archive into path containing Chinese characters and spaces"
$ChineseSpacesName = ([string][char]0x89E3 + [char]0x538B + " " + [char]0x9A8C + [char]0x8BC1 + " " + [char]0x8DEF + [char]0x5F84)
$ExtractParent = Join-Path $WorkRoot $ChineseSpacesName
New-Item -ItemType Directory -Path $ExtractParent -Force | Out-Null
Invoke-External -FilePath $cmakePath -WorkingDirectory $ExtractParent `
    -Arguments @("-E", "tar", "xf", $ZipPath) | Out-Null
$PkgRoot = Join-Path $ExtractParent "IR-SDK-$SdkVersion-Windows"
Assert-True (Test-Path $PkgRoot) "Extracted package directory not found: $PkgRoot"
$manifest = [System.IO.File]::ReadAllText((Join-Path $PkgRoot "manifest.json"), $Utf8NoBom) | ConvertFrom-Json
Assert-True ($manifest.package_mode -eq "windows-only") "manifest package_mode should be windows-only"
Assert-True ($manifest.hygiene_check_passed -eq $true) "manifest hygiene_check_passed should be true"
if ($manifest.source_commit -and $manifest.source_commit -ne "unknown") {
    $expectedCommit = if ($manifest.source_worktree_dirty -eq $true) { "$($manifest.source_commit)-dirty" } else { "$($manifest.source_commit)" }
    $expectedComment = "Source commit: $expectedCommit"
    Assert-True ($DllComments["IR_station.dll"] -eq $expectedComment) `
        "IR_station.dll Comments '$($DllComments["IR_station.dll"])' != '$expectedComment'"
    Assert-True ($DllComments["IR_analysis.dll"] -eq $expectedComment) `
        "IR_analysis.dll Comments '$($DllComments["IR_analysis.dll"])' != '$expectedComment'"
}
$manifestShaLines = [System.IO.File]::ReadAllLines((Join-Path $PkgRoot "MANIFEST.sha256"), $Utf8NoBom)
$checkedManifestFiles = 0
foreach ($line in $manifestShaLines) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    Assert-True ($line -match '^([0-9a-fA-F]{64})  (.+)$') "Malformed MANIFEST.sha256 line: $line"
    $hash = $matches[1].ToLowerInvariant()
    $rel = $matches[2]
    $targetFile = Join-Path $PkgRoot ($rel -replace '/', '\')
    Assert-True (Test-Path $targetFile) "MANIFEST entry missing: $rel"
    $gotHash = (Get-FileHash -Path $targetFile -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-True ($gotHash -eq $hash) "MANIFEST hash mismatch for $rel"
    $checkedManifestFiles++
}
$actualExtractedFiles = @(Get-ChildItem -Path $PkgRoot -Recurse -File | Where-Object { $_.Name -ne "MANIFEST.sha256" }).Count
Assert-True ($checkedManifestFiles -gt 0 -and $checkedManifestFiles -eq $actualExtractedFiles) `
    "MANIFEST file count mismatch: listed=$checkedManifestFiles extracted=$actualExtractedFiles"
Write-Host "  Extracted and verified $checkedManifestFiles files under: $PkgRoot"

Write-Step "9. Building and running standalone C/C++ client examples from extracted package"
$ClientBuildDir = Join-Path $WorkRoot "client-build"
Invoke-External -FilePath $cmakePath -WorkingDirectory $PkgRoot `
    -Arguments @("-S", (Join-Path $PkgRoot "examples"), "-B", $ClientBuildDir,
                 "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DIR_SDK_ROOT=$PkgRoot") | Out-Null
Invoke-External -FilePath $cmakePath -WorkingDirectory $PkgRoot `
    -Arguments @("--build", $ClientBuildDir) | Out-Null

$cClientRes = Invoke-External -FilePath (Join-Path $ClientBuildDir "ir_c_client.exe") -Arguments @($SdkVersion)
Assert-True ($cClientRes.StdOut.Trim() -eq $SdkVersion) "ir_c_client output mismatch: $($cClientRes.StdOut)"
$cppClientRes = Invoke-External -FilePath (Join-Path $ClientBuildDir "ir_cpp_client.exe")
Assert-True ($cppClientRes.StdOut -match "station_version=$([regex]::Escape($SdkVersion))") "ir_cpp_client missing station_version"
Assert-True ($cppClientRes.StdOut -match "analysis_version=$([regex]::Escape($SdkVersion))") "ir_cpp_client missing analysis_version"
$analysisCExe = Join-Path $ClientBuildDir "ir_analysis_c_sample.exe"
$analysisCDep = Invoke-External -FilePath $dumpbinPath -Arguments @("/nologo", "/dependents", $analysisCExe)
$analysisCDllLines = @($analysisCDep.StdOut -split "`r?`n" | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() })
Assert-True ($analysisCDllLines -contains "IR_analysis.dll") "ir_analysis_c_sample.exe must depend on IR_analysis.dll"
Assert-True (-not ($analysisCDllLines -contains "IR_station.dll")) "ir_analysis_c_sample.exe must not depend on IR_station.dll (got: $($analysisCDllLines -join ', '))"
$analysisCRes = Invoke-External -FilePath $analysisCExe
Assert-True ($analysisCRes.StdOut -match "(?m)^analysis_version=$([regex]::Escape($SdkVersion))\r?$") "ir_analysis_c_sample missing analysis_version"
Assert-True ($analysisCRes.StdOut -match "(?m)^gb_grade=A\r?$") "ir_analysis_c_sample missing gb_grade=A"
Assert-True ($analysisCRes.StdOut -match "(?m)^gjb_bad_count=0\r?$") "ir_analysis_c_sample missing gjb_bad_count=0"
Assert-True ($analysisCRes.StdOut -match "(?m)^noise3d_total=") "ir_analysis_c_sample missing noise3d_total"
Assert-True ($analysisCRes.StdOut -match "(?m)^lrnu_ok=1\r?$") "ir_analysis_c_sample missing lrnu_ok=1"
Assert-True ($analysisCRes.StdOut -match "(?m)^fft_ok=1\r?$") "ir_analysis_c_sample missing fft_ok=1"
Assert-True ($analysisCRes.StdOut -match "(?m)^gaussian_ok=1\r?$") "ir_analysis_c_sample missing gaussian_ok=1"
Assert-True ($analysisCRes.StdOut -match "(?m)^flicker_ok=1\r?$") "ir_analysis_c_sample missing flicker_ok=1"
Assert-True ($analysisCRes.StdOut -match "(?m)^fft2d_ok=1\r?$") "ir_analysis_c_sample missing fft2d_ok=1"
$standaloneCliCheck = Invoke-External -FilePath (Join-Path $ClientBuildDir "ir_run_recipe.exe") `
    -Arguments @("--check", "--station", (Join-Path $PkgRoot "examples\stations\offline.json"),
                 "--recipe", (Join-Path $PkgRoot "examples\recipes\noise_roi.json"),
                 "--output", (Join-Path $WorkRoot "standalone-check-unused"))
Assert-True ($standaloneCliCheck.StdOut.Trim() -eq "recipe=VALID") "standalone ir_run_recipe --check failed"
$standaloneAcceptList = Invoke-External -FilePath (Join-Path $ClientBuildDir "ir_capture_accept.exe") -Arguments @("--list")
$standaloneAcceptObj = $standaloneAcceptList.StdOut | ConvertFrom-Json
Assert-True ($null -ne $standaloneAcceptObj.capture_devices -and $null -ne $standaloneAcceptObj.serial_ports) "standalone ir_capture_accept --list output invalid"
Write-Host "  Standalone ir_c_client, ir_cpp_client, ir_analysis_c_sample, ir_run_recipe, and ir_capture_accept verified."

Write-Step "10. Running packaged CLI tests (PASS / ROI 2x2 / NG / ERROR)"
$PkgCli = Join-Path $PkgRoot "windows-x64\bin\ir_run_recipe.exe"
$OfflineStation = Join-Path $PkgRoot "examples\stations\offline.json"
$OfflineRecipe = Join-Path $PkgRoot "examples\recipes\noise_roi.json"
$PassOut = Join-Path $WorkRoot "cli-pass-out"
$checkRes = Invoke-External -FilePath $PkgCli -Arguments @("--check", "--station", $OfflineStation, "--recipe", $OfflineRecipe, "--output", $PassOut)
Assert-True ($checkRes.StdOut.Trim() -eq "recipe=VALID") "CLI --check failed"
$passRes = Invoke-External -FilePath $PkgCli -Arguments @("--run", "--station", $OfflineStation, "--recipe", $OfflineRecipe, "--output", $PassOut)
Assert-True ($passRes.StdOut -match "(?m)^run_status=PASS\r?$") "CLI PASS missing run_status=PASS (got: $($passRes.StdOut))"
$passJson = [System.IO.File]::ReadAllText((Join-Path $PassOut "result.json"), $Utf8NoBom) | ConvertFrom-Json
Assert-True ($passJson.run_status -eq "PASS") "CLI PASS result.json status mismatch"
Assert-TraceabilityFields $passJson $SdkVersion "CLI PASS"

$RoiOut = Join-Path $WorkRoot "cli-roi-2x2-out"
$roiRes = Invoke-External -FilePath $PkgCli -Arguments @(
    "--run",
    "--station", (Join-Path $PkgRoot "examples\stations\roi_2x2.json"),
    "--recipe", (Join-Path $PkgRoot "examples\recipes\noise_roi_2x2.json"),
    "--output", $RoiOut
)
Assert-True ($roiRes.StdOut -match "(?m)^run_status=PASS\r?$") "CLI ROI 2x2 failed (got: $($roiRes.StdOut))"
$roiJson = [System.IO.File]::ReadAllText((Join-Path $RoiOut "result.json"), $Utf8NoBom) | ConvertFrom-Json
Assert-True ([math]::Abs([double]$roiJson.metrics.'left.mean_dn'.value - 41.0) -lt 1e-12) "ROI 2x2 left.mean_dn != 41"
Assert-True ([math]::Abs([double]$roiJson.metrics.'noise.mean_dn'.value - 51.0) -lt 1e-12) "ROI 2x2 noise.mean_dn != 51"

$V2NoDiskOut = Join-Path $WorkRoot "cli-v2-nodisk-out"
$v2NoDiskRes = Invoke-External -FilePath $PkgCli -Arguments @(
    "--run",
    "--station", (Join-Path $PkgRoot "examples\stations\roi_2x2.json"),
    "--recipe", (Join-Path $PkgRoot "examples\recipes\nodisk_roi_v2.json"),
    "--output", $V2NoDiskOut
)
Assert-True ($v2NoDiskRes.StdOut -match "(?m)^run_status=PASS\r?$") "CLI Schema v2 no-disk failed (got: $($v2NoDiskRes.StdOut))"
$v2NoDiskJson = [System.IO.File]::ReadAllText((Join-Path $V2NoDiskOut "result.json"), $Utf8NoBom) | ConvertFrom-Json
Assert-True ($v2NoDiskJson.schema_version -eq 2) "CLI Schema v2 result schema_version != 2"
Assert-True ($v2NoDiskJson.raw_saved -eq $false) "CLI Schema v2 raw_saved should be false"
Assert-True ([math]::Abs([double]$v2NoDiskJson.metrics.'summary.mean'.value - 51.0) -lt 1e-12) "Schema v2 summary.mean != 51"
Assert-True ([math]::Abs([double]$v2NoDiskJson.metrics.'left.mean_dn'.value - 41.0) -lt 1e-12) "Schema v2 left.mean_dn != 41"
$v2RawFiles = @(Get-ChildItem -Path $V2NoDiskOut -Recurse -Filter "*.raw")
Assert-True ($v2RawFiles.Count -eq 0) "CLI Schema v2 no-disk must not save .raw files"

$TempRecipesDir = Join-Path $WorkRoot "temp-recipes"
New-Item -ItemType Directory -Path $TempRecipesDir -Force | Out-Null

$ngRecipeObj = [System.IO.File]::ReadAllText($OfflineRecipe, $Utf8NoBom) | ConvertFrom-Json
$ngRecipeObj.limits[0].value = 1.0
$NgRecipePath = Join-Path $TempRecipesDir "noise_roi_ng.json"
Write-Utf8File $NgRecipePath ($ngRecipeObj | ConvertTo-Json -Depth 10)
$NgOut = Join-Path $WorkRoot "cli-ng-out"
$ngRes = Invoke-External -FilePath $PkgCli -Arguments @("--run", "--station", $OfflineStation, "--recipe", $NgRecipePath, "--output", $NgOut) -AllowedExitCodes @(20)
Assert-True ($ngRes.ExitCode -eq 20) "CLI NG exit code expected 20"
Assert-True ($ngRes.StdOut -match "(?m)^run_status=NG\r?$") "CLI NG stdout missing run_status=NG (got: $($ngRes.StdOut))"
$ngJson = [System.IO.File]::ReadAllText((Join-Path $NgOut "result.json"), $Utf8NoBom) | ConvertFrom-Json
Assert-True ($ngJson.run_status -eq "NG") "CLI NG result.json status mismatch"

$errRecipeObj = [System.IO.File]::ReadAllText($OfflineRecipe, $Utf8NoBom) | ConvertFrom-Json
$errRecipeObj.steps[0].frames = 3
$ErrRecipePath = Join-Path $TempRecipesDir "noise_roi_short_error.json"
Write-Utf8File $ErrRecipePath ($errRecipeObj | ConvertTo-Json -Depth 10)
$ErrOut = Join-Path $WorkRoot "cli-error-out"
$errRes = Invoke-External -FilePath $PkgCli -Arguments @("--run", "--station", $OfflineStation, "--recipe", $ErrRecipePath, "--output", $ErrOut) -AllowedExitCodes @(11)
Assert-True ($errRes.ExitCode -eq 11) "CLI ERROR exit code expected 11 (IR_END_OF_STREAM)"
Assert-True ($errRes.StdOut -match "(?m)^run_status=ERROR\r?$") "CLI ERROR stdout missing run_status=ERROR (got: $($errRes.StdOut))"
Assert-True ($errRes.StdOut -match "(?m)^completed_frames=2\r?$") "CLI ERROR stdout missing completed_frames=2 (got: $($errRes.StdOut))"
Assert-True ($errRes.StdErr -match "stage=5" -and $errRes.StdErr -match "cleanup_status=0") "CLI ERROR stderr missing diagnostics"
Assert-True (-not (Test-Path $ErrOut)) "Failed CLI run must not create output directory"

$BadRecipePath = Join-Path $TempRecipesDir "invalid_empty_steps.json"
Write-Utf8File $BadRecipePath '{"schema_version":1,"recipe_id":"invalid","steps":[]}'
$badCheckRunRes = Invoke-External -FilePath $PkgCli -Arguments @("--run", "--station", $OfflineStation, "--recipe", $BadRecipePath, "--output", (Join-Path $WorkRoot "cli-bad-recipe-out")) -AllowedExitCodes @(2)
Assert-True ($badCheckRunRes.StdOut -match "(?m)^run_status=ERROR\r?$") "CLI bad-recipe --run missing run_status=ERROR"
Assert-True ($badCheckRunRes.StdOut -match "(?m)^completed_frames=0\r?$") "CLI bad-recipe --run missing completed_frames=0"
$PkgAccept = Join-Path $PkgRoot "windows-x64\bin\ir_capture_accept.exe"
$acceptListRes = Invoke-External -FilePath $PkgAccept -Arguments @("--list")
$acceptListObj = $acceptListRes.StdOut | ConvertFrom-Json
Assert-True ($acceptListObj.station_version -eq $SdkVersion) "ir_capture_accept --list version mismatch"
$AcceptReportPath = Join-Path $WorkRoot "capture-accept-report.json"
$acceptSoakRes = Invoke-External -FilePath $PkgAccept -Arguments @(
    "--backend", "raw_replay",
    "--source", (Join-Path $PkgRoot "examples\data\roi_2x2"),
    "--frames", "2", "--cycles", "3", "--preview-ms", "5",
    "--report", $AcceptReportPath
)
Assert-True ($acceptSoakRes.StdOut -match "(?m)^status=0\r?$") "ir_capture_accept soak failed status=0 (got: $($acceptSoakRes.StdOut))"
Assert-True ($acceptSoakRes.StdOut -match "cycles_completed=3 total_frames=6") "ir_capture_accept soak frame count mismatch"
Assert-True ($acceptSoakRes.StdOut -match "preview_errors=0") "ir_capture_accept soak preview errors"
$acceptReportJson = [System.IO.File]::ReadAllText($AcceptReportPath, $Utf8NoBom) | ConvertFrom-Json
Assert-True ($acceptReportJson.cycles_completed -eq 3 -and $acceptReportJson.total_frames -eq 6) "ir_capture_accept report JSON mismatch"
Write-Host "  CLI PASS, ROI 2x2, NG (exit 20), ERROR, and ir_capture_accept verified."

Write-Step "11. Running packaged Python ctypes examples (ir_noise.py and ir_recipe.py)"
$PyNoiseOut = Join-Path $WorkRoot "py-noise-out"
$pyNoiseRes = Invoke-External -FilePath $pythonPath `
    -Arguments @((Join-Path $PkgRoot "examples\python\ir_noise.py"),
                 (Join-Path $PkgRoot "examples\data\raw16"),
                 "--output", $PyNoiseOut)
Assert-True ($pyNoiseRes.StdOut -match "status=0 completed=2" -and $pyNoiseRes.StdOut -match "mean_dn=21") "ir_noise.py output mismatch"
$pyNoiseJson = [System.IO.File]::ReadAllText((Join-Path $PyNoiseOut "result.json"), $Utf8NoBom) | ConvertFrom-Json
Assert-TraceabilityFields $pyNoiseJson $SdkVersion "ir_noise.py"

$PyRecipeOut = Join-Path $WorkRoot "py-recipe-out"
$pyCheckRes = Invoke-External -FilePath $pythonPath `
    -Arguments @((Join-Path $PkgRoot "examples\python\ir_recipe.py"),
                 "--check", $OfflineStation, $OfflineRecipe, $PyRecipeOut)
Assert-True ($pyCheckRes.StdOut.Trim() -eq "recipe=VALID") "ir_recipe.py --check failed"
$pyRunRes = Invoke-External -FilePath $pythonPath `
    -Arguments @((Join-Path $PkgRoot "examples\python\ir_recipe.py"),
                 "--run", $OfflineStation, $OfflineRecipe, $PyRecipeOut)
Assert-True ($pyRunRes.StdOut -match "(?m)^run_status=PASS\r?$") "ir_recipe.py --run failed (got: $($pyRunRes.StdOut))"
$pyAnalysisRes = Invoke-External -FilePath $pythonPath `
    -Arguments @("-B", (Join-Path $PkgRoot "examples\python\ir_analysis.py"))
Assert-True ($pyAnalysisRes.StdOut -match "(?m)^analysis_version=$([regex]::Escape($SdkVersion))\r?$") "ir_analysis.py missing analysis_version"
Assert-True ($pyAnalysisRes.StdOut -match "(?m)^gb_grade=A\r?$") "ir_analysis.py missing gb_grade=A"
Assert-True ($pyAnalysisRes.StdOut -match "(?m)^gjb_bad_count=0\r?$") "ir_analysis.py missing gjb_bad_count=0"
Assert-True ($pyAnalysisRes.StdOut -match "(?m)^noise3d_status=0\r?$") "ir_analysis.py missing noise3d_status=0"
Assert-True ($pyAnalysisRes.StdOut -match "(?m)^lrnu_ok=1\r?$") "ir_analysis.py missing lrnu_ok=1"
Assert-True ($pyAnalysisRes.StdOut -match "(?m)^fft_ok=1\r?$") "ir_analysis.py missing fft_ok=1"
Assert-True ($pyAnalysisRes.StdOut -match "(?m)^gaussian_ok=1\r?$") "ir_analysis.py missing gaussian_ok=1"
Assert-True ($pyAnalysisRes.StdOut -match "(?m)^flicker_ok=1\r?$") "ir_analysis.py missing flicker_ok=1"
Assert-True ($pyAnalysisRes.StdOut -match "(?m)^fft2d_ok=1\r?$") "ir_analysis.py missing fft2d_ok=1"
$PyDriftOut = Join-Path $WorkRoot "py-drift-out"
$pyDriftRes = Invoke-External -FilePath $pythonPath `
    -Arguments @("-B", (Join-Path $PkgRoot "examples\python\ir_drift_nodisk.py"),
                 "--mode", "self-test", "--output-dir", $PyDriftOut)
Assert-True ($pyDriftRes.StdOut -match "(?m)^raw_saved=false\r?$") "ir_drift_nodisk.py missing raw_saved=false"
Assert-True ($pyDriftRes.StdOut -match "(?m)^burst_count=3\r?$") "ir_drift_nodisk.py missing burst_count=3"
Write-Host "  Python ir_noise.py, ir_recipe.py, ir_analysis.py, and ir_drift_nodisk.py verified."

Write-Step "12. Running TCP simulator fault-injection smoke tests (--drop-command --fragment --log-io + --unstable)"
$SimScript = Join-Path $PkgRoot "simulators\ir_sim_devices.py"
$BbRecipe = Join-Path $PkgRoot "examples\recipes\noise_blackbody.json"

function Read-TextShared([string]$Path) {
    if (-not (Test-Path $Path)) { return "" }
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $sr = New-Object System.IO.StreamReader($fs, $Utf8NoBom)
        try {
            return $sr.ReadToEnd()
        } finally {
            $sr.Dispose()
        }
    } finally {
        $fs.Dispose()
    }
}

function Start-SimAndWait([string[]]$SimArgs, [string]$LogOut, [string]$LogErr) {
    $argString = (($SimArgs | ForEach-Object { Format-WinArg $_ }) -join " ")
    $proc = Start-Process -FilePath $pythonPath -ArgumentList $argString `
        -RedirectStandardOutput $LogOut -RedirectStandardError $LogErr `
        -NoNewWindow -PassThru
    $deadline = (Get-Date).AddSeconds(6)
    while ((Get-Date) -lt $deadline) {
        if ($proc.HasExited) {
            $errText = Read-TextShared $LogErr
            throw "Simulator exited early (code=$($proc.ExitCode)): $errText"
        }
        $content = Read-TextShared $LogOut
        if ($content -match "TCP simulator listening") {
            return $proc
        }
        Start-Sleep -Milliseconds 50
    }
    try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch {}
    throw "Timed out waiting for TCP simulator to start"
}

$simPort = Get-FreeTcpPort
$simStationObj = [System.IO.File]::ReadAllText((Join-Path $PkgRoot "examples\stations\sim_tcp.json"), $Utf8NoBom) | ConvertFrom-Json
$simStationObj.acquisition.source = (Join-Path $PkgRoot "examples\data\raw16")
$simStationObj.devices.bb.connection.tcp_port = $simPort
$SimStationPath = Join-Path $TempRecipesDir "sim_tcp_verify.json"
Write-Utf8File $SimStationPath ($simStationObj | ConvertTo-Json -Depth 10)

$simLogOut = Join-Path $WorkRoot "sim-fault-stdout.log"
$simLogErr = Join-Path $WorkRoot "sim-fault-stderr.log"
$simProc = Start-SimAndWait @(
    "-u", $SimScript, "blackbody-tcp",
    "--host", "127.0.0.1", "--port", "$simPort",
    "--drop-command", "114", "--drop-count", "1",
    "--fragment", "--log-io"
) $simLogOut $simLogErr
try {
    $TcpPassOut = Join-Path $WorkRoot "cli-tcp-fault-pass"
    $tcpPassRes = Invoke-External -FilePath $PkgCli `
        -Arguments @("--run", "--station", $SimStationPath, "--recipe", $BbRecipe, "--output", $TcpPassOut)
    Assert-True ($tcpPassRes.StdOut -match "(?m)^run_status=PASS\r?$") "TCP fault-injection recovery did not PASS (got: $($tcpPassRes.StdOut))"
} finally {
    if ($simProc -and -not $simProc.HasExited) {
        Stop-Process -Id $simProc.Id -Force -ErrorAction SilentlyContinue
        $simProc.WaitForExit(3000) | Out-Null
    }
}
$simOutText = [System.IO.File]::ReadAllText($simLogOut, $Utf8NoBom)
Assert-True ($simOutText -match '\[DROP\]') "Simulator log did not record [DROP] fault injection"
Assert-True ($simOutText -match '\[RX\]' -and $simOutText -match '\[TX\]') "Simulator log missing [RX]/[TX]"
Write-Host "  TCP blackbody simulator (--drop-command 114 --drop-count 1 --fragment --log-io) recovered and PASSED."

$simPort2 = Get-FreeTcpPort
$simStationObj.devices.bb.connection.tcp_port = $simPort2
$SimStationPath2 = Join-Path $TempRecipesDir "sim_tcp_unstable.json"
Write-Utf8File $SimStationPath2 ($simStationObj | ConvertTo-Json -Depth 10)
$shortBbRecipeObj = [System.IO.File]::ReadAllText($BbRecipe, $Utf8NoBom) | ConvertFrom-Json
$shortBbRecipeObj.steps[0].timeout_ms = 500
$ShortBbRecipePath = Join-Path $TempRecipesDir "noise_blackbody_short_timeout.json"
Write-Utf8File $ShortBbRecipePath ($shortBbRecipeObj | ConvertTo-Json -Depth 10)

$simLogOut2 = Join-Path $WorkRoot "sim-unstable-stdout.log"
$simLogErr2 = Join-Path $WorkRoot "sim-unstable-stderr.log"
$simProc2 = Start-SimAndWait @(
    "-u", $SimScript, "blackbody-tcp",
    "--host", "127.0.0.1", "--port", "$simPort2",
    "--unstable"
) $simLogOut2 $simLogErr2
try {
    $TcpFailOut = Join-Path $WorkRoot "cli-tcp-unstable-fail"
    $tcpFailRes = Invoke-External -FilePath $PkgCli `
        -Arguments @("--run", "--station", $SimStationPath2, "--recipe", $ShortBbRecipePath, "--output", $TcpFailOut) `
        -AllowedExitCodes @(7)
    Assert-True ($tcpFailRes.ExitCode -eq 7) "Unstable blackbody expected exit 7 (IR_TIMEOUT)"
    Assert-True ($tcpFailRes.StdOut -match "(?m)^run_status=ERROR\r?$") "Unstable blackbody missing run_status=ERROR (got: $($tcpFailRes.StdOut))"
    Assert-True (-not (Test-Path $TcpFailOut)) "Unstable blackbody failure must not create output directory"
} finally {
    if ($simProc2 -and -not $simProc2.HasExited) {
        Stop-Process -Id $simProc2.Id -Force -ErrorAction SilentlyContinue
        $simProc2.WaitForExit(3000) | Out-Null
    }
}
Write-Host "  TCP blackbody simulator (--unstable) timeout failure verified (exit 7 + run_status=ERROR)."

Write-Step "12b. Running live CTRL_BREAK_EVENT cancellation test against TCP simulator"
$simPort3 = Get-FreeTcpPort
$simStationObj.devices.bb.connection.tcp_port = $simPort3
$SimStationPath3 = Join-Path $TempRecipesDir "sim_tcp_cancel.json"
Write-Utf8File $SimStationPath3 ($simStationObj | ConvertTo-Json -Depth 10)
$cancelBbRecipeObj = [System.IO.File]::ReadAllText($BbRecipe, $Utf8NoBom) | ConvertFrom-Json
$cancelBbRecipeObj.steps[0].stable_window_ms = 2000
$cancelBbRecipeObj.steps[0].timeout_ms = 15000
$CancelBbRecipePath = Join-Path $TempRecipesDir "noise_blackbody_cancel.json"
Write-Utf8File $CancelBbRecipePath ($cancelBbRecipeObj | ConvertTo-Json -Depth 10)

$simLogOut3 = Join-Path $WorkRoot "sim-cancel-stdout.log"
$simLogErr3 = Join-Path $WorkRoot "sim-cancel-stderr.log"
$CancelOut = Join-Path $WorkRoot "cli-tcp-cancel-out"
$simProc3 = Start-SimAndWait @(
    "-u", $SimScript, "blackbody-tcp",
    "--host", "127.0.0.1", "--port", "$simPort3",
    "--unstable", "--log-io"
) $simLogOut3 $simLogErr3
try {
    $cancelPy = @'
import json, pathlib, signal, subprocess, sys, time
cli, station, recipe, out_dir, sim_log = sys.argv[1:6]
proc = subprocess.Popen(
    [cli, "--run", "--station", station, "--recipe", recipe, "--output", out_dir],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
)
deadline = time.monotonic() + 8.0
seen_rx = False
while time.monotonic() < deadline:
    if proc.poll() is not None:
        break
    log_path = pathlib.Path(sim_log)
    if log_path.exists() and "[RX]" in log_path.read_text(encoding="utf-8", errors="replace"):
        seen_rx = True
        break
    time.sleep(0.05)
if not seen_rx:
    proc.kill()
    stdout, stderr = proc.communicate()
    raise RuntimeError(f"CLI did not reach blackbody polling before break (rc={proc.returncode}, out={stdout!r}, err={stderr!r})")
time.sleep(0.1)
proc.send_signal(signal.CTRL_BREAK_EVENT)
stdout, stderr = proc.communicate(timeout=8)
print(json.dumps({
    "returncode": proc.returncode,
    "stdout": stdout.decode("utf-8", errors="replace"),
    "stderr": stderr.decode("utf-8", errors="replace"),
}))
'@
    $cancelPyPath = Join-Path $TempRecipesDir "run_ctrl_break.py"
    Write-Utf8File $cancelPyPath $cancelPy
    $cancelRes = Invoke-External -FilePath $pythonPath `
        -Arguments @("-B", $cancelPyPath, $PkgCli, $SimStationPath3, $CancelBbRecipePath, $CancelOut, $simLogOut3)
    $cancelJson = $cancelRes.StdOut.Trim() | ConvertFrom-Json
    Assert-True ($cancelJson.returncode -eq 8) "CTRL_BREAK_EVENT expected exit 8 (IR_CANCELLED), got $($cancelJson.returncode)`nSTDERR: $($cancelJson.stderr)"
    Assert-True ($cancelJson.stdout -match "(?m)^run_status=CANCELLED\r?$") "CTRL_BREAK_EVENT missing run_status=CANCELLED (got: $($cancelJson.stdout))"
    Assert-True ($cancelJson.stdout -match "(?m)^completed_frames=0\r?$") "CTRL_BREAK_EVENT expected completed_frames=0 (got: $($cancelJson.stdout))"
    Assert-True ($cancelJson.stderr -match "status=8" -and $cancelJson.stderr -match "cleanup_status=0") "CTRL_BREAK_EVENT stderr missing diagnostics: $($cancelJson.stderr)"
    Assert-True (-not (Test-Path $CancelOut)) "Cancelled CLI run must not create output directory"
} finally {
    if ($simProc3 -and -not $simProc3.HasExited) {
        Stop-Process -Id $simProc3.Id -Force -ErrorAction SilentlyContinue
        $simProc3.WaitForExit(3000) | Out-Null
    }
}
Write-Host "  Live CTRL_BREAK_EVENT cancellation verified (exit 8 + run_status=CANCELLED + cleanup_status=0)."

Write-Step "12c. Validating JSON Schemas (Draft 2020-12) and bundled templates"
$schemaCheckPy = @'
import json, pathlib, sys
pkg = pathlib.Path(sys.argv[1]).resolve()
station_schema = json.loads((pkg / "examples/schemas/station.schema.json").read_text(encoding="utf-8"))
recipe_schema = json.loads((pkg / "examples/schemas/recipe.schema.json").read_text(encoding="utf-8"))
recipe_v2_schema = json.loads((pkg / "examples/schemas/recipe.v2.schema.json").read_text(encoding="utf-8"))
stations = sorted((pkg / "examples/stations").glob("*.json"))
recipes = sorted((pkg / "examples/recipes").glob("*.json"))
assert len(stations) == 5 and len(recipes) == 7, (len(stations), len(recipes))
for p in [*stations, *recipes]:
    data = json.loads(p.read_text(encoding="utf-8"))
    ref = (p.parent / data["$schema"]).resolve()
    assert ref.is_relative_to(pkg) and ref.is_file(), (p, ref)
try:
    from jsonschema import Draft202012Validator, exceptions as jex
except ImportError:
    print("mode=json-parse-only (python jsonschema not installed)")
    sys.exit(0)
Draft202012Validator.check_schema(station_schema)
Draft202012Validator.check_schema(recipe_schema)
Draft202012Validator.check_schema(recipe_v2_schema)
sv = Draft202012Validator(station_schema)
rv = Draft202012Validator(recipe_schema)
rv2 = Draft202012Validator(recipe_v2_schema)
for p in stations:
    sv.validate(json.loads(p.read_text(encoding="utf-8")))
for p in recipes:
    data = json.loads(p.read_text(encoding="utf-8"))
    (rv2 if data["schema_version"] == 2 else rv).validate(data)

# Negative boundary checks
bad_stations = [
    {"schema_version": 1, "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16},
     "devices": {"bb": {"kind": "pcn7-lt", "connection": {"host": "127.0.0.1", "expected_head_serial": "H"}}}},
    {"schema_version": 1, "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16},
     "devices": {"bb": {"kind": "pcn7-lt", "connection": {"host": "127.0.0.1", "head": -1, "expected_head_serial": "H"}}}},
    {"schema_version": 1, "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16, "max_batch_bytes": 2**63}},
    {"schema_version": 1, "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16, "queue_capacity": 1}},
    {"schema_version": 1, "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16, "queue_capacity": 5000}},
    {"schema_version": 1, "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16},
     "devices": {"ov": {"kind": "gws-sten", "connection": {"host": "127.0.0.1", "port": "COM3"}}}},
    {"schema_version": 1, "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16},
     "devices": {"dm": {"kind": "dmm6500", "connection": {"host": "127.0.0.1", "port": "COM4"}}}},
    {"schema_version": 1, "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16},
     "devices": {"dt": {"kind": "detector-serial", "connection": {"host": "127.0.0.1", "port": "COM5"}}}},
    {"schema_version": 1, "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16},
     "devices": {"ov": {"kind": "gws-sten", "connection": {"host": "127.0.0.1", "delimiter": "tab"}}}},
    {"schema_version": 1, "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16},
     "devices": {"pw": {"kind": "it6332a", "connection": {"host": "127.0.0.1", "port": "COM8"}}}},
    {"schema_version": 1, "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16},
     "devices": {"cl": {"kind": "cooler-modbus", "connection": {"host": "127.0.0.1", "port": "COM9"}}}},
    {"schema_version": 1, "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16},
     "devices": {"cl": {"kind": "cooler-modbus", "connection": {"host": "127.0.0.1", "slave_address": 250}}}},
]
for idx, s in enumerate(bad_stations):
    assert not sv.is_valid(s), f"bad station #{idx} unexpectedly validated"

good_stations_extra = [
    {
        "schema_version": 1,
        "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16},
        "devices": {
            "pw": {"kind": "it6332a", "connection": {"port": "COM8", "baud_rate": 115200}},
            "ov": {"kind": "gws-sten", "connection": {"port": "COM3", "baud_rate": 9600, "data_bits": 8, "parity": "none", "stop_bits": 1, "address": 1, "delimiter": "crlf"}},
            "dm": {"kind": "dmm6500", "connection": {"port": "COM4", "baud_rate": 115200}},
            "dt": {"kind": "detector-serial", "connection": {"port": "COM5", "baud_rate": 115200, "inter_command_ms": 5}},
            "cl": {"kind": "cooler-modbus", "connection": {"port": "COM9", "baud_rate": 9600, "stop_bits": 2, "slave_address": 1}},
        },
    },
    {
        "schema_version": 1,
        "acquisition": {"backend": "raw_replay", "source": "x", "valid_bits": 16},
        "devices": {
            "pw": {"kind": "it6332a", "connection": {"host": "127.0.0.1", "tcp_port": 5026}},
            "ov": {"kind": "gws-sten", "connection": {"host": "127.0.0.1", "tcp_port": 5000}},
            "dm": {"kind": "dmm6500", "connection": {"host": "127.0.0.1", "tcp_port": 5025}},
            "dt": {"kind": "detector-serial", "connection": {"host": "127.0.0.1", "tcp_port": 2102}},
            "cl": {"kind": "cooler-modbus", "connection": {"host": "127.0.0.1", "tcp_port": 5020, "slave_address": 1}},
        },
    },
]
for idx, s in enumerate(good_stations_extra):
    sv.validate(s)

good_recipes_v2_extra = [
    {
        "schema_version": 2,
        "recipe_id": "v2-instruments-schema-check",
        "steps": [
            {"id": "pw1", "type": "power_set", "device": "pw", "channel": 1, "voltage_v": 12.0, "current_limit_a": 2.0},
            {"id": "pw2", "type": "power_output", "device": "pw", "channel": 1, "enabled": True, "off_after_run": True},
            {"id": "pws", "type": "power_wait_stable", "device": "pw", "channel": 1, "expected_voltage_v": 12.0, "voltage_tolerance_v": 0.2, "max_current_a": 2.02, "startup_threshold_a": 0.3, "startup_required_samples": 3, "startup_timeout_ms": 5000, "stable_threshold_a": 0.3, "max_variation_a": 0.02, "stable_required_samples": 3, "sample_interval_ms": 10, "timeout_ms": 10000},
            {"id": "ov1", "type": "oven_stabilize", "device": "ov", "target_c": 25.0, "tolerance_c": 0.5, "stable_window_ms": 100, "timeout_ms": 5000, "require_no_alarm": True, "standby_after_run": True},
            {"id": "dt1", "type": "detector_timing", "device": "dt", "integration_us": 1000, "frame_hz": 50, "empty_us": 200},
            {"id": "dg1", "type": "detector_gpol", "device": "dt", "gpol_v": 2.5},
            {"id": "dr1", "type": "detector_register", "device": "dt", "command_code": 101, "payload_hex": "08"},
            {"id": "dc1", "type": "detector_current", "device": "dt", "name": "dc", "gain_code": 0, "channel_code": 1},
            {"id": "dm1", "type": "dmm_read", "device": "dm", "name": "dmm", "mode": "diode", "set_mode": True},
            {"id": "cl1", "type": "cooler_read", "device": "cl", "name": "cooler"},
            {"id": "c1", "type": "capture", "frames": 2, "frame_timeout_ms": 0, "output": "b0"},
            {"id": "fs1", "type": "analyze", "method": "frame_stats", "input": "b0", "outputs": {"mean_map": "m0"}},
            {"id": "fft1", "type": "analyze", "method": "fft2d", "input": "m0", "params": {"window": "rect", "detrend_mean": False, "shift_dc_center": True}},
            {"id": "ov2", "type": "oven_standby", "device": "ov"},
        ],
        "safety": {"oven_standby": ["ov"]},
        "limits": [
            {"metric": "dmm.value", "operator": "max", "value": 0.8, "unit": "V"},
            {"metric": "cooler.voltage_mv", "operator": "min", "value": 500.0, "unit": "mV"},
        ],
    }
]
for r in good_recipes_v2_extra:
    rv2.validate(r)

bad_recipes_v1 = [
    {"schema_version": 1, "recipe_id": "bad", "steps": [
        {"id": "s1", "type": "save_raw", "input": "b"},
        {"id": "s2", "type": "save_raw", "input": "b"},
        {"id": "s3", "type": "save_raw", "input": "b"},
    ]},
    {"schema_version": 1, "recipe_id": "bad", "steps": [
        {"id": "b", "type": "capture", "frames": 3000000000, "frame_timeout_ms": 0},
        {"id": "n", "type": "analyze", "method": "temporal_noise", "name": "n", "input": "b"},
        {"id": "s", "type": "save_raw", "input": "b"},
    ]},
    {"schema_version": 1, "recipe_id": "bad", "steps": [
        {"id": "p", "type": "power_output", "device": "supply", "channel": 1, "enabled": True},
        {"id": "b", "type": "capture", "frames": 2, "frame_timeout_ms": 0},
        {"id": "n", "type": "analyze", "method": "temporal_noise", "name": "n", "input": "b"},
        {"id": "s", "type": "save_raw", "input": "b"},
    ]},
]
for idx, r in enumerate(bad_recipes_v1):
    assert not rv.is_valid(r), f"bad recipe v1 #{idx} unexpectedly validated"

bad_recipes_v2 = [
    {"schema_version": 2, "recipe_id": "bad-nested", "steps": [
        {"id": "lp", "type": "repeat_burst", "max_iterations": 2, "steps": [
            {"id": "lp2", "type": "repeat_burst", "max_iterations": 1, "steps": [
                {"id": "c", "type": "capture", "frames": 2, "frame_timeout_ms": 0}
            ]}
        ]}
    ]},
    {"schema_version": 2, "recipe_id": "bad-subdir", "steps": [
        {"id": "c", "type": "capture", "frames": 2, "frame_timeout_ms": 0},
        {"id": "s", "type": "save_raw", "input": "c", "subdir": "a/b"}
    ]},
    {"schema_version": 2, "recipe_id": "bad-power", "steps": [
        {"id": "p", "type": "power_output", "device": "supply", "channel": 1, "enabled": True},
        {"id": "c", "type": "capture", "frames": 2, "frame_timeout_ms": 0}
    ]},
    {"schema_version": 2, "recipe_id": "bad-dmm-any-set", "steps": [
        {"id": "dm", "type": "dmm_read", "device": "dm", "mode": "any", "set_mode": True}
    ]},
    {"schema_version": 2, "recipe_id": "bad-gpol-high", "steps": [
        {"id": "dg", "type": "detector_gpol", "device": "dt", "gpol_v": 5.5}
    ]},
    {"schema_version": 2, "recipe_id": "bad-oven-missing-tol", "steps": [
        {"id": "ov", "type": "oven_stabilize", "device": "ov", "target_c": 25.0, "stable_window_ms": 100, "timeout_ms": 1000}
    ]},
    {"schema_version": 2, "recipe_id": "bad-safety-oven", "steps": [
        {"id": "c", "type": "capture", "frames": 2, "frame_timeout_ms": 0}
    ], "safety": {"oven_standby": [123]}},
    {"schema_version": 2, "recipe_id": "bad-reg-hex", "steps": [
        {"id": "dr", "type": "detector_register", "device": "dt", "command_code": 101, "payload_hex": "0"}
    ]},
    {"schema_version": 2, "recipe_id": "bad-curr-gain", "steps": [
        {"id": "dc", "type": "detector_current", "device": "dt", "gain_code": 8, "channel_code": 1}
    ]},
    {"schema_version": 2, "recipe_id": "bad-power-wait-zero-thresh", "steps": [
        {"id": "pws", "type": "power_wait_stable", "device": "supply", "channel": 1, "stable_threshold_a": 0.0}
    ]},
    {"schema_version": 2, "recipe_id": "bad-power-wait-zero-timeout", "steps": [
        {"id": "pws", "type": "power_wait_stable", "device": "supply", "channel": 1, "timeout_ms": 0}
    ]},
    {"schema_version": 2, "recipe_id": "bad-analyze-method", "steps": [
        {"id": "fft", "type": "analyze", "method": "fft3d", "input": "m0"}
    ]},
]
for idx, r in enumerate(bad_recipes_v2):
    assert not rv2.is_valid(r), f"bad recipe v2 #{idx} unexpectedly validated"

print(f"mode=Draft202012Validator stations={len(stations)+len(good_stations_extra)} recipes={len(recipes)+len(good_recipes_v2_extra)} negatives={len(bad_stations)+len(bad_recipes_v1)+len(bad_recipes_v2)}")
'@
$schemaPyPath = Join-Path $TempRecipesDir "check_schemas.py"
Write-Utf8File $schemaPyPath $schemaCheckPy
$schemaRes = Invoke-External -FilePath $pythonPath -Arguments @("-B", $schemaPyPath, $PkgRoot)
Write-Host ("  JSON Schema check passed ({0})." -f $schemaRes.StdOut.Trim())

Write-Step "13. Checking optional C# sample (dotnet)"
$dotnetCandidates = New-Object System.Collections.Generic.List[string]
$dotnetInPath = Get-Command dotnet -ErrorAction SilentlyContinue
if ($dotnetInPath -and ($dotnetInPath.Source -notmatch "(?i)\\Program Files \(x86\)\\")) {
    [void]$dotnetCandidates.Add($dotnetInPath.Source)
}
foreach ($cand in @(
    "C:\Program Files\dotnet\dotnet.exe",
    (Join-Path $env:LOCALAPPDATA "Microsoft\dotnet\dotnet.exe"),
    (Join-Path $env:USERPROFILE ".dotnet\dotnet.exe")
)) {
    if ($cand -and -not ($dotnetCandidates -contains $cand)) {
        [void]$dotnetCandidates.Add($cand)
    }
}
$dotnetExe = $null
foreach ($cand in $dotnetCandidates) {
    if (Test-Path $cand) {
        try {
            $sdkList = (& $cand --list-sdks 2>$null | Out-String)
            if ($sdkList -match "(?m)^10\.") {
                $dotnetExe = $cand
                break
            }
        } catch {}
    }
}
if (-not $dotnetExe) {
    Write-Host "  [SKIP] .NET 10 SDK (x64 dotnet) is not installed on this machine; skipping C# sample." -ForegroundColor Yellow
} else {
    $dotnetVer = (& $dotnetExe --version).Trim()
    $csEnv = @{
        IR_SDK_LIB_DIR                    = (Join-Path $PkgRoot "windows-x64\bin")
        DOTNET_ROOT                       = (Split-Path -Parent $dotnetExe)
        DOTNET_NOLOGO                     = "1"
        DOTNET_CLI_TELEMETRY_OPTOUT       = "1"
        DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"
    }
    $CsProj = Join-Path $PkgRoot "examples\csharp\IRSdkSample.csproj"
    $null = Invoke-External -FilePath $dotnetExe -WorkingDirectory $PkgRoot `
        -Environment $csEnv -Arguments @("build", $CsProj, "--nologo")

    $csRawRes = Invoke-External -FilePath $dotnetExe -WorkingDirectory $PkgRoot `
        -Environment $csEnv -Arguments @("run", "--no-build", "--project", $CsProj, "--", (Join-Path $PkgRoot "examples\data\raw16"))
    Assert-True ($csRawRes.StdOut -match "status=0 completed=2") "C# raw16 sample failed status/completed (got: $($csRawRes.StdOut))"
    Assert-True ($csRawRes.StdOut -match "(?m)^mean_dn=21\r?$") "C# raw16 sample missing mean_dn=21 (got: $($csRawRes.StdOut))"
    Assert-True ($csRawRes.StdOut -match "frames_delivered=2") "C# raw16 sample missing frames_delivered=2 (got: $($csRawRes.StdOut))"

    $CsOut = Join-Path $WorkRoot "csharp-recipe-out"
    $csRes = Invoke-External -FilePath $dotnetExe -WorkingDirectory $PkgRoot `
        -Environment $csEnv -Arguments @("run", "--no-build", "--project", $CsProj,
            "--", "--recipe", $OfflineStation, $OfflineRecipe, $CsOut)
    Assert-True ($csRes.StdOut -match "(?m)^run_status=PASS\r?$") "C# v1 recipe sample failed (got: $($csRes.StdOut))"
    Assert-True ($csRes.StdOut -match "(?m)^completed_frames=2\r?$") "C# v1 recipe sample completed_frames!=2 (got: $($csRes.StdOut))"

    $CsV2Out = Join-Path $WorkRoot "csharp-v2-nodisk-out"
    $csV2Res = Invoke-External -FilePath $dotnetExe -WorkingDirectory $PkgRoot `
        -Environment $csEnv -Arguments @("run", "--no-build", "--project", $CsProj,
            "--", "--recipe", (Join-Path $PkgRoot "examples\stations\roi_2x2.json"),
            (Join-Path $PkgRoot "examples\recipes\nodisk_roi_v2.json"), $CsV2Out)
    Assert-True ($csV2Res.StdOut -match "(?m)^run_status=PASS\r?$") "C# v2 no-disk recipe sample failed (got: $($csV2Res.StdOut))"
    Assert-True ($csV2Res.StdOut -match "(?m)^completed_frames=2\r?$") "C# v2 no-disk recipe completed_frames!=2 (got: $($csV2Res.StdOut))"
    $csV2RawFiles = @(Get-ChildItem -Path $CsV2Out -Recurse -Filter "*.raw")
    Assert-True ($csV2RawFiles.Count -eq 0) "C# v2 no-disk sample must not save .raw files"

    $csAnalysisRes = Invoke-External -FilePath $dotnetExe -WorkingDirectory $PkgRoot `
        -Environment $csEnv -Arguments @("run", "--no-build", "--project", $CsProj, "--", "--analysis")
    Assert-True ($csAnalysisRes.StdOut -match "(?m)^analysis_version=$([regex]::Escape($SdkVersion))\r?$") "C# analysis sample failed version (got: $($csAnalysisRes.StdOut))"
    Assert-True ($csAnalysisRes.StdOut -match "(?m)^gb_grade=1\r?$") "C# analysis sample failed gb_grade (got: $($csAnalysisRes.StdOut))"
    Assert-True ($csAnalysisRes.StdOut -match "(?m)^gjb_bad_count=0\r?$") "C# analysis sample failed gjb_bad_count (got: $($csAnalysisRes.StdOut))"
    Assert-True ($csAnalysisRes.StdOut -match "(?m)^fft_ok=1\r?$") "C# analysis sample failed fft_ok (got: $($csAnalysisRes.StdOut))"
    Assert-True ($csAnalysisRes.StdOut -match "(?m)^gaussian_ok=1\r?$") "C# analysis sample failed gaussian_ok (got: $($csAnalysisRes.StdOut))"
    Assert-True ($csAnalysisRes.StdOut -match "(?m)^flicker_ok=1\r?$") "C# analysis sample failed flicker_ok (got: $($csAnalysisRes.StdOut))"
    Assert-True ($csAnalysisRes.StdOut -match "(?m)^fft2d_ok=1\r?$") "C# analysis sample failed fft2d_ok (got: $($csAnalysisRes.StdOut))"
    Write-Host "  C# sample verified (dotnet ${dotnetVer}: raw16, v1 recipe, v2 no-disk recipe, analysis)."
}

if ($RunCom0ComHil) {
    Write-Step "14. Running live 6-pair com0com HIL verification"
    $hilMapPath = Join-Path $ExperimentDir "hil_serial_map.json"
    Assert-True (Test-Path $hilMapPath) "hil_serial_map.json not found at $hilMapPath"
    $hilArgs = @(
        "-B",
        (Join-Path $RepoRoot "tools\verify_com0com_hil.py"),
        "--station-lib", $StationDll,
        "--cli", $PkgCli,
        "--serial-map", $hilMapPath,
        "--csharp-project", (Join-Path $PkgRoot "examples\csharp\IRSdkSample.csproj"),
        "--python-recipe", (Join-Path $PkgRoot "examples\python\ir_recipe.py"),
        "--work-parent", $WorkRoot
    )
    if ($dotnetExe) {
        $hilArgs += @("--dotnet", $dotnetExe)
    }
    $hilRes = Invoke-External -FilePath $pythonPath -Arguments $hilArgs
    Assert-True ($hilRes.StdOut -match "(?m)^COM0COM_HIL=PASS\r?$") "Live com0com HIL missing COM0COM_HIL=PASS (got: $($hilRes.StdOut))"
    Write-Host ($hilRes.StdOut.Trim())
}

Write-Step "ALL WINDOWS VERIFICATION CHECKS PASSED"
Write-Host "  Package : $ZipPath"
Write-Host "  SHA-256 : $actualSha"
Write-Host "  Extract : $PkgRoot"
