#include "LhatSurveyBlueprintApiCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "LhatModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UnrealType.h"

namespace
{
	using FJson = TSharedPtr<FJsonObject>;
	using FValues = TArray<TSharedPtr<FJsonValue>>;
	void AppendJson(FValues& Values, const FJson& Object) { Values.Add(MakeShared<FJsonValueObject>(Object)); }
	FJson Metadata(const UField* Field)
	{
		auto Json = MakeShared<FJsonObject>();
		for (const TCHAR* Key : { TEXT("ToolTip"), TEXT("DisplayName"), TEXT("BlueprintInternalUseOnly"), TEXT("DeprecatedFunction"), TEXT("WorldContext"), TEXT("Latent"), TEXT("LatentInfo") })
			if (Field->HasMetaData(Key)) Json->SetStringField(Key, Field->GetMetaData(Key));
		return Json;
	}
	void References(const FProperty* Property, FValues& Values)
	{
		const UObject* Type = nullptr;
		if (const auto* Object = CastField<FObjectPropertyBase>(Property)) Type = Object->PropertyClass;
		if (const auto* Class = CastField<FClassProperty>(Property)) Type = Class->MetaClass;
		if (const auto* Struct = CastField<FStructProperty>(Property)) Type = Struct->Struct;
		if (const auto* Enum = CastField<FEnumProperty>(Property)) Type = Enum->GetEnum();
		if (const auto* Byte = CastField<FByteProperty>(Property)) Type = Byte->Enum;
		if (const auto* Interface = CastField<FInterfaceProperty>(Property)) Type = Interface->InterfaceClass;
		if (Type) Values.Add(MakeShared<FJsonValueString>(Type->GetPathName()));
		if (const auto* Array = CastField<FArrayProperty>(Property)) References(Array->Inner, Values);
		if (const auto* Set = CastField<FSetProperty>(Property)) References(Set->ElementProp, Values);
		if (const auto* Map = CastField<FMapProperty>(Property)) { References(Map->KeyProp, Values); References(Map->ValueProp, Values); }
	}
	FJson PropertyJson(const FProperty* Property)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Property->GetName());
		Json->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		Json->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
		FString Flags;
		const auto Flag = [&](EPropertyFlags Value, const TCHAR* Name) { if (Property->HasAnyPropertyFlags(Value)) { if (!Flags.IsEmpty()) Flags += TEXT(", "); Flags += Name; } };
		Flag(CPF_Parm, TEXT("Parm")); Flag(CPF_ReturnParm, TEXT("ReturnParm"));
		Flag(CPF_OutParm, TEXT("OutParm")); Flag(CPF_ReferenceParm, TEXT("ReferenceParm")); Flag(CPF_ConstParm, TEXT("ConstParm"));
		Flag(CPF_BlueprintVisible, TEXT("BlueprintVisible")); Flag(CPF_BlueprintReadOnly, TEXT("BlueprintReadOnly"));
		Flag(CPF_BlueprintAssignable, TEXT("BlueprintAssignable")); Flag(CPF_BlueprintCallable, TEXT("BlueprintCallable"));
		Flag(CPF_NativeAccessSpecifierPrivate, TEXT("NativeAccessSpecifierPrivate"));
		Flag(CPF_NativeAccessSpecifierProtected, TEXT("NativeAccessSpecifierProtected"));
		Flag(CPF_EditorOnly, TEXT("EditorOnly"));
		Json->SetStringField(TEXT("flags"), Flags);
		Json->SetStringField(TEXT("define_scope"), Property->HasAnyPropertyFlags(CPF_EditorOnly) ? TEXT("EditorOnlyData") : TEXT("None"));
		if (Property->ArrayDim > 1) Json->SetStringField(TEXT("array_dimension"), FString::FromInt(Property->ArrayDim));
		FValues Refs; References(Property, Refs); Json->SetArrayField(TEXT("references"), Refs);
		auto Meta = MakeShared<FJsonObject>();
		if (Property->HasMetaData(TEXT("ToolTip"))) Meta->SetStringField(TEXT("ToolTip"), Property->GetMetaData(TEXT("ToolTip")));
		Json->SetObjectField(TEXT("metadata"), Meta);
		return Json;
	}
	FJson ClassJson(const UBlueprint* Blueprint)
	{
		const UClass* Class = Blueprint->GeneratedClass;
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("kind"), TEXT("class"));
		Json->SetStringField(TEXT("name"), Class->GetName());
		Json->SetStringField(TEXT("cpp_name"), Class->GetName());
		Json->SetStringField(TEXT("path"), Class->GetPathName());
		Json->SetStringField(TEXT("asset"), Blueprint->GetPathName());
		Json->SetStringField(TEXT("base_path"), GetPathNameSafe(Class->GetSuperClass()));
		Json->SetBoolField(TEXT("editor_only_asset"), Blueprint->IsEditorOnly());
		Json->SetObjectField(TEXT("metadata"), Metadata(Class));
		FValues Functions, Properties;
		// Store declarations once; inherited members are resolved through base_path.
		for (TFieldIterator<UFunction> It(Class, EFieldIterationFlags::None); It; ++It)
		{
			const UFunction* Function = *It;
			auto Fn = MakeShared<FJsonObject>();
			Fn->SetStringField(TEXT("name"), Function->GetName());
			Fn->SetStringField(TEXT("path"), Function->GetPathName());
			FString Flags;
			const auto Flag = [&](EFunctionFlags Value, const TCHAR* Name) { if (Function->HasAnyFunctionFlags(Value)) { if (!Flags.IsEmpty()) Flags += TEXT(", "); Flags += Name; } };
			Flag(FUNC_BlueprintCallable, TEXT("BlueprintCallable")); Flag(FUNC_BlueprintPure, TEXT("BlueprintPure"));
			Flag(FUNC_BlueprintEvent, TEXT("BlueprintEvent")); Flag(FUNC_Delegate, TEXT("Delegate"));
			Flag(FUNC_Static, TEXT("Static")); Flag(FUNC_Const, TEXT("Const")); Flag(FUNC_Native, TEXT("Native"));
			Flag(FUNC_Private, TEXT("Private")); Flag(FUNC_Protected, TEXT("Protected")); Flag(FUNC_Net, TEXT("Net"));
			Fn->SetStringField(TEXT("flags"), Flags);
			Fn->SetObjectField(TEXT("metadata"), Metadata(Function));
			FValues Parameters;
			for (TFieldIterator<FProperty> Prop(Function, EFieldIterationFlags::None); Prop; ++Prop)
				if (Prop->HasAnyPropertyFlags(CPF_Parm)) AppendJson(Parameters, PropertyJson(*Prop));
			Fn->SetArrayField(TEXT("parameters"), Parameters);
			AppendJson(Functions, Fn);
		}
		for (TFieldIterator<FProperty> Prop(Class, EFieldIterationFlags::None); Prop; ++Prop) AppendJson(Properties, PropertyJson(*Prop));
		Json->SetArrayField(TEXT("functions"), Functions);
		Json->SetArrayField(TEXT("properties"), Properties);
		return Json;
	}
}

