#pragma once

#include "CoreMinimal.h"

THIRD_PARTY_INCLUDES_START
#include "lhat.h"
THIRD_PARTY_INCLUDES_END

/**
 * Owns one L^ program whose source files are read below ScriptRoot.
 *
 * Native registrations must be added after construction and before Check().
 * The native L^ API remains available through lhat.h for those registrations.
 */
class LHAT_API FLhatProgram final
{
public:
	explicit FLhatProgram(const FString& ScriptRoot, bool bStrict = true);
	~FLhatProgram();

	FLhatProgram(const FLhatProgram&) = delete;
	FLhatProgram& operator=(const FLhatProgram&) = delete;
	FLhatProgram(FLhatProgram&&) = delete;
	FLhatProgram& operator=(FLhatProgram&&) = delete;

	bool IsValid() const;
	const LhatUnit* Check(const FString& EntryPoint);
	bool Compile();
	LhatMachine* CreateMachine() const;
	bool Install(LhatMachine* Machine) const;
	LhatProgram* GetNativeHandle() const;
	FString GetDiagnostics() const;
	class FLhatBindings& GetBindings() const;
	static FString GetProjectScriptRoot();
	bool ReadParameterDefaults(const FString& EntryPoint, struct FInstancedPropertyBag& Out, FString& Error);

	static void DestroyMachine(LhatMachine* Machine);

private:
	struct FLoaderContext;

	static char* LoadScript(void* Context, const char* Path, size_t* OutLength);

	TUniquePtr<FLoaderContext> LoaderContext;
	TUniquePtr<class FLhatBindings> Bindings;
	LhatProgram* Program = nullptr;
	FString InitializationError;
};
