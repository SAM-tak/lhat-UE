#pragma once

#include "CoreMinimal.h"
#include "LhatScript.h"

/** The registration context belongs to the program and outlives all its machines. */
class LHAT_API FLhatBindings final
{
public:
	FLhatBindings();
	~FLhatBindings();
	FLhatBindings(const FLhatBindings&) = delete;
	FLhatBindings& operator=(const FLhatBindings&) = delete;
	bool Register(LhatProgram* Program);
	bool RegisterFunction(LhatProgram* Program, UClass* Class, const char* Type, const char* Name, const TCHAR* EngineName);
	bool WrapObject(LhatMachine* Machine, UObject* Object, LhatValue& Out) const;
	UObject* ReadObject(LhatMachine* Machine, LhatValue Value, UClass* ExpectedClass) const;
	bool RegisterNativeTypes(LhatProgram* Program, TArray<struct FLhatNativeType>& Types);
	bool GatherDynamicTypes(TArray<struct FLhatNativeType>& Types);
	bool RegisterDynamicFunctions(LhatProgram* Program);
	/** Dispatch inventory and unsupported-signature reasons as JSON. */
	FString GetDynamicBindingReport() const;
	const LhatHostValueTag* GetValueTag(const char* Name) const;
	const LhatHostDataTag* ObjectTag = nullptr;
	const LhatHostDataTag* ActorTag = nullptr;
	const LhatHostValueTag* VectorTag = nullptr;
private:
	struct FObjectType { const LhatHostDataTag* Tag; const char* Module; const char* Name; };
	TMap<UClass*, FObjectType> ObjectTypes;
	TMap<FString, const LhatHostValueTag*> ValueTypes;
	TArray<TUniquePtr<struct FLhatReflectedCall>> Calls;
	TUniquePtr<class FLhatDynamicBindings> Dynamic;
};
