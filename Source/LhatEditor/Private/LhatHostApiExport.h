#pragma once

#include "CoreMinimal.h"

namespace LhatHostApi
{
	/** Absolute path to lhat-host.json beside the .uproject file. */
	FString GetDefaultOutputPath();

	/** Empty means <Project>/lhat-host.json; relative paths are relative to ProjectDir. */
	bool Export(const FString& RequestedPath, FString& OutputFile, FString& Error, bool bBindingReport = false);
}
