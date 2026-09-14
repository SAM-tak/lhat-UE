#include "LhatScript.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && LHAT_WITH_FRONTEND
#include "LhatBindings.h"
#include "LhatNativeBindings.h"
#include "Tests/LhatBundledTestActor.h"
#include "Components/SceneComponent.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatBundledEngineTest, "Lhat.Editor.BundledEngineBindings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatBundledEngineTest::RunTest(const FString&)
{
	struct FScriptFile
	{
		FString Directory = FPaths::ProjectSavedDir() / (TEXT("LhatBundledTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FString Path = Directory / TEXT("Test.lh");
		~FScriptFile() { IFileManager::Get().Delete(*Path); IFileManager::Get().DeleteDirectory(*Directory, false, false); }
	} File;
	IFileManager::Get().MakeDirectory(*File.Directory, true);
	const FString Source = TEXT("import^ue\nreturn^p^actor:ue.Actor, component:ue.Engine.SceneComponent {\n")
		TEXT("let^ v = ue.Engine.KismetMathLibrary.Multiply_VectorFloat(ue.MakeVector(3, 4, 0), 2)\n")
		TEXT("component.SetRelativeScale3D(v)\nactor.SetActorTickEnabled(false^)\n")
		TEXT("let^ label = actor.GetActorLabel(false^)\n")
		TEXT("let^ text = ue.Engine.KismetStringLibrary.Concat_StrStr(\"Lhat\", \"UE\")\n")
		TEXT("return^ue.Engine.KismetMathLibrary.VSize(v) + ue.Engine.KismetStringLibrary.Len(text)\n}\n");
	if (!TestTrue(TEXT("Write bundle fixture"), FFileHelper::SaveStringToFile(Source, *File.Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
	FLhatProgram Program(File.Directory);
	const LhatUnit* Unit = Program.Check(TEXT("Test.lh"));
	if (!TestTrue(*Program.GetDiagnostics(), Unit && lhat_unit_ok(Unit) && Program.Compile())) return false;
	struct FMachine
	{
		LhatMachine* M;
		~FMachine() { FLhatProgram::DestroyMachine(M); }
	} Machine{Program.CreateMachine()};
	if (!TestTrue(TEXT("Install bundled Engine API"), Machine.M && Program.Install(Machine.M))) return false;
	const auto Entry = lhat_run(Machine.M, lhat_unit_proto(Unit));
	if (!TestEqual(TEXT("Create script closure"), Entry.status, LHAT_RUN_OK)
		|| !lhat_machine_set_global(Machine.M, "__bundled_test", Entry.value)) return false;
	// Use this test-only CDO and restore its tick setting; no level is modified.
	ALhatBundledTestActor* Actor = GetMutableDefault<ALhatBundledTestActor>();
	TStrongObjectPtr<USceneComponent> Component(NewObject<USceneComponent>());
	LhatValue Args[2];
	if (!TestTrue(TEXT("Wrap Actor and Component"), Program.GetBindings().WrapObject(Machine.M, Actor, Args[0])
		&& lhat_machine_set_global(Machine.M, "__bundled_actor", Args[0])
		&& Program.GetBindings().WrapObject(Machine.M, Component.Get(), Args[1])
		&& lhat_machine_set_global(Machine.M, "__bundled_component", Args[1]))) return false;
	const int32 EventsBefore = Actor->ProcessEventCount;
	const bool WasTickEnabled = Actor->IsActorTickEnabled();
	const auto Run = lhat_machine_call(Machine.M, Entry.value, Args, 2);
	TestEqual(TEXT("Execute bundle script"), Run.status, LHAT_RUN_OK);
	TestTrue(TEXT("Math values and strings compose"), lhat_is_number(Run.value) && lhat_number_as_real(Run.value) == 16);
	TestEqual(TEXT("Instance component call"), Component->GetRelativeScale3D(), FVector(6, 8, 0));
	TestEqual(TEXT("Actor instance and editor calls bypass ProcessEvent"), Actor->ProcessEventCount, EventsBefore);
	TestFalse(TEXT("Tick disabled"), Actor->IsActorTickEnabled());
	Actor->SetActorTickEnabled(WasTickEnabled);
	lhat_machine_collectgarbage(Machine.M);
	const auto Again = lhat_machine_call(Machine.M, Entry.value, Args, 2);
	TestEqual(TEXT("Bundle values survive Lhat GC"), Again.status, LHAT_RUN_OK);
	Actor->SetActorTickEnabled(WasTickEnabled);
	// Combined-provider runs also exercise the shared component type declaration.
	LhatValue Type;
	TestTrue(TEXT("Shared component type exists"), lhat_machine_registered(Machine.M, "ue.Engine", nullptr, "SceneComponent", &Type));
	return true;
}
#endif
