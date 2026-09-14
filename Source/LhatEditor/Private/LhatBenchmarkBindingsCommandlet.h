#pragma once

#include "Commandlets/Commandlet.h"
#include "LhatBenchmarkBindingsCommandlet.generated.h"

/** Opt-in, headless end-to-end Lhat native binding benchmark. */
UCLASS()
class ULhatBenchmarkBindingsCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	ULhatBenchmarkBindingsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
