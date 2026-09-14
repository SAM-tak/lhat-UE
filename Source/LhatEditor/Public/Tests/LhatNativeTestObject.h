#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "LhatNativeTestObject.generated.h"

/** A normal Blueprint API fixture; no Lhat-specific exposure metadata. */
UCLASS(BlueprintType)
class LHATEDITOR_API ULhatNativeTestObject : public UObject
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static int32 Add(int32 A, int32 B) { return A + B; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static FString Echo(const FString& Text) { return Text; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static UObject* EchoObject(UObject* Object) { return Object; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static FVector ScaleVector(FVector Value, double Scale) { return Value * Scale; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	int32 ReadValue() const { return Value; }
	UFUNCTION(BlueprintCallable, Category = "Lhat|Tests")
	void SetValue(int32 NewValue) { Value = NewValue; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static uint8 ByteIdentity(uint8 Byte) { return Byte; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static FName NameIdentity(FName Name) { return Name; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static bool Not(bool Value) { return !Value; }
	UFUNCTION(BlueprintCallable, Category = "Lhat|Tests")
	void UnsupportedOut(int32& OutValue) { OutValue = Value; }
	virtual void ProcessEvent(UFunction* Function, void* Parameters) override
	{
		++ProcessEventCount;
		Super::ProcessEvent(Function, Parameters);
	}
	int32 ProcessEventCount = 0;
	int32 Value = 0;
};
