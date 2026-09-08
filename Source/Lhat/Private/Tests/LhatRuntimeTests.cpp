#include "Misc/AutomationTest.h"
#include "LhatInstance.h"

#if WITH_DEV_AUTOMATION_TESTS && LHAT_WITH_FRONTEND

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "LhatComponent.h"
#include "LhatSubsystem.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"

namespace
{
	struct FTestWorld
	{
		UWorld* World;
		FTestWorld()
		{
			const auto Initialization = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Initialization);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			// AActor::ProcessEvent ignores even native calls until actors are initialized.
			World->InitializeActorsForPlay(FURL());
		}
		~FTestWorld()
		{
			World->BeginTearingDown();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		AActor* Actor()
		{
			AActor* Result = World->SpawnActor<AActor>();
			auto* Root = NewObject<USceneComponent>(Result);
			Result->SetRootComponent(Root);
			Root->SetMobility(EComponentMobility::Movable);
			Root->RegisterComponent();
			return Result;
		}
	};

	FString SampleRoot()
	{
		return FPaths::ConvertRelativePathToFull(IPluginManager::Get().FindPlugin(TEXT("Lhat"))->GetBaseDir() / TEXT("Examples/Script"));
	}

	// Own only a unique fixture file, never the project's real scripts.
	struct FScriptFixture
	{
		FString Directory = FLhatProgram::GetProjectScriptRoot() / (TEXT("LhatTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FString Path = Directory / TEXT("Test.lh");
		FString EntryPoint = FPaths::GetCleanFilename(Directory) / TEXT("Test.lh");
		FScriptFixture() { IFileManager::Get().MakeDirectory(*Directory, true); }
		~FScriptFixture()
		{
			IFileManager::Get().Delete(*Path);
			IFileManager::Get().DeleteDirectory(*Directory, false, false);
		}
		bool Write(const FString& Text) const
		{
			return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		FString Mover() const
		{
			FString Text;
			FFileHelper::LoadFileToString(Text, *(SampleRoot() / TEXT("Mover.lh")));
			return Text;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatLifecycleTest, "Lhat.Runtime.LifecycleAndGC", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatLifecycleTest::RunTest(const FString&)
{
	FTestWorld World;
	auto Program = MakeShared<FLhatProgram>(SampleRoot());
	const LhatUnit* Unit = Program->Check(TEXT("Mover.lh"));
	if (!TestTrue(*Program->GetDiagnostics(), Unit && lhat_unit_ok(Unit) && Program->Compile())) return false;
	AActor* First = World.Actor();
	AActor* Second = World.Actor();
	FLhatInstance One(Program);
	FLhatInstance Two(Program);
	if (!TestTrue(TEXT("First instance starts"), One.Start(Unit, First))) { AddError(One.GetError()); return false; }
	if (!TestTrue(TEXT("Second instance starts"), Two.Start(Unit, Second))) { AddError(Two.GetError()); return false; }
	TestTrue(TEXT("First tick"), One.Tick(0.5));
	TestTrue(TEXT("Second tick"), Two.Tick(0.25));
	TestEqual(TEXT("First Actor moves"), First->GetActorLocation().X, 50.0);
	TestEqual(TEXT("Second Actor has its own state"), Second->GetActorLocation().X, 25.0);
	TestTrue(TEXT("Repeated tick before GC"), One.Tick(0.5));
	TestEqual(TEXT("Repeated movement before GC"), First->GetActorLocation().X, 100.0);
	lhat_machine_collectgarbage(One.GetMachine());
	TestTrue(TEXT("State survives Lhat GC"), One.Tick(0.5));
	TestEqual(TEXT("Movement after Lhat GC"), First->GetActorLocation().X, 150.0);
	CollectGarbage(RF_NoFlags);
	TestTrue(TEXT("State survives both collectors"), One.Tick(0.5));
	TestEqual(TEXT("Rooted state continues"), First->GetActorLocation().X, 200.0);
	One.Stop();
	One.Stop();
	TestFalse(TEXT("Stop is idempotent"), One.IsRunning());
	Second->Destroy();
	TestFalse(TEXT("Destroyed Actor refuses access"), Two.Tick(0.25));
	TestTrue(TEXT("Fault contains script location"), Two.GetError().Contains(TEXT("Mover.lh")));
	Two.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatPathTest, "Lhat.Runtime.ScriptRoot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatPathTest::RunTest(const FString&)
{
	FLhatProgram Program(SampleRoot());
	TestNull(TEXT("Absolute entry points are rejected"), Program.Check(SampleRoot() / TEXT("Mover.lh")));
	TestNull(TEXT("Traversal outside Script is rejected"), Program.Check(TEXT("../../README.md")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatComponentTest, "Lhat.Runtime.ComponentLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatComponentTest::RunTest(const FString&)
{
	FScriptFixture Fixture;
	if (!TestTrue(TEXT("Write project fixture"), Fixture.Write(Fixture.Mover()))) return false;
	FTestWorld World;
	TestNotNull(TEXT("Game world has a runtime"), World.World->GetSubsystem<ULhatSubsystem>());
	AActor* Actor = World.Actor();
	auto* Component = NewObject<ULhatComponent>(Actor);
	Actor->AddInstanceComponent(Component);
	Component->ScriptPath = Fixture.EntryPoint;
	Component->RegisterComponent();
	Actor->DispatchBeginPlay();
	if (!TestTrue(TEXT("Component starts the project script"), Component->LastError.IsEmpty()))
	{
		AddError(Component->LastError);
		return false;
	}
	Component->TickComponent(0.25f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("Component forwards Tick"), Actor->GetActorLocation().X, 25.0);
	Actor->RouteEndPlay(EEndPlayReason::EndPlayInEditor);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatParametersTest, "Lhat.Runtime.ParametersAndDefaults", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatParametersTest::RunTest(const FString&)
{
	FScriptFixture Fixture;
	if (!TestTrue(TEXT("Write initial defaults"), Fixture.Write(Fixture.Mover()))) return false;
	FTestWorld World;
	AActor* First = World.Actor();
	AActor* Second = World.Actor();
	auto* One = NewObject<ULhatComponent>(First);
	auto* Two = NewObject<ULhatComponent>(Second);
	First->AddInstanceComponent(One);
	Second->AddInstanceComponent(Two);
	One->ScriptPath = Two->ScriptPath = Fixture.EntryPoint;
	One->RefreshParameters();
	Two->RefreshParameters();
	if (!TestTrue(*One->LastError, One->LastError.IsEmpty())) return false;
	const auto Speed = One->Parameters.GetValueDouble(TEXT("Speed"));
	if (!TestTrue(TEXT("Annotation exports Speed"), Speed.IsValid())) return false;
	TestEqual(TEXT("Declaration default"), Speed.GetValue(), 100.0);
	TestFalse(TEXT("Unannotated fields are private"), One->Parameters.GetValueDouble(TEXT("Frames")).IsValid());
	Two->Parameters.SetValueDouble(TEXT("Speed"), 300.0);
	if (!TestTrue(TEXT("Edit script default"), Fixture.Write(Fixture.Mover().Replace(TEXT("Speed = 100"), TEXT("Speed = 200"))))) return false;
	One->RefreshParameters();
	Two->RefreshParameters();
	TestEqual(TEXT("Unchanged value follows new default"), One->Parameters.GetValueDouble(TEXT("Speed")).GetValue(), 200.0);
	TestEqual(TEXT("Explicit override survives refresh"), Two->Parameters.GetValueDouble(TEXT("Speed")).GetValue(), 300.0);
	One->RegisterComponent();
	Two->RegisterComponent();
	First->DispatchBeginPlay();
	Second->DispatchBeginPlay();
	One->TickComponent(0.5f, LEVELTICK_All, nullptr);
	Two->TickComponent(0.5f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("First uses script default"), First->GetActorLocation().X, 100.0);
	TestEqual(TEXT("Second uses instance override"), Second->GetActorLocation().X, 150.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatReflectionTest, "Lhat.Runtime.ReflectedCalls", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatReflectionTest::RunTest(const FString&)
{
	FScriptFixture Fixture;
	const FString Source = TEXT("module^game.test\nimport^ue\npublic^let^Test = def^{\n")
		TEXT(" self^{ abstract^Actor : ue.Actor, @EditAnywhere Hidden = true^, @EditAnywhere Label = \"日本語\" },\n")
		TEXT(" override^new = f^actor:ue.Actor { self^{Actor = actor} },\n")
		TEXT(" BeginPlay = p^self^{ self^.Actor.SetActorHiddenInGame(self^.Hidden) ue.Log(self^.Label) },\n")
		TEXT(" Tick = p^self^, delta:number^{ self^.Actor.SetActorLocation(ue.MakeVector(1, 2, 3)) },\n")
		TEXT(" EndPlay = p^self^{ self^.Actor.SetActorHiddenInGame(false^) },\n}\n");
	if (!TestTrue(TEXT("Write reflection fixture"), Fixture.Write(Source))) return false;
	FTestWorld World;
	AActor* Actor = World.Actor();
	auto Program = MakeShared<FLhatProgram>(FLhatProgram::GetProjectScriptRoot());
	FInstancedPropertyBag Defaults;
	FString Error;
	if (!TestTrue(TEXT("Read bool/string defaults"), Program->ReadParameterDefaults(Fixture.EntryPoint, Defaults, Error))) { AddError(Error); return false; }
	const auto Label = Defaults.GetValueString(TEXT("Label"));
	if (!TestTrue(TEXT("String parameter exists"), Label.IsValid())) return false;
	TestEqual(TEXT("UTF-8 defaults survive conversion"), Label.GetValue(), FString(TEXT("日本語")));
	Defaults.SetValueString(TEXT("Label"), TEXT("UE からの設定"));
	FLhatInstance Instance(Program);
	if (!TestTrue(TEXT("Start reflection fixture"), Instance.Start(Program->Check(Fixture.EntryPoint), Actor, &Defaults))) { AddError(Instance.GetError()); return false; }
	TestTrue(TEXT("Reflected bool argument"), Actor->IsHidden());
	TestTrue(TEXT("Vector constructor"), Instance.Tick(0.1));
	TestEqual(TEXT("Vector fields reach UE"), Actor->GetActorLocation(), FVector(1, 2, 3));
	Instance.Stop();
	TestFalse(TEXT("EndPlay is forwarded"), Actor->IsHidden());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatBudgetTest, "Lhat.Runtime.ExecutionBudget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatBudgetTest::RunTest(const FString&)
{
	FScriptFixture Fixture;
	if (!TestTrue(TEXT("Write looping fixture"), Fixture.Write(Fixture.Mover().Replace(TEXT("self^.Frames += 1"), TEXT("repeat^ while^ true^ { self^.Frames += 1 }"))))) return false;
	FTestWorld World;
	auto Program = MakeShared<FLhatProgram>(FLhatProgram::GetProjectScriptRoot());
	const LhatUnit* Unit = Program->Check(Fixture.EntryPoint);
	if (!TestTrue(TEXT("Loop compiles"), Unit && lhat_unit_ok(Unit) && Program->Compile())) { AddError(Program->GetDiagnostics()); return false; }
	FLhatInstance Instance(Program);
	if (!TestTrue(TEXT("Looping instance starts"), Instance.Start(Unit, World.Actor()))) { AddError(Instance.GetError()); return false; }
	TestFalse(TEXT("Infinite loop yields to host"), Instance.Tick(0.1));
	TestFalse(TEXT("Faulted instance is disabled"), Instance.IsRunning());
	TestTrue(TEXT("Actionable budget diagnostic"), Instance.GetError().Contains(TEXT("execution budget")));
	Instance.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatWorldTest, "Lhat.Runtime.WorldIsolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatWorldTest::RunTest(const FString&)
{
	FScriptFixture Fixture;
	if (!TestTrue(TEXT("Write world fixture"), Fixture.Write(Fixture.Mover()))) return false;
	TSharedPtr<FLhatInstance> Survivor;
	{
		FTestWorld FirstWorld;
		AActor* First = FirstWorld.Actor();
		auto* Runtime = FirstWorld.World->GetSubsystem<ULhatSubsystem>();
		FString Error;
		FInstancedPropertyBag Empty;
		Survivor = Runtime->StartScript(Fixture.EntryPoint, First, Empty, Error);
		if (!TestTrue(TEXT("First world's script starts"), Survivor.IsValid())) { AddError(Error); return false; }
		if (!TestTrue(TEXT("Edit source between worlds"), Fixture.Write(Fixture.Mover().Replace(TEXT("Speed = 100"), TEXT("Speed = 200"))))) return false;
		AActor* CachedActor = FirstWorld.Actor();
		auto Cached = Runtime->StartScript(Fixture.EntryPoint, CachedActor, Empty, Error);
		if (!TestTrue(TEXT("Same world reuses its program"), Cached.IsValid())) { AddError(Error); return false; }
		TestTrue(TEXT("Cached instance ticks"), Cached->Tick(0.5));
		TestEqual(TEXT("No live source reload inside a world"), CachedActor->GetActorLocation().X, 50.0);
		FTestWorld SecondWorld;
		AActor* FreshActor = SecondWorld.Actor();
		auto Fresh = SecondWorld.World->GetSubsystem<ULhatSubsystem>()->StartScript(Fixture.EntryPoint, FreshActor, Empty, Error);
		if (!TestTrue(TEXT("Second world compiles independently"), Fresh.IsValid())) { AddError(Error); return false; }
		TestTrue(TEXT("New world's instance ticks"), Fresh->Tick(0.5));
		TestEqual(TEXT("New world sees edited source"), FreshActor->GetActorLocation().X, 100.0);
		TestFalse(TEXT("Cross-world Actor is rejected"), Runtime->StartScript(Fixture.EntryPoint, FreshActor, Empty, Error).IsValid());
	}
	TestFalse(TEXT("World teardown stops externally held instances"), Survivor->IsRunning());
	Survivor.Reset();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatErrorsTest, "Lhat.Runtime.DiagnosticsAndParameterTypes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatErrorsTest::RunTest(const FString&)
{
	FScriptFixture Fixture;
	if (!TestTrue(TEXT("Write diagnostic fixture"), Fixture.Write(Fixture.Mover()))) return false;
	FTestWorld World;
	auto Program = MakeShared<FLhatProgram>(FLhatProgram::GetProjectScriptRoot());
	const LhatUnit* Unit = Program->Check(Fixture.EntryPoint);
	if (!TestTrue(TEXT("Diagnostic fixture compiles"), Unit && lhat_unit_ok(Unit) && Program->Compile())) { AddError(Program->GetDiagnostics()); return false; }
	FInstancedPropertyBag WrongType;
	WrongType.AddProperty(TEXT("Speed"), EPropertyBagPropertyType::Bool);
	FLhatInstance Instance(Program);
	TestFalse(TEXT("Incompatible parameter is rejected"), Instance.Start(Unit, World.Actor(), &WrongType));
	TestTrue(TEXT("Parameter diagnostic names the field"), Instance.GetError().Contains(TEXT("Speed")));
	if (!TestTrue(TEXT("Write malformed source"), Fixture.Write(TEXT("module^game.invalid\npublic^let^broken = }\n")))) return false;
	FLhatProgram Invalid(FLhatProgram::GetProjectScriptRoot());
	const LhatUnit* BadUnit = Invalid.Check(Fixture.EntryPoint);
	TestTrue(TEXT("Syntax error refuses compilation"), !BadUnit || !lhat_unit_ok(BadUnit));
	TestTrue(TEXT("Compile diagnostic includes source path"), Invalid.GetDiagnostics().Contains(TEXT("Test.lh:")));
	return true;
}

#endif
