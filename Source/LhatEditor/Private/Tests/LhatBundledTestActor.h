#pragma once

#include "GameFramework/Actor.h"
#include "LhatBundledTestActor.generated.h"

/** Inherits real Engine APIs; deliberately has no project-generated bindings. */
UCLASS()
class ALhatBundledTestActor : public AActor
{
	GENERATED_BODY()
public:
	virtual void ProcessEvent(UFunction* Function, void* Parameters) override
	{
		++ProcessEventCount;
		Super::ProcessEvent(Function, Parameters);
	}
	int32 ProcessEventCount = 0;
};
