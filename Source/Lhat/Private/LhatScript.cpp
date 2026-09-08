#include "LhatScript.h"

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
	LoaderContext->ScriptRoot = FPaths::ConvertRelativePathToFull(ScriptRoot);
	FPaths::CollapseRelativeDirectories(LoaderContext->ScriptRoot);
	Program = lhat_program_new(bStrict, &FLhatProgram::LoadScript, LoaderContext.Get());
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
	return Program != nullptr;
}

const LhatUnit* FLhatProgram::Check(const FString& EntryPoint)
{
	if (Program == nullptr)
	{
		return nullptr;
	}

	FTCHARToUTF8 EntryPointUtf8(*EntryPoint);
	return lhat_program_check(Program, EntryPointUtf8.Get());
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
	FString FullPath = FPaths::ConvertRelativePathToFull(Loader->ScriptRoot, RequestedPath);
	FPaths::CollapseRelativeDirectories(FullPath);
	if (!FPaths::IsUnderDirectory(FullPath, Loader->ScriptRoot))
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
