#pragma once

#include "CoreMinimal.h"

class FLhatBindings;
struct FLhatNativeType;
struct LhatProgram;

/** Program-owned reflection cache; each invocation has independent argument storage. */
class FLhatDynamicBindings final
{
public:
	explicit FLhatDynamicBindings(FLhatBindings& Bindings);
	~FLhatDynamicBindings();
	bool Gather(TArray<FLhatNativeType>& Types);
	bool Register(LhatProgram* Program);
	FString Report() const;
private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
