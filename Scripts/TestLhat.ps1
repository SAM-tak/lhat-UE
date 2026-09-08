[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)]
	[string]$EngineRoot,
	[Parameter(Mandatory = $true)]
	[string]$ProjectPath
)

$ErrorActionPreference = "Stop"
$projectFile = (Resolve-Path -LiteralPath $ProjectPath).Path
if ([IO.Path]::GetExtension($projectFile) -ne ".uproject") {
	throw "ProjectPath must name a .uproject file."
}
$editor = Join-Path (Resolve-Path -LiteralPath $EngineRoot).Path "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) {
	throw "UnrealEditor-Cmd.exe was not found under EngineRoot."
}
$logDirectory = Join-Path (Split-Path -Parent $projectFile) "Saved\Logs"
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$testLog = Join-Path $logDirectory ("LhatTests-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")

# Automation's queued SoftQuit propagates failures and flushes the log on exit;
# -TestExit alone can return 0 on failures, while forced Quit can truncate logs.
& $editor $projectFile /Engine/Maps/Entry `
	"-ExecCmds=Automation RunTests Lhat.; SoftQuit" `
	-unattended -NullRHI -nosplash -nosound "-abslog=$testLog"
$testExitCode = $LASTEXITCODE
if (-not (Test-Path -LiteralPath $testLog -PathType Leaf)) {
	throw "Unreal produced no test log (exit $testExitCode)."
}
$testOutput = Get-Content -LiteralPath $testLog -Raw
$passed = [regex]::Matches($testOutput, 'Test Completed\. Result=\{Success\}.*Path=\{Lhat\.').Count
$failed = [regex]::Matches($testOutput, 'Test Completed\. Result=\{Fail\}.*Path=\{Lhat\.').Count
Write-Host "Lhat tests: $passed passed, $failed failed. Log: $testLog"
if ($testExitCode -ne 0 -or $failed -ne 0 -or $passed -eq 0 -or
	-not $testOutput.Contains("TEST COMPLETE. EXIT CODE: 0")) {
	throw "Lhat automation failed or did not complete (Unreal exit $testExitCode). See $testLog"
}
