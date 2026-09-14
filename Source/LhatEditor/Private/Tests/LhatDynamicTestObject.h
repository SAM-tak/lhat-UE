#pragma once

#include "CoreMinimal.h"
#include "Engine/LatentActionManager.h"
#include "GameFramework/Actor.h"
#include "UObject/Object.h"
#include "LhatDynamicTestObject.generated.h"

// Private, unexported C++ class: deliberately cannot be statically generated.
UCLASS(BlueprintType, Blueprintable)
class ULhatDynamicTestObject : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, Category = "Lhat|Tests")
	int32 Value = 3;
	UPROPERTY(BlueprintReadOnly, Category = "Lhat|Tests")
	FString ReadOnly = TEXT("read only");
	UPROPERTY(BlueprintReadWrite, Category = "Lhat|Tests")
	uint8 Flag : 1;
	uint8 AdjacentFlag : 1;

	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static int64 Echo64(int64 N) { return N; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static uint8 ByteIdentity(uint8 N) { return N; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static FString Echo(const FString& Text) { return Text; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static FText TextEcho(FText Text) { return Text; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static UObject* EchoObject(UObject* Object) { return Object; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static TSubclassOf<AActor> ActorClass(TSubclassOf<AActor> Class) { return Class; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static FVector Scale(FVector Vector, double Factor) { return Vector * Factor; }
	UFUNCTION(BlueprintCallable, Category = "Lhat|Tests")
	bool Results(int32 N, int32& Twice, FString& Text) { Twice = N * 2; Text = TEXT("ok"); return true; }
	UFUNCTION(BlueprintCallable, Category = "Lhat|Tests")
	void Bump(UPARAM(ref) int32& N) { N += Value; }
	UFUNCTION(BlueprintCallable, Category = "Lhat|Tests")
	void EmptyTransform(FTransform& Out) {}
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Lhat|Tests")
	int32 Compute(int32 Input);
	virtual int32 Compute_Implementation(int32 Input) { return Input + 1; }
	UFUNCTION(BlueprintPure, Category = "Lhat|Tests")
	static int32 UnsupportedArray(const TArray<int32>& Values) { return Values.Num(); }
	UFUNCTION(BlueprintCallable, Category = "Lhat|Tests", meta = (Latent, LatentInfo = "Info", WorldContext = "World"))
	static void LatentStub(UObject* World, FLatentActionInfo Info) {}
	virtual void ProcessEvent(UFunction* Function, void* Parameters) override
	{
		++ProcessEventCount;
		Super::ProcessEvent(Function, Parameters);
	}
	int32 ProcessEventCount = 0;
};
