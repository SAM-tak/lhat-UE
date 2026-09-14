[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EngineRoot,
    [Parameter(Mandatory = $true)][string]$ProjectPath,
    [string]$Target,
    [string]$ManifestPath,
    [string]$Python = 'python',
    [switch]$SkipExport,
    [string]$SnapshotDirectory,
    [switch]$Install,
    [switch]$Build
)

$ErrorActionPreference = 'Stop'
$projectFile = (Resolve-Path -LiteralPath $ProjectPath).Path
$projectDirectory = Split-Path -Parent $projectFile
$engineDirectory = (Resolve-Path -LiteralPath $EngineRoot).Path
if (!$Target) { $Target = [IO.Path]::GetFileNameWithoutExtension($projectFile) + 'Editor' }
if (!$Target.EndsWith('Editor')) { throw 'Generate from an Editor target to include both Runtime and Editor APIs.' }
if (!$ManifestPath) {
    $ManifestPath = Join-Path $projectDirectory "Intermediate/Build/Win64/$Target/$Target.uhtmanifest"
}
if (!$SkipExport) {
    # Refresh header discovery without compiling stale generated callbacks first.
    $targetArgument = '-Target=' + $Target + ' Win64 Development -Project="' + $projectFile + '"'
    # Invoke dotnet directly: cmd/.bat strips the nested quotes around spaced project paths.
    $dotnet = Join-Path $engineDirectory 'Engine/Binaries/ThirdParty/DotNet/10.0/win-x64/dotnet.exe'
    $ubtAssembly = Join-Path $engineDirectory 'Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll'
    & $dotnet $ubtAssembly -Mode=UnrealHeaderTool $targetArgument -NoDefaultExporters -NoGoWide
    if ($LASTEXITCODE -ne 0) { throw 'UHT target discovery failed.' }
    if (!$SnapshotDirectory) {
        $SnapshotDirectory = Join-Path $projectDirectory ('Saved/LhatNativeBindings/Snapshot-' + [Guid]::NewGuid().ToString('N'))
    }
    & (Join-Path $PSScriptRoot 'ExportUhtForSurvey.ps1') -EngineRoot $engineDirectory -ProjectPath $projectFile -ManifestPath $ManifestPath -OutputDirectory $SnapshotDirectory
}
elseif (!$SnapshotDirectory) { throw '-SkipExport requires -SnapshotDirectory (the snapshot root containing UHT/Lhat).' }
# Do not load generated modules here. They use LoadingPhase=None and are loaded
# by FLhatProgram only, so stale bindings cannot block their own regeneration.
$contextFile = Join-Path ([IO.Path]::GetFullPath($SnapshotDirectory)) 'context.json'
$contextLog = Join-Path ([IO.Path]::GetFullPath($SnapshotDirectory)) 'NativeContext.log'
& (Join-Path $engineDirectory 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $projectFile -run=LhatNativeContext "-Output=$contextFile" -unattended -NullRHI -nosound -nosplash "-abslog=$contextLog"
if ($LASTEXITCODE -ne 0) { throw 'Enabled-plugin inventory failed. Build the Lhat and LhatEditor modules first.' }
$uhtDirectory = Join-Path $SnapshotDirectory 'UHT/Lhat'
$generatorArguments = @((Join-Path $PSScriptRoot 'GenerateNativeBindings.py'), '--snapshot', $uhtDirectory, '--project', $projectFile)
if ($Install) { $generatorArguments += '--install' }
& $Python @generatorArguments
if ($LASTEXITCODE -ne 0) { throw "Native binding generation failed: $LASTEXITCODE" }
if ($Build) {
    & (Join-Path $engineDirectory 'Engine/Build/BatchFiles/Build.bat') $Target Win64 Development "-Project=$projectFile" -WaitMutex -NoHotReload
    if ($LASTEXITCODE -ne 0) { throw "Generated C++ build failed: $LASTEXITCODE" }
}
