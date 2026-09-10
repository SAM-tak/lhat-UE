#include "Modules/ModuleManager.h"

#include "HAL/IConsoleManager.h"
#include "LhatHostApiExport.h"
#include "LhatModule.h"

namespace
{
	void DumpHostApi(const TArray<FString>& Args)
	{
		if (Args.Num() > 1 || (Args.Num() == 1 && Args[0].IsEmpty()))
		{
			UE_LOG(LogLhat, Error, TEXT("Usage: Lhat.DumpHostApi [\"output path\"]"));
			return;
		}
		FString OutputFile, Error;
		if (LhatHostApi::Export(Args.IsEmpty() ? FString() : Args[0], OutputFile, Error))
		{
			UE_LOG(LogLhat, Display, TEXT("Host API written to %s"), *OutputFile);
		}
		else UE_LOG(LogLhat, Error, TEXT("Host API export failed: %s"), *Error);
	}
}

class FLhatEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		DumpCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("Lhat.DumpHostApi"),
			TEXT("Export the registered UE host API. Usage: Lhat.DumpHostApi [\"output path\"]. Default: <Project>/lhat-host.json; relative paths use ProjectDir."),
			FConsoleCommandWithArgsDelegate::CreateStatic(&DumpHostApi));
	}
	virtual void ShutdownModule() override
	{
		if (DumpCommand) IConsoleManager::Get().UnregisterConsoleObject(DumpCommand, false);
		DumpCommand = nullptr;
	}
private:
	IConsoleObject* DumpCommand = nullptr;
};

IMPLEMENT_MODULE(FLhatEditorModule, LhatEditor)
