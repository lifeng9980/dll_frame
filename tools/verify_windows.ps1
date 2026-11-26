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

Write-Step "2. Configuring and building Debug via CMake windows preset"
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

$BuildDir = Join-Path $ExperimentDir "test"
$BuildBinDir = Join-Path $BuildDir "bin"
$StationDll = Join-Path $BuildBinDir "IR_station.dll"
$AnalysisDll = Join-Path $BuildBinDir "IR_analysis.dll"
$CliExe = Join-Path $BuildBinDir "ir_run_recipe.exe"
$AcceptExe = Join-Path $BuildBinDir "ir_capture_accept.exe"


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
    "IR_WaitPowerCurrentStable",
    "IR_CheckRecipeJson",
    "IR_RunRecipeJson",
    "IR_RequestStop"
)) {
    Assert-True ($stationExports -match "(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+$([regex]::Escape($sym))(?:\s|$)") "IR_station.dll must export $sym"
}
$analysisExports = (Invoke-External -FilePath $dumpbinPath -Arguments @("/nologo", "/exports", $AnalysisDll)).StdOut
foreach ($sym in @(
    "IR_AnalysisVersion",
    "IR_FrameStatistics",
    "IR_ROIMean",
    "IR_Noise3DBuildCalibDn",
    "IR_Noise3DComputeFramesDn",
    "IR_GbProcessBadPixels",
    "IR_GbEvaluateMaps",
    "IR_GjbProcessBadPixels",
    "IR_GjbEvaluateMaps",
    "IR_BuildTwoPointNucF64",
    "IR_ApplyAffineMapF64",
    "IR_FrameStatisticsNuc",
    "IR_MapSummaryF64",
    "IR_SpatialLowHighPassF64",
    "IR_MapBinaryOpF64",
    "IR_ThresholdMaskF64",
    "IR_MaskCombine",
    "IR_ComputeRfft1D",
    "IR_ChannelSpectrum",
    "IR_GjbEvaluateBurstMetrics",
    "IR_FlickerDetect",
    "IR_Fft2dMagnitudeF64"
)) {
    Assert-True ($analysisExports -match "(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+$([regex]::Escape($sym))(?:\s|$)") "IR_analysis.dll must export $sym"
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
Assert-True (Test-Path (Join-Path $PkgRoot "include\IR_types.h")) "Missing include\IR_types.h in extracted package"
Assert-True (Test-Path (Join-Path $PkgRoot "include\IR_plugin_abi.h")) "Missing include\IR_plugin_abi.h in extracted package"
Assert-True (Test-Path (Join-Path $PkgRoot "include\IR_plugin_cxx.hpp")) "Missing include\IR_plugin_cxx.hpp in extracted package"
Write-Host "  Extracted and verified $checkedManifestFiles files under: $PkgRoot"

Write-Step "9. Building and running standalone C/C++ client examples from extracted package"
$ClientBuildDir = Join-Path $WorkRoot "client-build"
Invoke-External -FilePath $cmakePath -WorkingDirectory $PkgRoot `
    -Arguments @("-S", (Join-Path $PkgRoot "examples"), "-B", $ClientBuildDir,
                 "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug", "-DIR_SDK_ROOT=$PkgRoot") | Out-Null
Invoke-External -FilePath $cmakePath -WorkingDirectory $PkgRoot `
    -Arguments @("--build", $ClientBuildDir) | Out-Null

$cClientRes = Invoke-External -FilePath (Join-Path $ClientBuildDir "ir_c_client.exe") -Arguments @($SdkVersion)
Assert-True ($cClientRes.StdOut.Trim() -eq $SdkVersion) "ir_c_client output mismatch: $($cClientRes.StdOut)"
$null = Invoke-External -FilePath (Join-Path $ClientBuildDir "ir_cpp_client.exe")
$analysisCExe = Join-Path $ClientBuildDir "ir_analysis_c_sample.exe"
$analysisCDep = Invoke-External -FilePath $dumpbinPath -Arguments @("/nologo", "/dependents", $analysisCExe)
$analysisCDllLines = @($analysisCDep.StdOut -split "`r?`n" | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() })
Assert-True ($analysisCDllLines -contains "IR_analysis.dll") "ir_analysis_c_sample.exe must depend on IR_analysis.dll"
Assert-True (-not ($analysisCDllLines -contains "IR_station.dll")) "ir_analysis_c_sample.exe must not depend on IR_station.dll (got: $($analysisCDllLines -join ', '))"
$null = Invoke-External -FilePath $analysisCExe
$standaloneCliCheck = Invoke-External -FilePath (Join-Path $ClientBuildDir "ir_run_recipe.exe") `
    -Arguments @("--check", "--station", (Join-Path $PkgRoot "examples\stations\offline.json"),
                 "--recipe", (Join-Path $PkgRoot "examples\recipes\noise_roi.json"),
                 "--output", (Join-Path $WorkRoot "standalone-check-unused"))
Assert-True ($standaloneCliCheck.StdOut.Trim() -eq "recipe=VALID") "standalone ir_run_recipe --check failed"
$null = Invoke-External -FilePath (Join-Path $ClientBuildDir "ir_capture_accept.exe") -Arguments @("--list")
Write-Host "  Standalone ir_c_client, ir_cpp_client, ir_analysis_c_sample, ir_run_recipe, and ir_capture_accept verified."

Write-Step "10. Running packaged CLI smoke tests (PASS / NG / ERROR)"
$PkgCli = Join-Path $PkgRoot "windows-x64\bin\ir_run_recipe.exe"
$TempRecipesDir = Join-Path $WorkRoot "temp-recipes"
New-Item -ItemType Directory -Path $TempRecipesDir -Force | Out-Null

$OfflineStationObj = [System.IO.File]::ReadAllText((Join-Path $PkgRoot "examples\stations\offline.json"), $Utf8NoBom) | ConvertFrom-Json
$OfflineStationObj.acquisition.source = (Join-Path $RepoRoot "examples\data\tc002a_640")
$OfflineStation = Join-Path $TempRecipesDir "offline_tc002a.json"
Write-Utf8File $OfflineStation ($OfflineStationObj | ConvertTo-Json -Depth 10)
$OfflineRecipe = Join-Path $PkgRoot "examples\recipes\noise_roi.json"

$PassOut = Join-Path $WorkRoot "cli-pass-out"
$passRes = Invoke-External -FilePath $PkgCli -Arguments @("--run", "--station", $OfflineStation, "--recipe", $OfflineRecipe, "--output", $PassOut)
Assert-True ($passRes.StdOut -match "(?m)^run_status=PASS\r?$") "CLI PASS missing run_status=PASS (got: $($passRes.StdOut))"
$passJson = [System.IO.File]::ReadAllText((Join-Path $PassOut "result.json"), $Utf8NoBom) | ConvertFrom-Json
Assert-TraceabilityFields $passJson $SdkVersion "CLI PASS"

$ngRecipeObj = [System.IO.File]::ReadAllText($OfflineRecipe, $Utf8NoBom) | ConvertFrom-Json
$ngRecipeObj.limits[0].value = 1.0
$NgRecipePath = Join-Path $TempRecipesDir "noise_roi_ng.json"
Write-Utf8File $NgRecipePath ($ngRecipeObj | ConvertTo-Json -Depth 10)
$NgOut = Join-Path $WorkRoot "cli-ng-out"
$ngRes = Invoke-External -FilePath $PkgCli -Arguments @("--run", "--station", $OfflineStation, "--recipe", $NgRecipePath, "--output", $NgOut) -AllowedExitCodes @(20)
Assert-True ($ngRes.ExitCode -eq 20 -and $ngRes.StdOut -match "(?m)^run_status=NG\r?$") "CLI NG check failed"

$errRecipeObj = [System.IO.File]::ReadAllText($OfflineRecipe, $Utf8NoBom) | ConvertFrom-Json
$errRecipeObj.steps[0].frames = 101
$ErrRecipePath = Join-Path $TempRecipesDir "noise_roi_short_error.json"
Write-Utf8File $ErrRecipePath ($errRecipeObj | ConvertTo-Json -Depth 10)
$ErrOut = Join-Path $WorkRoot "cli-error-out"
$errRes = Invoke-External -FilePath $PkgCli -Arguments @("--run", "--station", $OfflineStation, "--recipe", $ErrRecipePath, "--output", $ErrOut) -AllowedExitCodes @(11)
Assert-True ($errRes.ExitCode -eq 11 -and $errRes.StdOut -match "(?m)^run_status=ERROR\r?$") "CLI ERROR check failed"
$errJson = [System.IO.File]::ReadAllText((Join-Path $ErrOut "result.json"), $Utf8NoBom) | ConvertFrom-Json
Assert-True ($errJson.run_status -eq "ERROR") "CLI ERROR diagnostic result.json status mismatch"
Write-Host "  Packaged CLI PASS, NG (exit 20), and ERROR diagnostic result.json verified."

Write-Step "11. Running packaged Python ctypes smoke test (ir_recipe.py)"
$PyRecipeOut = Join-Path $WorkRoot "py-recipe-out"
$pyRunRes = Invoke-External -FilePath $pythonPath `
    -Arguments @((Join-Path $PkgRoot "examples\python\ir_recipe.py"),
                 "--run", $OfflineStation, $OfflineRecipe, $PyRecipeOut)
