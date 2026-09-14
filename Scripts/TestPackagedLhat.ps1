[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$EngineRoot,
	[Parameter(Mandatory = $true)][string]$PluginPackage,
	[string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$packageDirectory = (Resolve-Path -LiteralPath $PluginPackage).Path
if (!(Test-Path -LiteralPath (Join-Path $packageDirectory 'Lhat.uplugin'))) { throw 'PluginPackage must contain Lhat.uplugin.' }
if (Test-Path -LiteralPath (Join-Path $packageDirectory 'HostProject')) { throw 'Use the final package, not the retained UAT host project.' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path (Split-Path -Parent $packageDirectory) ('BlueprintSmoke-' + [Guid]::NewGuid().ToString('N')) }
$fixture = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $fixture) { throw "Refusing to overwrite fixture: $fixture" }
New-Item -ItemType Directory -Path (Join-Path $fixture 'Plugins') | Out-Null
Copy-Item -LiteralPath $packageDirectory -Destination (Join-Path $fixture 'Plugins/Lhat') -Recurse
$projectFile = Join-Path $fixture 'BlueprintSmoke.uproject'
# No Modules, no Source, no .Target.cs, no generated provider and no build call.
# Also prove the bundle does not rely on optional/default-enabled Engine plugins.
[IO.File]::WriteAllText($projectFile, '{"FileVersion":3,"DisableEnginePluginsByDefault":true,"Plugins":[{"Name":"Lhat","Enabled":true}]}', [Text.UTF8Encoding]::new($false))
$editor = Join-Path (Resolve-Path -LiteralPath $EngineRoot).Path 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$exportLog = Join-Path $fixture 'Export.log'
& $editor $projectFile -run=LhatDumpHostApi -unattended -NullRHI -nosplash -nosound "-abslog=$exportLog"
if ($LASTEXITCODE -ne 0) { throw "Blueprint-only host export failed: $exportLog" }
$api = Get-Content -LiteralPath (Join-Path $fixture 'lhat-host.json') -Raw | ConvertFrom-Json
foreach ($name in @('Abs', 'VSize', 'SetActorTickEnabled', 'SetRelativeScale3D', 'GetActorLabel')) {
	if (!($api.functions | Where-Object { $_.name -eq $name })) { throw "Bundled API missing: $name" }
}
if ($api.types | Where-Object { $_.module -like 'ue.LhatGenerated*' }) { throw 'Project-generated API leaked into the binary-only fixture.' }
& (Join-Path $PSScriptRoot 'TestLhat.ps1') -EngineRoot $EngineRoot -ProjectPath $projectFile
if (Test-Path -LiteralPath (Join-Path $fixture 'Source')) { throw 'Smoke test unexpectedly created project C++ source.' }
$lastLog = Get-ChildItem -LiteralPath (Join-Path $fixture 'Saved/Logs') -Filter 'LhatTests-*.log' | Sort-Object LastWriteTime | Select-Object -Last 1
$logText = Get-Content -LiteralPath $lastLog.FullName -Raw
if ($logText -notmatch 'Result=\{Success\}.*Path=\{Lhat.Editor.BundledEngineBindings\}') { throw 'Bundled binding integration test did not pass.' }
foreach ($test in @('DynamicNativeBindings', 'DynamicBlueprintBindings')) {
	if ($logText -notmatch ("Result=\{Success\}.*Path=\{Lhat.Editor." + $test + "\}")) { throw "Dynamic binding test did not pass: $test" }
}
if ($logText -match "InternalLoadLibrary: 'LhatGenerated") { throw 'A project-generated provider was loaded.' }
Write-Host "Blueprint-only binary plugin verified: $fixture"
Write-Host "Host API: $($api.types.Count) types / $($api.functions.Count) functions. No project compilation was invoked."
