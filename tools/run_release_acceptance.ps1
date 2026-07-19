param(
    [string]$BuildRoot = "out/release-acceptance",
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Triplet = "x64-mingw-dynamic",
    [UInt64]$StressPoints = 1000000,
    [UInt64]$StressGridSide = 4096,
    [UInt64]$StressEdits = 128,
    [switch]$SkipGdal,
    [switch]$SkipStress
)

$ErrorActionPreference = "Stop"
$Source = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $VcpkgRoot) {
    throw "VCPKG_ROOT is not set. Pass -VcpkgRoot or set the environment variable."
}
$Toolchain = Join-Path $VcpkgRoot "scripts/buildsystems/vcpkg.cmake"
if (-not (Test-Path $Toolchain)) { throw "vcpkg toolchain not found: $Toolchain" }

function Invoke-Native([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

function Configure-Build-Test([string]$Name, [bool]$WithGdal,
                              [bool]$Benchmarks) {
    $Build = Join-Path $Source "$BuildRoot/$Name"
    Invoke-Native cmake @(
        "-S", $Source, "-B", $Build, "-G", "MinGW Makefiles",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
        "-DVCPKG_TARGET_TRIPLET=$Triplet", "-DVCPKG_MANIFEST_MODE=OFF",
        "-DDT_BUILD_TESTS=ON", "-DDT_BUILD_EXAMPLES=ON", "-DDT_BUILD_GUI=ON",
        "-DDT_WITH_GDAL=$($WithGdal.ToString().ToUpperInvariant())",
        "-DDT_BUILD_BENCHMARKS=$($Benchmarks.ToString().ToUpperInvariant())")
    Invoke-Native cmake @("--build", $Build, "-j", "4")
    Invoke-Native ctest @("--test-dir", $Build, "--output-on-failure", "-C", "Release")
    return $Build
}

$CoreBuild = Configure-Build-Test "core" $false (-not $SkipStress)
$Sdk = Join-Path $Source "$BuildRoot/sdk"
Invoke-Native cmake @("--install", $CoreBuild, "--prefix", $Sdk)

$Consumer = Join-Path $Source "$BuildRoot/consumer"
Invoke-Native cmake @(
    "-S", (Join-Path $Source "tests/sdk_consumer"), "-B", $Consumer,
    "-G", "MinGW Makefiles", "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_PREFIX_PATH=$Sdk")
Invoke-Native cmake @("--build", $Consumer, "-j", "4")
$env:PATH = "$(Join-Path $Sdk 'bin');$env:PATH"
Invoke-Native (Join-Path $Consumer "dterrain_sdk_consumer.exe") @()

if (-not $SkipStress) {
    Invoke-Native (Join-Path $CoreBuild "dterrain_stress_suite.exe") @(
        "--points", "$StressPoints", "--grid-side", "$StressGridSide",
        "--edits", "$StressEdits")
}

$PackageBuild = $CoreBuild
if (-not $SkipGdal) {
    $PackageBuild = Configure-Build-Test "gdal" $true $false
}
Invoke-Native cpack @("--config", (Join-Path $PackageBuild "CPackConfig.cmake"),
                      "-G", "ZIP", "-B", (Join-Path $Source "$BuildRoot/packages"))
Write-Host "dterrain 1.0 release acceptance passed."
