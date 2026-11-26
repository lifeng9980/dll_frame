[CmdletBinding()]
param(
    [string]$WorkRoot = "",
    [ValidateSet("Debug", "Release")][string]$Configuration = "Debug",
    [switch]$RequireClean,
    [string]$PythonExe = "",
    [switch]$RunCom0ComHil
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
try {
    [Console]::OutputEncoding = $Utf8NoBom
    [Console]::InputEncoding = $Utf8NoBom
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
        # Preserve inherited empty values. Windows PowerShell's setter removes them.
        if ($line -match '^([^=]+)=(.+)$') {
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
$cmakeText = [System.IO.File]::ReadAllText((Join-Path $RepoRoot "CMakeLists.txt"), $Utf8NoBom)
if ($cmakeText -notmatch 'project\(IRSDK VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Cannot parse PROJECT_VERSION from CMakeLists.txt"
}
$SdkVersion = $matches[1]
$ExpectedFileVersion = "$SdkVersion.0"
$IsRelease = $Configuration -eq "Release"
$Preset = if ($IsRelease) { "windows-release" } else { "windows" }
$EnableCom0ComHil = if ($RunCom0ComHil) { "ON" } else { "OFF" }

Write-Step "1. Setting up MSVC x64, CMake, Ninja, and Python"
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

Write-Step "2. Configuring and building $Configuration via the $Preset preset"
$res = Invoke-External -FilePath $cmakePath -WorkingDirectory $RepoRoot `
    -Arguments @("--preset", $Preset, "-DBUILD_TESTING=ON", "-DPython3_EXECUTABLE=$pythonPath", `
                 "-DIR_ENABLE_COM0COM_HIL=$EnableCom0ComHil")
Write-Host $res.StdOut.Trim()
$res = Invoke-External -FilePath $cmakePath -WorkingDirectory $RepoRoot `
    -Arguments @("--build", "--preset", $Preset)
if ($res.StdOut.Trim()) { Write-Host $res.StdOut.Trim() }

Write-Step "3. Running CTest via the $Preset preset"
if ($RunCom0ComHil) {
    $hilMapPath = Join-Path $ExperimentDir "hil_serial_map.json"
    Assert-True (Test-Path -LiteralPath $hilMapPath -PathType Leaf) "hil_serial_map.json not found at $hilMapPath"
}
$res = Invoke-External -FilePath $ctestPath -WorkingDirectory $RepoRoot `
    -Arguments @("--preset", $Preset, "--output-on-failure", "--no-tests=error")
Write-Host $res.StdOut.Trim()

$BuildDir = Join-Path $ExperimentDir ("build-" + $Configuration.ToLowerInvariant())
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

if (-not $IsRelease) {
    Write-Step "WINDOWS DEBUG VERIFICATION PASSED"
    return
}

if (-not $WorkRoot) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss-fff"
    $WorkRoot = Join-Path $BuildDir "verify-$stamp"
}
$WorkRoot = [System.IO.Path]::GetFullPath($WorkRoot)
New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null

Write-Step "6. Installing Windows Release SDK prefix"
$InstallDir = Join-Path $WorkRoot "install-windows-release"
Invoke-External -FilePath $cmakePath -WorkingDirectory $RepoRoot `
    -Arguments @("--install", $BuildDir, "--config", "Release", "--prefix", $InstallDir) | Out-Null
Write-Host "  Installed to $InstallDir"

Write-Step "7. Packaging Windows-only SDK archive with hygiene verification"
$PackageDir = Join-Path $WorkRoot "package"
New-Item -ItemType Directory -Path $PackageDir -Force | Out-Null
$ZipPath = Join-Path $PackageDir "IR-SDK-$SdkVersion-Windows.zip"
$pkgArgs = @(
    "-X", "utf8", "-B",
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
                 "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DIR_SDK_ROOT=$PkgRoot") | Out-Null
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
                 "--recipe", (Join-Path $PkgRoot "examples\recipes\noise_roi.json"))
Assert-True ($standaloneCliCheck.StdOut.Trim() -eq "recipe=VALID") "standalone ir_run_recipe --check failed"
$null = Invoke-External -FilePath (Join-Path $ClientBuildDir "ir_capture_accept.exe") -Arguments @("--list")
Write-Host "  Standalone ir_c_client, ir_cpp_client, ir_analysis_c_sample, ir_run_recipe, and ir_capture_accept verified."

Write-Step "10. Running packaged CLI and Python smoke tests"
$PkgCli = Join-Path $PkgRoot "windows-x64\bin\ir_run_recipe.exe"
$TempRecipesDir = Join-Path $WorkRoot "temp-recipes"
New-Item -ItemType Directory -Path $TempRecipesDir -Force | Out-Null
$RawSmokeDir = Join-Path $TempRecipesDir "replay"
Invoke-External -FilePath $pythonPath -WorkingDirectory $RepoRoot `
    -Arguments @("-B", (Join-Path $RepoRoot "sdk_tests\raw_replay_cli_test.py"),
                 "--write-fixture", $RawSmokeDir) | Out-Null

$OfflineStationObj = [System.IO.File]::ReadAllText((Join-Path $PkgRoot "examples\stations\offline.json"), $Utf8NoBom) | ConvertFrom-Json
$OfflineStationObj.acquisition.source = "replay"
$OfflineStation = Join-Path $TempRecipesDir "offline.json"
Write-Utf8File $OfflineStation ($OfflineStationObj | ConvertTo-Json -Depth 10)
$OfflineRecipe = Join-Path $PkgRoot "examples\recipes\noise_roi.json"
$PkgPythonRecipe = Join-Path $PkgRoot "examples\python\ir_recipe.py"
$PackageEnv = @{ IR_SDK_LIB_DIR = "" }

$PassOut = Join-Path $WorkRoot "cli-pass-out"
$passRes = Invoke-External -FilePath $PkgCli -WorkingDirectory $WorkRoot `
    -Arguments @("--run", "--station", $OfflineStation, "--recipe", $OfflineRecipe, "--output", "cli-pass-out")
Assert-True ($passRes.StdOut -match "(?m)^run_status=PASS\r?$") "CLI PASS missing run_status=PASS (got: $($passRes.StdOut))"
$passJson = [System.IO.File]::ReadAllText((Join-Path $PassOut "result.json"), $Utf8NoBom) | ConvertFrom-Json
Assert-TraceabilityFields $passJson $SdkVersion "CLI PASS"

$pyCheck = Invoke-External -FilePath $pythonPath -WorkingDirectory $WorkRoot -Environment $PackageEnv `
    -Arguments @("-B", $PkgPythonRecipe, "--check", (Join-Path $PkgRoot "examples\stations\offline.json"), $OfflineRecipe)
Assert-True ($pyCheck.StdOut.Trim() -eq "recipe=VALID") "Packaged Python recipe preflight failed"
$pyRunRes = Invoke-External -FilePath $pythonPath -WorkingDirectory $WorkRoot `
    -Environment $PackageEnv -Arguments @("-B", $PkgPythonRecipe,
                 "--run", $OfflineStation, $OfflineRecipe, "py-recipe-out")
Assert-True ($pyRunRes.StdOut -match "(?m)^run_status=PASS\r?$") "ir_recipe.py --run failed (got: $($pyRunRes.StdOut))"
Assert-True (Test-Path (Join-Path $WorkRoot "py-recipe-out\result.json")) "Python output missing under working directory"
Write-Host "  Packaged CLI and Python recipe runs verified."

Write-Step "11. Checking optional C# sample (dotnet)"
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
    $csRes = Invoke-External -FilePath $dotnetExe -WorkingDirectory $WorkRoot `
        -Environment $csEnv -Arguments @("run", "--no-build", "--project", $CsProj,
            "--", "--recipe", $OfflineStation, $OfflineRecipe, "csharp-recipe-out")
    Assert-True ($csRes.StdOut -match "(?m)^run_status=PASS\r?$") "C# recipe sample failed (got: $($csRes.StdOut))"
    Assert-True (Test-Path (Join-Path $CsOut "result.json")) "C# output missing under working directory"

    $null = Invoke-External -FilePath $dotnetExe -WorkingDirectory $PkgRoot `
        -Environment $csEnv -Arguments @("run", "--no-build", "--project", $CsProj, "--", "--analysis")
    Write-Host "  C# sample verified (dotnet ${dotnetVer}: recipe + analysis)."
}

Write-Step "ALL WINDOWS VERIFICATION CHECKS PASSED"
Write-Host "  Package : $ZipPath"
Write-Host "  SHA-256 : $actualSha"
Write-Host "  Extract : $PkgRoot"
