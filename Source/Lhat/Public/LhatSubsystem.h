#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "LhatSubsystem.generated.h"

class AActor;
class FLhatProgram;
class FLhatInstance;
struct FInstancedPropertyBag;

/** Compiled scripts are shared only inside one game/PIE world. Each component has its own VM. */
UCLASS()
class LHAT_API ULhatSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	TSharedPtr<FLhatInstance> StartScript(const FString& ScriptPath, AActor* Owner, const FInstancedPropertyBag& Parameters, FString& Error);
	void StopScript(const TSharedPtr<FLhatInstance>& Instance);
	virtual void Deinitialize() override;
protected:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
private:
	TMap<FString, TSharedPtr<FLhatProgram>> Programs;
	TArray<TSharedPtr<FLhatInstance>> Instances;
};
