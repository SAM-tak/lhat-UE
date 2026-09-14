[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$EngineRoot,
	[string]$OutputDirectory,
	[switch]$EditorOnly,
	[string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
$pluginDirectory = Split-Path -Parent $PSScriptRoot
$engineDirectory = (Resolve-Path -LiteralPath $EngineRoot).Path
if (!$OutputDirectory) {
	$OutputDirectory = Join-Path $pluginDirectory ('Saved/Packages/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$jobDirectory = [IO.Path]::GetFullPath($OutputDirectory)
# BuildPlugin clears its Package directory. Always give it a new child of an
# entirely new job directory, never the source plugin or a user-populated folder.
if (Test-Path -LiteralPath $jobDirectory) { throw "OutputDirectory already exists: $jobDirectory" }
if ([IO.Path]::GetPathRoot($jobDirectory) -eq $jobDirectory) { throw 'OutputDirectory must not be a drive root.' }
if ($jobDirectory.Length -gt 100) { Write-Warning 'A short OutputDirectory is recommended on Windows to avoid compiler path-length limits.' }
& (Join-Path $PSScriptRoot 'BuildLhat.ps1') -Python $Python
$native = Join-Path $pluginDirectory 'Intermediate/LhatCore/Win64/Native'
$inputPlugin = Join-Path $jobDirectory 'Input/Lhat'
$package = Join-Path $jobDirectory 'Lhat'
New-Item -ItemType Directory -Path $inputPlugin | Out-Null
foreach ($name in @('Source', 'Config', 'Bindings', 'Docs', 'Examples', 'Lhat.uplugin', 'README.md')) {
	Copy-Item -LiteralPath (Join-Path $pluginDirectory $name) -Destination $inputPlugin -Recurse
}
Get-ChildItem -LiteralPath $pluginDirectory -Filter 'LICENSE*' -File | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $inputPlugin }
$packagedCore = Join-Path $inputPlugin 'Build/LhatCore/Win64/Native'
New-Item -ItemType Directory -Path (Join-Path $packagedCore 'Release'), (Join-Path $packagedCore 'UEBridge') | Out-Null
foreach ($name in @('lhat.lib', 'lhatport.lib')) { Copy-Item -LiteralPath (Join-Path $native "Release/$name") -Destination (Join-Path $packagedCore 'Release') }
Copy-Item -LiteralPath (Join-Path $native 'include') -Destination $packagedCore -Recurse
Copy-Item -LiteralPath (Join-Path $native 'UEBridge/include') -Destination (Join-Path $packagedCore 'UEBridge') -Recurse
Copy-Item -LiteralPath (Join-Path $native 'UEBridge/LhatCoreExports.inl') -Destination (Join-Path $packagedCore 'UEBridge')
Get-ChildItem -LiteralPath (Join-Path $pluginDirectory 'LhatCore') -Filter 'LICENSE*' -File | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $packagedCore }
$cjsonNotices = Join-Path $inputPlugin 'ThirdParty/cJSON'
New-Item -ItemType Directory -Path $cjsonNotices | Out-Null
Copy-Item -LiteralPath (Join-Path $pluginDirectory 'LhatCore/vendor/cjson/LICENSE') -Destination $cjsonNotices
$uat = Join-Path $engineDirectory 'Engine/Build/BatchFiles/RunUAT.bat'
$arguments = @('BuildPlugin', "-Plugin=$inputPlugin/Lhat.uplugin", "-Package=$package", '-HostPlatforms=Win64', '-NoDeleteHostProject')
if ($EditorOnly) { $arguments += '-NoTargetPlatforms' } else { $arguments += '-TargetPlatforms=Win64' }
& $uat @arguments 2>&1 | Tee-Object -FilePath (Join-Path $jobDirectory 'Package.log')
if ($LASTEXITCODE -ne 0) { throw "Plugin packaging failed. Staging and logs retained in $jobDirectory" }
foreach ($module in @('Lhat', 'LhatEditor')) {
	if (!(Test-Path -LiteralPath (Join-Path $package "Binaries/Win64/UnrealEditor-$module.dll"))) { throw "Missing packaged $module DLL." }
}
$engineBuildId = (Get-Content -LiteralPath (Join-Path $engineDirectory 'Engine/Binaries/Win64/UnrealEditor.modules') -Raw | ConvertFrom-Json).BuildId
$packageBuildId = (Get-Content -LiteralPath (Join-Path $package 'Binaries/Win64/UnrealEditor.modules') -Raw | ConvertFrom-Json).BuildId
if ($engineBuildId -ne $packageBuildId) { throw 'Packaged Editor modules do not match the engine BuildId.' }
if (!$EditorOnly) {
	foreach ($configuration in @('Development', 'Shipping')) {
		$manifests = @(Get-ChildItem -LiteralPath (Join-Path $package 'Intermediate/Build') -Recurse -Filter 'Lhat.precompiled' | Where-Object { $_.FullName -match "[\\/]UnrealGame[\\/]$configuration[\\/]" })
		if (!$manifests.Count) { throw "Missing $configuration precompiled manifest." }
		foreach ($manifest in $manifests) {
			foreach ($relative in (Get-Content -LiteralPath $manifest.FullName -Raw | ConvertFrom-Json).OutputFiles) {
				if (!(Test-Path -LiteralPath (Join-Path $manifest.DirectoryName $relative) -PathType Leaf)) { throw "Missing precompiled output: $relative ($configuration)." }
			}
		}
	}
}
# Keep diagnostics, but do not ship the build's dummy project inside the plugin.
$hostProject = [IO.Path]::GetFullPath((Join-Path $package 'HostProject'))
$retainedHost = [IO.Path]::GetFullPath((Join-Path $jobDirectory 'HostProject'))
foreach ($path in @($hostProject, $retainedHost)) {
	if (!$path.StartsWith($jobDirectory.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe staging move: $path" }
}
if (Test-Path -LiteralPath $retainedHost) { throw "Retained host destination exists: $retainedHost" }
Move-Item -LiteralPath $hostProject -Destination $retainedHost
$release = [ordered]@{
	schema = 1
	editor_only = [bool]$EditorOnly
	engine = (Get-Content -LiteralPath (Join-Path $engineDirectory 'Engine/Build/Build.version') -Raw | ConvertFrom-Json)
	engine_build_id = $engineBuildId
	core_revision = (& git -C (Join-Path $pluginDirectory 'LhatCore') rev-parse HEAD)
	bundled_manifest_sha256 = (Get-FileHash -LiteralPath (Join-Path $package 'Bindings/BundledEngineApi.generated.json') -Algorithm SHA256).Hash
}
[IO.File]::WriteAllText((Join-Path $package 'Build/LhatPrecompiled.json'), ($release | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
Write-Host "Packaged plugin: $package"
Write-Host 'Built for this exact UE engine build. Blueprint projects need no generated project module.'
Write-Host 'Raw Lhat source staging and packaged-game execution are separate, not validated by BuildPlugin.'
