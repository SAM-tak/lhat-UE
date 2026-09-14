#include "Misc/AutomationTest.h"
#include "LhatScript.h"

#if WITH_DEV_AUTOMATION_TESTS && LHAT_WITH_FRONTEND

#include "LhatNativeBindings.h"
#include "GameFramework/Actor.h"
#include "Tests/LhatNativeTestObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatGeneratedNativeTest, "Lhat.Editor.GeneratedNativeCalls", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatGeneratedNativeTest::RunTest(const FString&)
{
	FString LoadError;
	if (!TestTrue(TEXT("Load generated modules"), LhatUEBindings::LoadGeneratedProviders(LoadError))) { AddError(LoadError); return false; }
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("LhatGeneratedEditor")))
	{
		AddInfo(TEXT("Skipped: generated project modules have not been installed."));
		return true;
	}
	FLhatProgram Program(FLhatProgram::GetProjectScriptRoot());
	if (!TestTrue(*Program.GetDiagnostics(), Program.IsValid() && Program.Compile())) return false;
	struct FMachine
	{
		LhatMachine* Value;
		~FMachine() { FLhatProgram::DestroyMachine(Value); }
	} Machine{Program.CreateMachine()};
	LhatMachine* M = Machine.Value;
	if (!TestTrue(TEXT("Install generated API"), Program.Install(M))) return false;
	LhatValue Type;
	if (!TestTrue(TEXT("Editor class is registered"), lhat_machine_registered(M, "ue.LhatEditor", nullptr, "LhatNativeTestObject", &Type))) return false;
	auto Call = [M, Type](const char* Name, std::initializer_list<LhatValue> Args)
	{
		return lhat_machine_call_member(M, Type, Name, FCStringAnsi::Strlen(Name), Args.begin(), Args.size());
	};
	const auto Sum = Call("Add", {lhat_integer(19), lhat_integer(23)});
	TestEqual(TEXT("Static C++ call"), Sum.status, LHAT_RUN_OK);
	TestTrue(TEXT("Integer result"), lhat_is_integer(Sum.value) && lhat_as_integer(Sum.value) == 42);
	LhatValue Text;
	const FTCHARToUTF8 Utf8(TEXT("日本語の往復"));
	if (!TestTrue(TEXT("Create UTF-8 argument"), lhat_machine_make_string(M, Utf8.Get(), Utf8.Length(), &Text))) return false;
	const auto Echo = Call("Echo", {Text});
	TestEqual(TEXT("FString call"), Echo.status, LHAT_RUN_OK);
	FString Decoded;
	TestTrue(TEXT("Decode FString"), LhatUEBindings::Read(M, Program.GetBindings(), Echo.value, Decoded));
	TestEqual(TEXT("UTF-8 round trip"), Decoded, FString(TEXT("日本語の往復")));
	const auto Nil = Call("EchoObject", {lhat_nil()});
	TestTrue(TEXT("Nullable UObject argument and return"), Nil.status == LHAT_RUN_OK && lhat_is_nil(Nil.value));
	const auto Negated = Call("Not", {lhat_bool(true)});
	TestTrue(TEXT("Bool codec"), Negated.status == LHAT_RUN_OK && lhat_is_bool(Negated.value) && !lhat_as_bool(Negated.value));
	const FVector Vector(1, 2, 3);
	LhatHostValueRoom Room;
	LhatValue VectorValue;
	if (!TestTrue(TEXT("Create value argument"), lhat_place_hostvalue(Program.GetBindings().VectorTag, &Vector, &Room, &VectorValue))) return false;
	const auto Scaled = Call("ScaleVector", {VectorValue, lhat_real(2.5)});
	// The pinned core erases wide hostvalue returns at this C entry boundary.
	// GeneratedNativeScript below checks the actual vector result inside Lhat.
	TestTrue(TEXT("Vector input crosses C entry boundary"), Scaled.status == LHAT_RUN_OK && lhat_is_nil(Scaled.value));
	TStrongObjectPtr<ULhatNativeTestObject> Object(NewObject<ULhatNativeTestObject>());
	LhatValue Wrapped;
	if (!TestTrue(TEXT("Wrap most-derived class"), Program.GetBindings().WrapObject(M, Object.Get(), Wrapped) && lhat_machine_set_global(M, "__native_fixture", Wrapped))) return false;
	LhatValue NewValue = lhat_integer(57);
	const auto Set = lhat_machine_call_member(M, Wrapped, "SetValue", 8, &NewValue, 1);
	TestEqual(TEXT("Generated instance method"), Set.status, LHAT_RUN_OK);
	TestEqual(TEXT("Native object was mutated"), Object->Value, 57);
	lhat_machine_collectgarbage(M);
	const auto Get = lhat_machine_call_member(M, Wrapped, "ReadValue", 9, nullptr, 0);
	TestTrue(TEXT("Instance survives Lhat GC"), Get.status == LHAT_RUN_OK && lhat_is_integer(Get.value) && lhat_as_integer(Get.value) == 57);
	TestEqual(TEXT("Native calls do not dispatch through ProcessEvent"), Object->ProcessEventCount, 0);
	const auto Identity = Call("EchoObject", {Wrapped});
	TestTrue(TEXT("Object identity and derived tag survive base return"), Identity.status == LHAT_RUN_OK && lhat_is_object_kind(Identity.value, LHAT_OBJECT_HOSTDATA) && lhat_as_object(Identity.value) == lhat_as_object(Wrapped));
	TestTrue(TEXT("Reject uint8 overflow"), Call("ByteIdentity", {lhat_integer(256)}).status != LHAT_RUN_OK);
	TestTrue(TEXT("Reject uint8 underflow"), Call("ByteIdentity", {lhat_integer(-1)}).status != LHAT_RUN_OK);
	TestTrue(TEXT("Reject nonintegral integer argument"), Call("ByteIdentity", {lhat_real(1.5)}).status != LHAT_RUN_OK);
	TestTrue(TEXT("Accept uint8 upper bound"), Call("ByteIdentity", {lhat_integer(255)}).status == LHAT_RUN_OK);
	TestTrue(TEXT("Accept integral real for integer parameter"), Call("ByteIdentity", {lhat_real(42.0)}).status == LHAT_RUN_OK);
	double Special = 0;
	TestTrue(TEXT("Preserve NaN for native math APIs"), LhatUEBindings::Read(M, Program.GetBindings(), lhat_real(std::numeric_limits<double>::quiet_NaN()), Special) && FMath::IsNaN(Special));
	TestTrue(TEXT("Preserve infinity for native math APIs"), LhatUEBindings::Read(M, Program.GetBindings(), lhat_real(std::numeric_limits<double>::infinity()), Special) && Special == std::numeric_limits<double>::infinity());
	const char NameText[] = "NativeName";
	if (!TestTrue(TEXT("Create name argument"), lhat_machine_make_string(M, NameText, sizeof(NameText) - 1, &Text))) return false;
	const auto Name = Call("NameIdentity", {Text});
	TestTrue(TEXT("FName codec"), Name.status == LHAT_RUN_OK && LhatUEBindings::Read(M, Program.GetBindings(), Name.value, Decoded));
	TestEqual(TEXT("FName round trip"), Decoded, FString(TEXT("NativeName")));
	const char EmbeddedNull[] = {'A', '\0', 'B'};
	if (!TestTrue(TEXT("Create embedded-NUL argument"), lhat_machine_make_string(M, EmbeddedNull, sizeof(EmbeddedNull), &Text))) return false;
	TestTrue(TEXT("Reject embedded NUL in FName"), Call("NameIdentity", {Text}).status == LHAT_RUN_PANIC);
	const FString LongName = FString::ChrN(NAME_SIZE, TEXT('N'));
	const FTCHARToUTF8 LongNameUtf8(*LongName);
	if (!TestTrue(TEXT("Create long name argument"), lhat_machine_make_string(M, LongNameUtf8.Get(), LongNameUtf8.Length(), &Text))) return false;
	TestTrue(TEXT("Reject overlong FName"), Call("NameIdentity", {Text}).status == LHAT_RUN_PANIC);
	LhatValue DefaultActor;
	AActor* Actor = GetMutableDefault<AActor>();
	if (!TestTrue(TEXT("Wrap Actor CDO without changing it"), Program.GetBindings().WrapObject(M, Actor, DefaultActor))) return false;
	LhatValue CreateLabel = lhat_bool(false);
	const auto Label = lhat_machine_call_member(M, DefaultActor, "GetActorLabel", 13, &CreateLabel, 1);
	TestTrue(TEXT("WITH_EDITOR method on a Runtime class"), Label.status == LHAT_RUN_OK && LhatUEBindings::Read(M, Program.GetBindings(), Label.value, Decoded));
	TestEqual(TEXT("Editor label matches native value"), Decoded, Actor->GetActorLabel(false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatGeneratedScriptTest, "Lhat.Editor.GeneratedNativeScript", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatGeneratedScriptTest::RunTest(const FString&)
{
	FString LoadError;
	if (!TestTrue(TEXT("Load generated modules"), LhatUEBindings::LoadGeneratedProviders(LoadError))) { AddError(LoadError); return false; }
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("LhatGeneratedEditor")))
	{
		AddInfo(TEXT("Skipped: generated project modules have not been installed."));
		return true;
	}
	const FString Directory = FPaths::ProjectSavedDir() / TEXT("LhatNativeBindingTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Path = Directory / TEXT("Test.lh");
	IFileManager::Get().MakeDirectory(*Directory, true);
	// Import the parent once; it already exposes the registered child modules.
	const FString Source = TEXT("import^ue\n")
		TEXT("let^ answer = ue.LhatEditor.LhatNativeTestObject.Add(20, 22)\n")
		TEXT("let^ absolute = ue.Engine.KismetMathLibrary.Abs(-42.0)\n")
		TEXT("let^ scaled = ue.LhatEditor.LhatNativeTestObject.ScaleVector(ue.MakeVector(1, 2, 3), 2.5)\n")
		TEXT("return^ answer + absolute + scaled.X + scaled.Y + scaled.Z\n");
	if (!TestTrue(TEXT("Write generated API script"), FFileHelper::SaveStringToFile(Source, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
	FLhatProgram Program(Directory);
	const LhatUnit* Unit = Program.Check(TEXT("Test.lh"));
	const bool Ready = TestTrue(*Program.GetDiagnostics(), Unit && lhat_unit_ok(Unit) && Program.Compile());
	if (Ready)
	{
		LhatMachine* M = Program.CreateMachine();
		if (TestTrue(TEXT("Install before script execution"), Program.Install(M)))
		{
			const auto Run = lhat_run(M, lhat_unit_proto(Unit));
			TestEqual(TEXT("Generated calls run from Lhat source"), Run.status, LHAT_RUN_OK);
			TestTrue(TEXT("Runtime, Editor and vector APIs compose"), lhat_is_number(Run.value) && lhat_number_as_real(Run.value) == 99.0);
		}
		FLhatProgram::DestroyMachine(M);
	}
	IFileManager::Get().Delete(*Path);
	IFileManager::Get().DeleteDirectory(*Directory, false, false);
	return Ready;
}
#endif
