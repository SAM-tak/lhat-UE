#include "LhatHostApiExport.h"

#include "HAL/FileManager.h"
#include "LhatScript.h"
#include "LhatBindings.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Windows/WindowsHWrapper.h"

FString LhatHostApi::GetDefaultOutputPath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), TEXT("lhat-host.json"));
}

bool LhatHostApi::Export(const FString& RequestedPath, FString& OutputFile, FString& Error, bool bBindingReport)
{
	check(IsInGameThread());
	Error.Empty();
	OutputFile = RequestedPath.IsEmpty()
		? (bBindingReport ? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), TEXT("lhat-bindings.json")) : GetDefaultOutputPath())
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
	if (bBindingReport)
	{
		if (!Program.IsValid()) { Error = Program.GetDiagnostics(); return false; }
		const FString Report = Program.GetBindings().GetDynamicBindingReport();
		const FTCHARToUTF8 Utf8(*Report);
		Json.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
	else if (!Program.GetHostApiJson(Json, Error)) return false;

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
