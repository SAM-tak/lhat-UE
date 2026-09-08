#include "LhatComponent.h"

#include "Engine/World.h"
#include "LhatInstance.h"
#include "LhatModule.h"
#include "LhatSubsystem.h"
#include "UObject/GarbageCollection.h"

ULhatComponent::ULhatComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

ULhatComponent::~ULhatComponent() = default;

void ULhatComponent::BeginPlay()
{
	Super::BeginPlay();
	LastError.Empty();
	if (ScriptPath.IsEmpty()) return;
	if (auto* Runtime = GetWorld()->GetSubsystem<ULhatSubsystem>())
	{
		const FInstancedPropertyBag Overrides = GetParameterOverrides();
		Instance = Runtime->StartScript(ScriptPath, GetOwner(), Overrides, LastError);
	}
	if (!Instance)
	{
		if (LastError.IsEmpty()) LastError = TEXT("Lhat runtime is unavailable in this world.");
		UE_LOG(LogLhat, Error, TEXT("%s (%s): %s"), *GetPathName(), *ScriptPath, *LastError);
	}
	SetComponentTickEnabled(Instance && Instance->HasTick());
}

void ULhatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (Instance && !Instance->Tick(DeltaTime))
	{
		LastError = Instance->GetError();
		UE_LOG(LogLhat, Error, TEXT("%s (%s): %s"), *GetPathName(), *ScriptPath, *LastError);
		SetComponentTickEnabled(false);
	}
}

void ULhatComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Instance)
	{
		UWorld* World = GetWorld();
		// BeginDestroy can deliver EndPlay after the world has already gone away.
		if (World && !IsGarbageCollecting())
		{
			if (auto* Runtime = World->GetSubsystem<ULhatSubsystem>()) Runtime->StopScript(Instance);
			else Instance->Stop();
		}
		else Instance->Stop(false);
		if (!Instance->GetError().IsEmpty()) LastError = Instance->GetError();
		Instance.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

FInstancedPropertyBag ULhatComponent::GetParameterOverrides() const
{
	FInstancedPropertyBag Overrides = Parameters;
	TArray<FName> Unchanged;
	if (Parameters.GetPropertyBagStruct())
	{
		for (const FPropertyBagPropertyDesc& Desc : Parameters.GetPropertyBagStruct()->GetPropertyDescs())
		{
			const auto Current = Parameters.GetValueSerializedString(Desc.Name);
			const auto Default = ParameterDefaults.GetValueSerializedString(Desc.Name);
			if (Current.IsValid() && Default.IsValid() && Current.GetValue() == Default.GetValue()) Unchanged.Add(Desc.Name);
		}
	}
	Overrides.RemovePropertiesByName(Unchanged);
	return Overrides;
}

void ULhatComponent::RefreshParameters()
{
#if WITH_EDITOR && LHAT_WITH_FRONTEND
	if (Instance) return;
	FLhatProgram Program(FLhatProgram::GetProjectScriptRoot());
	FInstancedPropertyBag Defaults;
	FString Error;
	if (!Program.ReadParameterDefaults(ScriptPath, Defaults, Error))
	{
		LastError = Error;
		UE_LOG(LogLhat, Error, TEXT("%s: %s"), *ScriptPath, *Error);
		return;
	}
	Modify();
	const FInstancedPropertyBag Overrides = GetParameterOverrides();
	Parameters = Defaults;
	if (ParameterScriptPath == ScriptPath && Defaults.GetPropertyBagStruct() && Overrides.GetPropertyBagStruct())
	{
		for (const FPropertyBagPropertyDesc& Desc : Defaults.GetPropertyBagStruct()->GetPropertyDescs())
		{
			const FPropertyBagPropertyDesc* Previous = Overrides.GetPropertyBagStruct()->FindPropertyDescByName(Desc.Name);
			if (Previous && Previous->ValueType == Desc.ValueType && Previous->ContainerTypes == Desc.ContainerTypes)
			{
				const auto Value = Overrides.GetValueSerializedString(Desc.Name);
				if (Value.IsValid()) Parameters.SetValueSerializedString(Desc.Name, Value.GetValue());
			}
		}
	}
	ParameterDefaults = MoveTemp(Defaults);
	ParameterScriptPath = ScriptPath;
	LastError.Empty();
#endif
}

#if WITH_EDITOR
void ULhatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(ULhatComponent, ScriptPath)) RefreshParameters();
	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif
