#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "LhatDumpHostApiCommandlet.h"
#include "LhatHostApiExport.h"
#include "LhatScript.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FString ReadText(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}

	TSharedPtr<FJsonObject> FindEntry(const TSharedPtr<FJsonObject>& Json, const TCHAR* ArrayName, const TCHAR* Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Json->TryGetArrayField(ArrayName, Entries)) return nullptr;
		for (const TSharedPtr<FJsonValue>& Entry : *Entries)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString EntryName;
			if (Entry->TryGetObject(Object) && (*Object)->TryGetStringField(TEXT("name"), EntryName) && EntryName == Name) return *Object;
		}
		return nullptr;
	}

	struct FExportFixture
	{
		FString RelativeDirectory = TEXT("Saved/LhatHostApiTest ") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RelativeDirectory);
		TArray<FString> Files;
		FString File(const TCHAR* Name)
		{
			FString Path = Directory / Name;
			Files.Add(Path);
			return Path;
		}
		~FExportFixture()
		{
			for (const FString& Path : Files)
			{
				FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Path, false);
				IFileManager::Get().Delete(*Path, false, false, true);
			}
			// No recursive deletion: a failed export must not leave a temp file behind.
			IFileManager::Get().DeleteDirectory(*Directory, false, false);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatHostApiJsonTest, "Lhat.Editor.HostApiJson", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatHostApiJsonTest::RunTest(const FString&)
{
	// The folder need not exist: export must not load scripts, create a VM, or enter PIE.
	FLhatProgram Program(FPaths::ProjectSavedDir() / TEXT("UnusedHostApiSourceRoot"));
	TArray<uint8> Bytes, Again;
	FString Error;
	if (!TestTrue(TEXT("Dump registered API"), Program.GetHostApiJson(Bytes, Error))) { AddError(Error); return false; }
	if (!TestTrue(TEXT("JSON has a body"), Bytes.Num() > 2)) return false;
	TestEqual(TEXT("No UTF-8 BOM"), Bytes[0], static_cast<uint8>('{'));
	TestFalse(TEXT("No trailing NUL in file payload"), Bytes.Last() == 0);
	const FUTF8ToTCHAR Converted(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num());
	const FString Text(Converted.Length(), Converted.Get());
	TSharedPtr<FJsonObject> Json;
	if (!TestTrue(TEXT("Parse exported JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json))) return false;
	TestTrue(TEXT("Runtime strict setting is exported"), Json->GetBoolField(TEXT("strict")));
	const auto Actor = FindEntry(Json, TEXT("types"), TEXT("Actor"));
	const auto Vector = FindEntry(Json, TEXT("types"), TEXT("Vector"));
	if (!TestTrue(TEXT("UE types are exported"), Actor.IsValid() && Vector.IsValid())) return false;
	TestEqual(TEXT("Actor inheritance is preserved"), Actor->GetStringField(TEXT("base_name")), FString(TEXT("Object")));
	TestEqual(TEXT("FVector uses native size"), Vector->GetIntegerField(TEXT("size")), static_cast<int32>(sizeof(FVector)));
	TestTrue(TEXT("Vector constructor is exported"), FindEntry(Json, TEXT("functions"), TEXT("MakeVector")).IsValid());
	TestTrue(TEXT("Reflected method is exported"), FindEntry(Json, TEXT("functions"), TEXT("GetActorLocation")).IsValid());
	TestTrue(TEXT("Annotation is exported"), FindEntry(Json, TEXT("annotations"), TEXT("EditAnywhere")).IsValid());
	TestTrue(TEXT("Repeated dump succeeds"), Program.GetHostApiJson(Again, Error));
	TestTrue(TEXT("Repeated dump is deterministic"), Bytes == Again);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatHostApiExportTest, "Lhat.Editor.HostApiExport", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatHostApiExportTest::RunTest(const FString&)
{
	// Check the default without overwriting the project's actual host API file.
	TestEqual(TEXT("Default output is beside the uproject file"), LhatHostApi::GetDefaultOutputPath(),
		FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), TEXT("lhat-host.json")));
	FExportFixture Fixture;
	const FString ExportFile = Fixture.File(TEXT("lhat-host.json"));
	const FString ConsoleFile = Fixture.File(TEXT("console host.json"));
	const FString CommandletFile = Fixture.File(TEXT("commandlet host.json"));
	FString Output, Error;
	if (!TestTrue(TEXT("Relative-path export creates parents"), LhatHostApi::Export(Fixture.RelativeDirectory / TEXT("lhat-host.json"), Output, Error))) { AddError(Error); return false; }
	TestEqual(TEXT("Relative path uses ProjectDir"), Output, ExportFile);
	const FString Expected = ReadText(ExportFile);
	TestTrue(TEXT("Export replaces an existing generated file"), LhatHostApi::Export(ExportFile, Output, Error));
	TestEqual(TEXT("Replacement is deterministic"), ReadText(ExportFile), Expected);

	const FString Command = FString::Printf(TEXT("Lhat.DumpHostApi \"%s\""), *ConsoleFile);
	TestTrue(TEXT("Editor console command is registered"), IConsoleManager::Get().ProcessUserConsoleInput(*Command, *GLog, nullptr));
	TestEqual(TEXT("Console exports the same runtime API"), ReadText(ConsoleFile), Expected);
	auto* Commandlet = NewObject<ULhatDumpHostApiCommandlet>();
	TestEqual(TEXT("Commandlet returns success"), Commandlet->Main(FString::Printf(TEXT("-Output=\"%s\""), *CommandletFile)), 0);
	TestEqual(TEXT("Commandlet exports the same runtime API"), ReadText(CommandletFile), Expected);
	AddExpectedError(TEXT("Output requires a nonempty path"), EAutomationExpectedErrorFlags::Contains, 1);
	TestEqual(TEXT("Explicitly empty output is rejected"), Commandlet->Main(TEXT("-Output=\"\"")), 2);

	TestFalse(TEXT("Directory output is rejected"), LhatHostApi::Export(Fixture.Directory, Output, Error));
	TestFalse(TEXT("Failure explains the path problem"), Error.IsEmpty());
	if (TestTrue(TEXT("Mark generated fixture read-only"), FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*ExportFile, true)))
	{
		TestFalse(TEXT("Read-only destination is not overwritten"), LhatHostApi::Export(ExportFile, Output, Error));
		TestEqual(TEXT("Failed replace preserves the old file"), ReadText(ExportFile), Expected);
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*ExportFile, false);
	}
	TArray<FString> TemporaryFiles;
	IFileManager::Get().FindFiles(TemporaryFiles, *(Fixture.Directory / TEXT(".lhat-host-*.tmp")), true, false);
	TestTrue(TEXT("Temporary files are cleaned up"), TemporaryFiles.IsEmpty());
	return true;
}

#endif
