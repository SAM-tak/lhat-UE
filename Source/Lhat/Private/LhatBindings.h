#pragma once

#include "CoreMinimal.h"
#include "LhatScript.h"

/** The registration context belongs to the program and outlives all its machines. */
class FLhatBindings final
{
public:
	FLhatBindings();
	~FLhatBindings();
	bool Register(LhatProgram* Program);
	bool RegisterFunction(LhatProgram* Program, UClass* Class, const char* Type, const char* Name, const TCHAR* EngineName);
	bool WrapObject(LhatMachine* Machine, UObject* Object, LhatValue& Out) const;
	UObject* ReadObject(LhatMachine* Machine, LhatValue Value, UClass* ExpectedClass) const;
	const LhatHostDataTag* ObjectTag = nullptr;
	const LhatHostDataTag* ActorTag = nullptr;
	const LhatHostValueTag* VectorTag = nullptr;
private:
	TArray<TUniquePtr<struct FLhatReflectedCall>> Calls;
};
