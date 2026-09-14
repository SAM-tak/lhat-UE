#pragma once

#include "Commandlets/Commandlet.h"
#include "LhatNativeContextCommandlet.generated.h"

/** Capture the editor's actual enabled plugins without constructing a Lhat program. */
UCLASS()
class ULhatNativeContextCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	ULhatNativeContextCommandlet();
	virtual int32 Main(const FString& Params) override;
};
