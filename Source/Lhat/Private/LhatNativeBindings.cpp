#include "LhatNativeBindings.h"

#include "GameFramework/Actor.h"
#include "LhatModule.h"
#include "Interfaces/IProjectManager.h"
#include "ProjectDescriptor.h"
#include "Modules/ModuleManager.h"

// Linked into the Lhat plugin itself; no project module or startup ordering needed.
const FLhatNativeBindingProvider& LhatBundledEngineProvider();

namespace
{
	TArray<const FLhatNativeBindingProvider*>& Providers()
	{
		static TArray<const FLhatNativeBindingProvider*> Value;
		return Value;
	}

	int32 Depth(UClass* Class)
	{
		int32 N = 0;
		for (; Class; Class = Class->GetSuperClass()) ++N;
		return N;
	}
}

bool LhatUEBindings::LoadGeneratedProviders(FString& Error)
{
	check(IsInGameThread());
	const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
	if (!Project) return true;
	for (const FModuleDescriptor& Module : Project->Modules)
	{
		bool bGenerated = Module.Name == TEXT("LhatGeneratedRuntime");
#if WITH_EDITOR
		bGenerated |= Module.Name == TEXT("LhatGeneratedEditor");
#endif
		if (bGenerated && !FModuleManager::Get().LoadModulePtr<IModuleInterface>(Module.Name))
		{
			Error = FString::Printf(TEXT("Could not load %s. Regenerate and rebuild the native bindings."), *Module.Name.ToString());
			return false;
		}
	}
	return true;
}

void LhatUEBindings::AddProvider(const FLhatNativeBindingProvider* Provider)
{
	check(IsInGameThread());
	check(Provider);
	check(!Providers().Contains(Provider));
	Providers().Add(Provider);
}

void LhatUEBindings::RemoveProvider(const FLhatNativeBindingProvider* Provider)
{
	check(IsInGameThread());
	Providers().Remove(Provider);
}

bool LhatUEBindings::RegisterProviders(LhatProgram* Program, FLhatBindings& Bindings)
{
	TArray<const FLhatNativeBindingProvider*> Ordered = Providers();
	Ordered.Sort([](const FLhatNativeBindingProvider& A, const FLhatNativeBindingProvider& B) { return FCString::Strcmp(A.Name, B.Name) < 0; });
	TArray<FLhatNativeType> Types;
	const auto& Bundled = LhatBundledEngineProvider();
	Bundled.GatherTypes(Types);
	for (const auto* Provider : Ordered) Provider->GatherTypes(Types);
	if (!Bindings.GatherDynamicTypes(Types)) return false;
	if (!Bindings.RegisterNativeTypes(Program, Types)) return false;
	if (!Bundled.RegisterFunctions(Program, Bindings)) return false;
	for (const auto* Provider : Ordered)
	{
		if (!Provider->RegisterFunctions(Program, Bindings))
		{
			UE_LOG(LogLhat, Error, TEXT("Native registration failed: %s"), Provider->Name);
			return false;
		}
	}
	return Bindings.RegisterDynamicFunctions(Program);
}

bool FLhatBindings::RegisterNativeTypes(LhatProgram* Program, TArray<FLhatNativeType>& Types)
{
	ObjectTypes.Add(UObject::StaticClass(), {ObjectTag, "ue", "Object"});
	ObjectTypes.Add(AActor::StaticClass(), {ActorTag, "ue", "Actor"});
	for (const FLhatNativeType& Type : Types)
		if (!Type.Class || !Type.Module || !Type.Name) return false;
	Types.Sort([](const FLhatNativeType& A, const FLhatNativeType& B)
	{
		const int32 AD = Depth(A.Class), BD = Depth(B.Class);
		return AD == BD ? A.Class->GetPathName() < B.Class->GetPathName() : AD < BD;
	});
	for (const FLhatNativeType& Type : Types)
	{
		// Both the bundle and an optional project provider may need the same
		// native type. Share its one tag, but never accept conflicting names.
		if (const FObjectType* Existing = ObjectTypes.Find(Type.Class))
		{
			if (FCStringAnsi::Strcmp(Existing->Module, Type.Module) || FCStringAnsi::Strcmp(Existing->Name, Type.Name)) return false;
			continue;
		}
		const FObjectType* Base = nullptr;
		for (UClass* Class = Type.Class->GetSuperClass(); Class && !Base; Class = Class->GetSuperClass()) Base = ObjectTypes.Find(Class);
		if (!Base) return false;
		const auto* Tag = lhat_register_hostdata_subtype(Program, Type.Module, Type.Name, Base->Module, Base->Name);
		if (!Tag) return false;
		ObjectTypes.Add(Type.Class, {Tag, Type.Module, Type.Name});
	}
	ValueTypes.Add(TEXT("Vector"), VectorTag);
	return LhatUEBindings::RegisterMathTypes(Program, ValueTypes);
}

