[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$EngineRoot,
	[Parameter(Mandatory = $true)][string]$ProjectPath,
	[ValidateRange(1, 10000000)][int]$Iterations = 200000,
	[ValidateRange(3, 101)][int]$Samples = 11,
	[ValidateRange(1, 20)][int]$Warmups = 3
)

$ErrorActionPreference = "Stop"
$projectFile = (Resolve-Path -LiteralPath $ProjectPath).Path
if ([IO.Path]::GetExtension($projectFile) -ne ".uproject") { throw "ProjectPath must name a .uproject file." }
$editor = Join-Path (Resolve-Path -LiteralPath $EngineRoot).Path "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) { throw "UnrealEditor-Cmd.exe not found." }
$directory = Join-Path (Split-Path -Parent $projectFile) ("Saved\LhatBenchmarks\" + (Get-Date -Format "yyyyMMdd-HHmmss-fff"))
New-Item -ItemType Directory -Path $directory | Out-Null
$outputFile = Join-Path $directory "bindings.json"
$logFile = Join-Path $directory "Unreal.log"
& $editor $projectFile -run=LhatBenchmarkBindings "-Iterations=$Iterations" "-Samples=$Samples" `
	"-Warmups=$Warmups" "-Output=$outputFile" "-abslog=$logFile" -unattended -NullRHI -nosplash -nosound
$resultCode = $LASTEXITCODE
if ($resultCode -ne 0 -or -not (Test-Path -LiteralPath $outputFile -PathType Leaf)) {
	throw "Binding benchmark failed (exit $resultCode). See $logFile"
}
$report = Get-Content -LiteralPath $outputFile -Raw | ConvertFrom-Json
if ($report.results.Count -ne 5 -or (Get-Content -LiteralPath $logFile -Raw) -notmatch 'LHAT BENCHMARK COMPLETE') {
	throw "Incomplete benchmark. See $logFile"
}
foreach ($row in $report.results) {
	if (-not $row.checksums_verified -or $row.static_ns_samples.Count -ne $Samples -or $row.cached_ns_samples.Count -ne $Samples -or
		$row.static_ns_median -le 0 -or $row.cached_ns_median -le 0) { throw "Invalid benchmark result." }
}
$report.results | Select-Object @{Name='API'; Expression={($_.function -split ':')[-1]}}, `
	@{Name='Static ns'; Expression={[math]::Round($_.static_ns_median, 1)}}, `
	@{Name='Cached ns'; Expression={[math]::Round($_.cached_ns_median, 1)}}, `
	@{Name='Ratio'; Expression={[math]::Round($_.ratio, 2)}}, `
	@{Name='Delta ns'; Expression={[math]::Round($_.paired_delta_ns_median, 1)}} | Format-Table -AutoSize
Write-Host "Results and reproducible Lhat scripts: $directory"
