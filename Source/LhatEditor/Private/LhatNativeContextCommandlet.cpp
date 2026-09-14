#include "LhatNativeContextCommandlet.h"

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

ULhatNativeContextCommandlet::ULhatNativeContextCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 ULhatNativeContextCommandlet::Main(const FString& Params)
{
	FString Output;
	if (!FParse::Value(*Params, TEXT("Output="), Output) || Output.IsEmpty()) return 2;
	Output = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Output);
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1);
	Root->SetStringField(TEXT("project"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
	Root->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
	TSharedRef<FJsonObject> Modules = MakeShared<FJsonObject>();
	TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetDiscoveredPlugins();
	Plugins.Sort([](const TSharedRef<IPlugin>& A, const TSharedRef<IPlugin>& B) { return A->GetName() < B->GetName(); });
	for (const TSharedRef<IPlugin>& Plugin : Plugins)
	{
		for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("plugin"), Plugin->GetName());
			Entry->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
			Entry->SetBoolField(TEXT("explicitly_loaded"), Plugin->GetDescriptor().bExplicitlyLoaded);
			Modules->SetObjectField(Module.Name.ToString(), Entry);
		}
	}
	Root->SetObjectField(TEXT("modules"), Modules);
	FString Json;
	if (!FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json))) return 1;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output), true);
	return FFileHelper::SaveStringToFile(Json, *Output, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ? 0 : 1;
}