const LhatHostValueTag* FLhatBindings::GetValueTag(const char* Name) const
{
	const auto* Found = ValueTypes.Find(UTF8_TO_TCHAR(Name));
	return Found ? *Found : nullptr;
}

bool LhatUEBindings::RegisterMathTypes(LhatProgram* Program, TMap<FString, const LhatHostValueTag*>& Tags)
{
	// These are plain value types. FText and other owning structs require separate codecs.
	struct FType { const char* Name; size_t Size; };
	const FType Types[] = {{"Vector2D", sizeof(FVector2D)}, {"Rotator", sizeof(FRotator)},
		{"Quat", sizeof(FQuat)}, {"Transform", sizeof(FTransform)}, {"LinearColor", sizeof(FLinearColor)}, {"Color", sizeof(FColor)}};
	for (const FType& Type : Types)
	{
		const auto* Tag = lhat_register_hostvalue_type(Program, "ue", Type.Name, Type.Size);
		if (!Tag) return false;
		Tags.Add(UTF8_TO_TCHAR(Type.Name), Tag);
	}
	struct FField { const char* Type; const char* Name; size_t Offset; LhatHostValueFieldKind Kind; };
	const FField Fields[] = {
		{"Vector2D", "X", STRUCT_OFFSET(FVector2D, X), LHAT_HVFIELD_F64}, {"Vector2D", "Y", STRUCT_OFFSET(FVector2D, Y), LHAT_HVFIELD_F64},
		{"Rotator", "Pitch", STRUCT_OFFSET(FRotator, Pitch), LHAT_HVFIELD_F64}, {"Rotator", "Yaw", STRUCT_OFFSET(FRotator, Yaw), LHAT_HVFIELD_F64}, {"Rotator", "Roll", STRUCT_OFFSET(FRotator, Roll), LHAT_HVFIELD_F64},
		{"Quat", "X", STRUCT_OFFSET(FQuat, X), LHAT_HVFIELD_F64}, {"Quat", "Y", STRUCT_OFFSET(FQuat, Y), LHAT_HVFIELD_F64}, {"Quat", "Z", STRUCT_OFFSET(FQuat, Z), LHAT_HVFIELD_F64}, {"Quat", "W", STRUCT_OFFSET(FQuat, W), LHAT_HVFIELD_F64},
		{"LinearColor", "R", STRUCT_OFFSET(FLinearColor, R), LHAT_HVFIELD_F32}, {"LinearColor", "G", STRUCT_OFFSET(FLinearColor, G), LHAT_HVFIELD_F32}, {"LinearColor", "B", STRUCT_OFFSET(FLinearColor, B), LHAT_HVFIELD_F32}, {"LinearColor", "A", STRUCT_OFFSET(FLinearColor, A), LHAT_HVFIELD_F32},
		{"Color", "R", STRUCT_OFFSET(FColor, R), LHAT_HVFIELD_U8}, {"Color", "G", STRUCT_OFFSET(FColor, G), LHAT_HVFIELD_U8}, {"Color", "B", STRUCT_OFFSET(FColor, B), LHAT_HVFIELD_U8}, {"Color", "A", STRUCT_OFFSET(FColor, A), LHAT_HVFIELD_U8}
	};
	for (const FField& Field : Fields)
		if (!lhat_register_hostvalue_field(Program, "ue", Field.Type, Field.Name, Field.Offset, Field.Kind)) return false;
	return true;
}
