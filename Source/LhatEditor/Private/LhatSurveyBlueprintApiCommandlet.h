#pragma once

#include "Commandlets/Commandlet.h"
#include "LhatSurveyBlueprintApiCommandlet.generated.h"

/** Read-only inventory for API-size experiments; does not register executable bindings. */
UCLASS()
class ULhatSurveyBlueprintApiCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	ULhatSurveyBlueprintApiCommandlet();
	virtual int32 Main(const FString& Params) override;
};
