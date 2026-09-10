#include "LhatHostApiExport.h"

#include "HAL/FileManager.h"
#include "LhatScript.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Windows/WindowsHWrapper.h"

FString LhatHostApi::GetDefaultOutputPath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), TEXT("lhat-host.json"));
}

bool LhatHostApi::Export(const FString& RequestedPath, FString& OutputFile, FString& Error)
{
	check(IsInGameThread());
	Error.Empty();
	OutputFile = RequestedPath.IsEmpty()
		? GetDefaultOutputPath()
		: FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RequestedPath);
	FPaths::NormalizeFilename(OutputFile);
	if (!FPaths::CollapseRelativeDirectories(OutputFile) || FPaths::GetCleanFilename(OutputFile).IsEmpty()
		|| IFileManager::Get().DirectoryExists(*OutputFile))
	{
		Error = FString::Printf(TEXT("Output must name a file: %s"), *OutputFile);
		return false;
	}

	// Reuse exactly the runtime registrations, without checking any source or creating a VM.
	FLhatProgram Program(FLhatProgram::GetProjectScriptRoot());
	TArray<uint8> Json;
	if (!Program.GetHostApiJson(Json, Error)) return false;

	const FString Directory = FPaths::GetPath(OutputFile);
	if (!IFileManager::Get().MakeDirectory(*Directory, true))
	{
		Error = FString::Printf(TEXT("Could not create output directory: %s"), *Directory);
		return false;
	}
	const FString TemporaryFile = FPaths::CreateTempFilename(*Directory, TEXT(".lhat-host-"), TEXT(".tmp"));
	if (!FFileHelper::SaveArrayToFile(Json, *TemporaryFile))
	{
		IFileManager::Get().Delete(*TemporaryFile, false, false, true);
		Error = FString::Printf(TEXT("Could not write host API JSON in: %s"), *Directory);
		return false;
	}
	// The plugin currently targets Win64. Replace in the same directory, without
	// IFileManager::Move's delete-before-rename (which can lose the old file on failure).
	if (!::MoveFileExW(*TemporaryFile, *OutputFile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		const uint32 Code = ::GetLastError();
		IFileManager::Get().Delete(*TemporaryFile, false, false, true);
		Error = FString::Printf(TEXT("Could not replace %s (Windows error %u). Check permissions and read-only files."), *OutputFile, Code);
		return false;
	}
	return true;
}