Assert-True ($pyRunRes.StdOut -match "(?m)^run_status=PASS\r?$") "ir_recipe.py --run failed (got: $($pyRunRes.StdOut))"
Write-Host "  Python ir_recipe.py smoke verified."

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
    try { & taskkill.exe /PID $proc.Id /T /F *> $null } catch {}
    throw "Timed out waiting for TCP simulator to start"
}

function Stop-SimProcess($Proc) {
    if (-not $Proc) { return }
    try {
        if (-not $Proc.HasExited) {
            Invoke-External -FilePath "taskkill.exe" -Arguments @("/PID", "$($Proc.Id)", "/T", "/F") -AllowedExitCodes @(0, 128) | Out-Null
            $Proc.WaitForExit(3000) | Out-Null
        }
    } catch {
    } finally {
        try { $Proc.Dispose() } catch {}
    }
}

$simStationObj = [System.IO.File]::ReadAllText((Join-Path $PkgRoot "examples\stations\sim_tcp.json"), $Utf8NoBom) | ConvertFrom-Json
$simStationObj.acquisition.source = (Join-Path $RepoRoot "examples\data\tc002a_640")

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
    Assert-True (Test-Path (Join-Path $CancelOut "result.json")) "Cancelled CLI run must write diagnostic result.json"
    $cancelDiagJson = [System.IO.File]::ReadAllText((Join-Path $CancelOut "result.json"), $Utf8NoBom) | ConvertFrom-Json
    Assert-True ($cancelDiagJson.run_status -eq "CANCELLED") "Cancelled CLI diagnostic result.json run_status != CANCELLED"
} finally {
    Stop-SimProcess $simProc3
}
Write-Host "  Live CTRL_BREAK_EVENT cancellation verified (exit 8 + run_status=CANCELLED + cleanup_status=0)."

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
            if ($sdkList -match "(?m)^(10|1[1-9])\.") {
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

    $CsOut = Join-Path $WorkRoot "csharp-recipe-out"
    $csRes = Invoke-External -FilePath $dotnetExe -WorkingDirectory $PkgRoot `
        -Environment $csEnv -Arguments @("run", "--no-build", "--project", $CsProj,
            "--", "--recipe", $OfflineStation, $OfflineRecipe, $CsOut)
    Assert-True ($csRes.StdOut -match "(?m)^run_status=PASS\r?$") "C# recipe sample failed (got: $($csRes.StdOut))"

    $null = Invoke-External -FilePath $dotnetExe -WorkingDirectory $PkgRoot `
        -Environment $csEnv -Arguments @("run", "--no-build", "--project", $CsProj, "--", "--analysis")
    Write-Host "  C# sample verified (dotnet ${dotnetVer}: recipe + analysis)."
}

if ($RunCom0ComHil) {
    Write-Step "14. Running live 6-pair com0com HIL verification"
    $hilMapPath = Join-Path $ExperimentDir "hil_serial_map.json"
    Assert-True (Test-Path $hilMapPath) "hil_serial_map.json not found at $hilMapPath"
    $hilArgs = @(
        "-B",
        (Join-Path $RepoRoot "tools\verify_com0com_hil.py"),
        "--station-lib", (Join-Path $PkgRoot "windows-x64\bin\IR_station.dll"),
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
