[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EngineRoot,
    [Parameter(Mandatory = $true)][string]$ProjectPath,
    [Parameter(Mandatory = $true)][string]$ManifestPath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$projectFile = (Resolve-Path -LiteralPath $ProjectPath).Path
$manifestFile = (Resolve-Path -LiteralPath $ManifestPath).Path
$engineDirectory = (Resolve-Path -LiteralPath $EngineRoot).Path
$surveyDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $surveyDirectory) {
    throw 'Use a new output directory to avoid mixing snapshots.'
}
New-Item -ItemType Directory -Path $surveyDirectory | Out-Null
$dotnet = Join-Path $engineDirectory 'Engine/Binaries/ThirdParty/DotNet/10.0/win-x64/dotnet.exe'
$exporterOutput = Join-Path $surveyDirectory 'Exporter'
$exporterProject = Join-Path $PSScriptRoot 'UhtSurvey/UhtSurvey.csproj'
& $dotnet build $exporterProject "-p:EngineRoot=$engineDirectory" "-p:BaseIntermediateOutputPath=$surveyDirectory/ExporterObj/" --output $exporterOutput --nologo --verbosity quiet
if ($LASTEXITCODE -ne 0) { throw "Survey exporter build failed: $LASTEXITCODE" }
$manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
$manifest | Add-Member -NotePropertyName UhtPlugins -NotePropertyValue @((Join-Path $exporterOutput 'UhtSurvey.dll')) -Force
$manifest.ExternalDependenciesFile = Join-Path $surveyDirectory 'survey.deps'
foreach ($module in $manifest.Modules) {
    $module.OutputDirectory = Join-Path $surveyDirectory ('UHT/' + $module.Name)
    New-Item -ItemType Directory -Path $module.OutputDirectory -Force | Out-Null
}
# Only the survey exporter runs; normal generated engine/plugin files are untouched.
$surveyManifest = Join-Path $surveyDirectory 'survey.uhtmanifest'
$manifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $surveyManifest -Encoding utf8
$ubt = Join-Path $engineDirectory 'Engine/Build/BatchFiles/RunUBT.bat'
& $ubt -Mode=UnrealHeaderTool $projectFile $surveyManifest -NoDefaultExporters -LhatSurvey -NoGoWide 2>&1 | Tee-Object -FilePath (Join-Path $surveyDirectory 'UhtExport.log')
if ($LASTEXITCODE -ne 0) { throw "UHT JSON export failed: $LASTEXITCODE" }
$exports = @(Get-ChildItem -LiteralPath (Join-Path $surveyDirectory 'UHT') -Recurse -Filter '*.json')
if ($exports.Count -ne $manifest.Modules.Count) {
    throw "Expected $($manifest.Modules.Count) module exports, found $($exports.Count)."
}
Write-Host "Exported $($exports.Count) UHT modules to $surveyDirectory"
