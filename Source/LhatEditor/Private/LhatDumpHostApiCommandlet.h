#pragma once

#include "Commandlets/Commandlet.h"
#include "LhatDumpHostApiCommandlet.generated.h"

/** Export the UE host API without opening an editor window or starting PIE. */
UCLASS()
class ULhatDumpHostApiCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	ULhatDumpHostApiCommandlet();
	virtual int32 Main(const FString& Params) override;
};
