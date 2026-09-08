#pragma once

#include "LhatScript.h"

class AActor;
struct FInstancedPropertyBag;

/** One component's machine and rooted state. Its program always outlives the machine. */
class LHAT_API FLhatInstance final
{
public:
	explicit FLhatInstance(TSharedRef<FLhatProgram> InProgram);
	~FLhatInstance();
	FLhatInstance(const FLhatInstance&) = delete;
	FLhatInstance& operator=(const FLhatInstance&) = delete;

	bool Start(const LhatUnit* Unit, AActor* Owner, const FInstancedPropertyBag* Parameters = nullptr);
	bool Tick(double DeltaSeconds);
	void Stop(bool bDispatchEndPlay = true);
	bool IsRunning() const { return Machine && bStarted && !bFaulted; }
	bool HasTick() const { return bHasTick; }
	const FString& GetError() const { return Error; }
	LhatMachine* GetMachine() const { return Machine; }
	LhatValue GetState() const { return State; }

private:
	bool Accept(const LhatRunResult& Result, const TCHAR* Operation);
	bool Call(const char* Name, const LhatValue* Arguments, size_t Count);
	bool HasMember(const char* Name) const;
	TSharedRef<FLhatProgram> Program;
	LhatMachine* Machine = nullptr;
	LhatValue State = lhat_nil();
	FString Error;
	bool bStarted = false;
	bool bStartAttempted = false;
	bool bFaulted = false;
	bool bHasTick = false;
};
