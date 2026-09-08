#pragma once

#include "Components/ActorComponent.h"
#include "StructUtils/PropertyBag.h"
#include "LhatComponent.generated.h"

class FLhatInstance;

UCLASS(ClassGroup=(Scripting), meta=(BlueprintSpawnableComponent))
class LHAT_API ULhatComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	ULhatComponent();
	virtual ~ULhatComponent() override;

	/** Path relative to the project's Script directory, e.g. Mover.lh. */
	UPROPERTY(EditAnywhere, Category="Lhat")
	FString ScriptPath;

	UPROPERTY(EditAnywhere, Category="Lhat", meta=(FixedLayout))
	FInstancedPropertyBag Parameters;

	UPROPERTY(VisibleInstanceOnly, Transient, Category="Lhat")
	FString LastError;

	/** Re-read declaration defaults after editing the .lh file. Preserves compatible overrides. */
	UFUNCTION(CallInEditor, Category="Lhat")
	void RefreshParameters();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
private:
	FInstancedPropertyBag GetParameterOverrides() const;
	UPROPERTY()
	FInstancedPropertyBag ParameterDefaults;
	UPROPERTY()
	FString ParameterScriptPath;
	TSharedPtr<FLhatInstance> Instance;
};
