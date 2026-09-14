#include "LhatScript.h"
#include "LhatBindings.h"
#include "LhatNativeBindings.h"

#include "HAL/UnrealMemory.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

struct FLhatProgram::FLoaderContext
{
	FString ScriptRoot;
};

FLhatProgram::FLhatProgram(const FString& ScriptRoot, bool bStrict)
	: LoaderContext(MakeUnique<FLoaderContext>())
{
	check(IsInGameThread());
	LoaderContext->ScriptRoot = FPaths::ConvertRelativePathToFull(ScriptRoot);
	FPaths::CollapseRelativeDirectories(LoaderContext->ScriptRoot);
	if (!LhatUEBindings::LoadGeneratedProviders(InitializationError)) return;
	Program = lhat_program_new(bStrict, &FLhatProgram::LoadScript, LoaderContext.Get());
	Bindings = MakeUnique<FLhatBindings>();
	if (Program == nullptr || !Bindings->Register(Program))
	{
		InitializationError = TEXT("Could not register the UE host API.");
	}
}

FLhatProgram::~FLhatProgram()
{
	if (Program != nullptr)
	{
		lhat_program_free(Program);
	}
}

bool FLhatProgram::IsValid() const
{
	return Program != nullptr && InitializationError.IsEmpty();
}

const LhatUnit* FLhatProgram::Check(const FString& EntryPoint)
{
	check(IsInGameThread());
	if (!IsValid() || EntryPoint.IsEmpty() || !FPaths::IsRelative(EntryPoint))
	{
		return nullptr;
	}
	FString FullPath = FPaths::ConvertRelativePathToFull(LoaderContext->ScriptRoot, EntryPoint);
	if (!FPaths::CollapseRelativeDirectories(FullPath) || !FPaths::IsUnderDirectory(FullPath, LoaderContext->ScriptRoot))
	{
		return nullptr;
	}

	FTCHARToUTF8 EntryPointUtf8(*EntryPoint);
	return lhat_program_check(Program, EntryPointUtf8.Get());
}

FString FLhatProgram::GetProjectScriptRoot()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Script"));
}

FLhatBindings& FLhatProgram::GetBindings() const
{
	return *Bindings;
}

FString FLhatProgram::GetDiagnostics() const
{
	if (!InitializationError.IsEmpty())
	{
		return InitializationError;
	}
	FString Result;
	for (size_t Index = 0; Index < lhat_program_diagnostic_count(Program); ++Index)
	{
		const LhatProgramDiagnostic* Diagnostic = lhat_program_diagnostic(Program, Index);
		Result += FString::Printf(TEXT("%s: %s\n"), UTF8_TO_TCHAR(Diagnostic->path), UTF8_TO_TCHAR(lhat_program_error_message(Diagnostic->code)));
	}
	for (const LhatUnit* Unit = lhat_program_units(Program); Unit; Unit = lhat_unit_next(Unit))
	{
		for (size_t Index = 0; Index < lhat_unit_diagnostic_count(Unit); ++Index)
		{
			const size_t Length = lhat_unit_diagnostic_write(Unit, Index, false, nullptr, 0);
			TArray<char> Text;
			Text.SetNumUninitialized(static_cast<int32>(Length + 1));
			lhat_unit_diagnostic_write(Unit, Index, false, Text.GetData(), Text.Num());
			Result += UTF8_TO_TCHAR(Text.GetData());
			Result += TEXT("\n");
		}
	}
	if (lhat_program_compile_status(Program) != LHAT_COMPILE_OK)
	{
		Result += UTF8_TO_TCHAR(lhat_compile_status_message(lhat_program_compile_status(Program)));
	}
	return Result;
}

bool FLhatProgram::Compile()
{
	return Program != nullptr && lhat_program_compile(Program);
}

LhatMachine* FLhatProgram::CreateMachine() const
{
	return lhat_machine_new();
}

bool FLhatProgram::Install(LhatMachine* Machine) const
{
	return Program != nullptr && Machine != nullptr && lhat_program_install(Program, Machine);
}

LhatProgram* FLhatProgram::GetNativeHandle() const
{
	return Program;
}

void FLhatProgram::DestroyMachine(LhatMachine* Machine)
{
	if (Machine != nullptr)
	{
		lhat_machine_dispose(Machine);
	}
}

char* FLhatProgram::LoadScript(void* Context, const char* Path, size_t* OutLength)
{
	FLoaderContext* Loader = static_cast<FLoaderContext*>(Context);
	if (Loader == nullptr || Path == nullptr || OutLength == nullptr)
	{
		return nullptr;
	}

	const FString RequestedPath = UTF8_TO_TCHAR(Path);
	*OutLength = 0;
	if (!FPaths::IsRelative(RequestedPath))
	{
		return nullptr;
	}
	FString FullPath = FPaths::ConvertRelativePathToFull(Loader->ScriptRoot, RequestedPath);
	if (!FPaths::CollapseRelativeDirectories(FullPath) || !FPaths::IsUnderDirectory(FullPath, Loader->ScriptRoot))
	{
		return nullptr;
	}

	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *FullPath))
	{
		return nullptr;
	}

	char* Buffer = static_cast<char*>(lhat_alloc(static_cast<size_t>(FileBytes.Num()) + 1));
	if (Buffer == nullptr)
	{
		return nullptr;
	}

	if (!FileBytes.IsEmpty())
	{
		FMemory::Memcpy(Buffer, FileBytes.GetData(), FileBytes.Num());
	}
	Buffer[FileBytes.Num()] = '\0';
	*OutLength = static_cast<size_t>(FileBytes.Num());
	return Buffer;
}