ULhatSurveyBlueprintApiCommandlet::ULhatSurveyBlueprintApiCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 ULhatSurveyBlueprintApiCommandlet::Main(const FString& Params)
{
	FString Output;
	if (!FParse::Value(*Params, TEXT("Output="), Output) || Output.IsEmpty())
	{
		UE_LOG(LogLhat, Error, TEXT("Specify a new file with -Output=Saved/.../blueprints.json"));
		return 2;
	}
	Output = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Output);
	if (IFileManager::Get().FileExists(*Output)) { UE_LOG(LogLhat, Error, TEXT("Survey output already exists: %s"), *Output); return 2; }
	auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.SearchAllAssets(true);
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName()); Filter.bRecursiveClasses = true;
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString(); });
	UE_LOG(LogLhat, Display, TEXT("Survey: found %d Blueprint assets in mounted content roots"), Assets.Num());
	FValues Types, Failures, WithoutClass;
	for (int32 Index = 0; Index < Assets.Num(); ++Index)
	{
		const auto* Blueprint = Cast<UBlueprint>(Assets[Index].GetAsset());
		if (!Blueprint) Failures.Add(MakeShared<FJsonValueString>(Assets[Index].GetSoftObjectPath().ToString()));
		else if (!Blueprint->GeneratedClass) WithoutClass.Add(MakeShared<FJsonValueString>(Assets[Index].GetSoftObjectPath().ToString()));
		else AppendJson(Types, ClassJson(Blueprint));
		if ((Index + 1) % 32 == 0)
		{
			UE_LOG(LogLhat, Display, TEXT("Survey: inspected %d / %d Blueprint assets"), Index + 1, Assets.Num());
			CollectGarbage(RF_NoFlags);
		}
	}
	auto Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("scope"), TEXT("Saved standalone UBlueprint assets in mounted roots; no map-embedded level scripts or unsaved editor changes. No assets are saved."));
	Root->SetNumberField(TEXT("asset_count"), Assets.Num());
	Root->SetArrayField(TEXT("types"), Types); Root->SetArrayField(TEXT("load_failures"), Failures); Root->SetArrayField(TEXT("without_generated_class"), WithoutClass);
	FString Json; FJsonSerializer::Serialize(Root, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output), true) || !FFileHelper::SaveStringToFile(Json, *Output, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 1;
	UE_LOG(LogLhat, Display, TEXT("Survey: %d classes, %d failures, %d without generated class; written to %s"), Types.Num(), Failures.Num(), WithoutClass.Num(), *Output);
	return Failures.IsEmpty() ? 0 : 1;
}
