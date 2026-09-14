[CmdletBinding()]
param(
    [string]$Python = 'python',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$pluginDirectory = Split-Path -Parent $PSScriptRoot
$nativeDirectory = Join-Path $pluginDirectory 'Intermediate/LhatCore/Win64/Native'
$bridgeDirectory = Join-Path $nativeDirectory 'UEBridge'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsDirectory = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsDirectory) { throw 'MSVC C++ tools were not found.' }
$toolsVersion = (Get-Content -LiteralPath (Join-Path $vsDirectory 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$toolsDirectory = Join-Path $vsDirectory "VC/Tools/MSVC/$toolsVersion"
$kitsRoot = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$sdk = Get-ChildItem -LiteralPath (Join-Path $kitsRoot 'Include') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName 'ucrt/stdio.h')) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$sdk) { throw 'Windows SDK UCRT headers were not found.' }
New-Item -ItemType Directory -Path $bridgeDirectory -Force | Out-Null
$preprocessed = Join-Path $bridgeDirectory 'CoreApi.i'
& (Join-Path $toolsDirectory 'bin/Hostx64/x64/cl.exe') /nologo /TC /std:c11 /P "/Fi$preprocessed" "/I$pluginDirectory/LhatCore/include" "/I$nativeDirectory/include" "/I$toolsDirectory/include" "/I$($sdk.FullName)/ucrt" (Join-Path $PSScriptRoot 'CoreApiInput.c')
if ($LASTEXITCODE -ne 0) { throw 'Core API preprocessing failed.' }
$symbols = & (Join-Path $toolsDirectory 'bin/Hostx64/x64/dumpbin.exe') /nologo /linkermember:2 (Join-Path $nativeDirectory "$Configuration/lhat.lib") (Join-Path $nativeDirectory "$Configuration/lhatport.lib")
if ($LASTEXITCODE -ne 0) { throw 'Core API symbol discovery failed.' }
$symbolsFile = Join-Path $bridgeDirectory 'CoreApiSymbols.txt'
$symbols | Set-Content -LiteralPath $symbolsFile -Encoding utf8
& $Python (Join-Path $PSScriptRoot 'GenerateCoreApi.py') --preprocessed $preprocessed --symbols $symbolsFile --output $bridgeDirectory
if ($LASTEXITCODE -ne 0) { throw 'UE core API header generation failed.' }
