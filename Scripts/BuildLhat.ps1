[CmdletBinding()]
param(
	[ValidateSet("Debug", "Development", "Test", "Shipping")]
	[string]$Configuration = "Development"
)

$ErrorActionPreference = "Stop"

$pluginRoot = Split-Path -Parent $PSScriptRoot
$lhatSourceRoot = Join-Path $pluginRoot "LhatCore"
$buildRoot = Join-Path $pluginRoot "Intermediate\LhatCore\Win64\Native"
$cmakeConfiguration = if ($Configuration -eq "Debug") { "Debug" } else { "Release" }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path (Join-Path $lhatSourceRoot "CMakeLists.txt") -PathType Leaf)) {
	throw "The LhatCore submodule is unavailable. Run 'git submodule update --init --recursive' from the lhat-UE repository."
}

$cmake = Get-Command cmake -ErrorAction Stop
$vsVersion = if (Test-Path $vswhere -PathType Leaf) {
	& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
}
if ([string]::IsNullOrWhiteSpace($vsVersion)) {
	throw "A Visual Studio installation with the MSVC x64/x86 build tools is required."
}

$vsMajorVersion = [int]($vsVersion.Split('.')[0])
$cmakeGenerator = switch ($vsMajorVersion) {
	18 { "Visual Studio 18 2026" }
	17 { "Visual Studio 17 2022" }
	default { throw "Visual Studio $vsMajorVersion is not supported by this bootstrap script." }
}

$configureArgs = @(
	"-S", $lhatSourceRoot,
	"-B", $buildRoot,
	"-G", $cmakeGenerator,
	"-A", "x64",
	"-DLHAT_WITH_FRONTEND=ON",
	"-DLHAT_BUILD_TESTS=OFF",
	"-DLHAT_BUILD_CLI=OFF",
	"-DLHAT_BUILD_LSP=OFF",
	"-DLHAT_BUILD_DAP=OFF",
	"-DLHAT_BUILD_STDLIB=OFF",
	"-DLHAT_WITH_DEBUGGER=OFF",
	"-DLHAT_WITH_COMMENTS=OFF",
	"-DLHAT_WITH_RESOLUTIONS=OFF"
)

& $cmake.Source @configureArgs
if ($LASTEXITCODE -ne 0) {
	throw "CMake configuration failed with exit code $LASTEXITCODE."
}

& $cmake.Source --build $buildRoot --config $cmakeConfiguration --target lhat lhatport
if ($LASTEXITCODE -ne 0) {
	throw "Lhat build failed with exit code $LASTEXITCODE."
}

$artifacts = @(
	(Join-Path $buildRoot "$cmakeConfiguration\lhat.lib"),
	(Join-Path $buildRoot "$cmakeConfiguration\lhatport.lib"),
	(Join-Path $buildRoot "include\lhat\version.h")
)
$missingArtifacts = $artifacts | Where-Object { -not (Test-Path $_ -PathType Leaf) }
if ($missingArtifacts) {
	throw "Lhat completed without the expected artifacts: $($missingArtifacts -join ', ')"
}

Write-Host "Lhat $cmakeConfiguration libraries are ready in $buildRoot."
