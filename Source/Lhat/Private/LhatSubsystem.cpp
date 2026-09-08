#include "LhatSubsystem.h"

#include "LhatInstance.h"
#include "LhatModule.h"
#include "GameFramework/Actor.h"

bool ULhatSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TSharedPtr<FLhatInstance> ULhatSubsystem::StartScript(const FString& ScriptPath, AActor* Owner, const FInstancedPropertyBag& Parameters, FString& Error)
{
	check(IsInGameThread());
	Error.Empty();
	if (!IsValid(Owner) || Owner->GetWorld() != GetWorld())
	{
		Error = TEXT("The script's Actor must belong to this runtime's World.");
		return nullptr;
	}
	TSharedPtr<FLhatProgram> Program = Programs.FindRef(ScriptPath);
	if (!Program)
	{
		Program = MakeShared<FLhatProgram>(FLhatProgram::GetProjectScriptRoot());
	}
	const LhatUnit* Unit = Program->Check(ScriptPath);
	if (!Unit || !lhat_unit_ok(Unit) || !Program->Compile())
	{
		Error = Program->GetDiagnostics();
		if (Error.IsEmpty()) Error = TEXT("Invalid script path or script could not be compiled.");
		return nullptr;
	}
	Programs.Add(ScriptPath, Program);
	TSharedPtr<FLhatInstance> Instance = MakeShared<FLhatInstance>(Program.ToSharedRef());
	if (!Instance->Start(Unit, Owner, &Parameters))
	{
		Error = Instance->GetError();
		return nullptr;
	}
	Instances.Add(Instance);
	return Instance;
}

void ULhatSubsystem::StopScript(const TSharedPtr<FLhatInstance>& Instance)
{
	if (Instance)
	{
		Instance->Stop();
		Instances.Remove(Instance);
	}
}

void ULhatSubsystem::Deinitialize()
{
	for (const TSharedPtr<FLhatInstance>& Instance : Instances)
	{
		Instance->Stop();
	}
	Instances.Empty();
	Programs.Empty();
	Super::Deinitialize();
}
