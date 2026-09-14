#include "Misc/AutomationTest.h"
#include "LhatScript.h"

#if WITH_DEV_AUTOMATION_TESTS && LHAT_WITH_FRONTEND
#include "LhatNativeBindings.h"
#include "Tests/LhatDynamicTestObject.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	struct FScriptFile
	{
		FString Directory = FPaths::ProjectSavedDir() / (TEXT("LhatDynamicTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FString Path = Directory / TEXT("Test.lh");
		FScriptFile() { IFileManager::Get().MakeDirectory(*Directory, true); }
		~FScriptFile() { IFileManager::Get().Delete(*Path); IFileManager::Get().DeleteDirectory(*Directory, false, false); }
		bool Write(const FString& Text) { return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM); }
	};
	struct FMachine
	{
		LhatMachine* M;
		explicit FMachine(FLhatProgram& P) : M(P.CreateMachine()) {}
		~FMachine() { FLhatProgram::DestroyMachine(M); }
	};
	bool Number(const LhatRunResult& R, int64 N)
	{
		return R.status == LHAT_RUN_OK && lhat_is_number(R.value) && lhat_number_as_real(R.value) == double(N);
	}
	bool HasReport(const FString& Report, const FString& Member, const FString& Route, const FString& Reason = {})
	{
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Report), Root)) return false;
		for (const auto& V : Root->GetArrayField(TEXT("members")))
			if (V->AsObject()->GetStringField(TEXT("member")) == Member && V->AsObject()->GetStringField(TEXT("route")) == Route
				&& (Reason.IsEmpty() || V->AsObject()->GetStringField(TEXT("reason")) == Reason)) return true;
		return false;
	}
	UEdGraphPin* AddReturnGraph(UBlueprint* BP, FName Name, bool Override, const FString& Result)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(BP, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (Override)
			FBlueprintEditorUtils::AddFunctionGraph(BP, Graph, false, ULhatDynamicTestObject::StaticClass());
		else
			FBlueprintEditorUtils::AddFunctionGraph(BP, Graph, true, static_cast<UClass*>(nullptr));
		UK2Node_FunctionEntry* Entry = nullptr;
		UK2Node_FunctionResult* Exit = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (auto* E = Cast<UK2Node_FunctionEntry>(Node)) Entry = E;
			if (auto* E = Cast<UK2Node_FunctionResult>(Node)) Exit = E;
		}
		if (!Entry) return nullptr;
		if (!Exit)
		{
			FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
			Exit = Creator.CreateNode();
			Creator.Finalize();
		}
		UEdGraphPin* Value = Exit->FindPin(TEXT("ReturnValue"));
		if (!Value)
		{
			FEdGraphPinType Type;
			Type.PinCategory = UEdGraphSchema_K2::PC_Int;
			Value = Exit->CreateUserDefinedPin(TEXT("ReturnValue"), Type, EGPD_Input);
		}
		if (!Value) return nullptr;
		Value->BreakAllPinLinks(); // The override template initially returns the parent call's value.
		Value->DefaultValue = Result;
		auto* Then = Entry->FindPin(UEdGraphSchema_K2::PN_Then);
		auto* Exec = Exit->FindPin(UEdGraphSchema_K2::PN_Execute);
		if (!Then || !Exec || (!Then->LinkedTo.Contains(Exec) && !Graph->GetSchema()->TryCreateConnection(Then, Exec))) return nullptr;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatDynamicNativeTest, "Lhat.Editor.DynamicNativeBindings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatDynamicNativeTest::RunTest(const FString&)
{
	FScriptFile File;
	if (!TestTrue(TEXT("Write dynamic script"), File.Write(
		TEXT("import^ue\nreturn^p^x:ue.LhatEditor.LhatDynamicTestObject {\n")
		TEXT("x.Set_Value(9)\nx.Set_Flag(true^)\n")
		TEXT("let^ ok, twice, text = x.Results(7)\nlet^ bumped = x.Bump(1)\n")
		TEXT("let^ v = ue.LhatEditor.LhatDynamicTestObject.Scale(ue.MakeVector(1, 2, 3), 2)\n")
		TEXT("let^ identity = x.EmptyTransform()\nlet^ moved = ue.Engine.KismetMathLibrary.TransformLocation(identity, ue.MakeVector(1, 2, 3))\n")
		TEXT("if^!ok { return^ -1 }\n")
		TEXT("return^twice + bumped + v.X + v.Y + v.Z + moved.Z + ue.Engine.KismetStringLibrary.Len(text)\n}\n")))) return false;
	FLhatProgram Program(File.Directory);
	if (!TestTrue(*Program.GetDiagnostics(), Program.IsValid())) return false;
	const FString Report = Program.GetBindings().GetDynamicBindingReport();
	TestTrue(TEXT("Bundled API takes priority"), HasReport(Report, TEXT("ue.Engine.KismetMathLibrary:Abs"), TEXT("static")));
	TestTrue(TEXT("Private native class automatically uses dynamic bridge"), HasReport(Report, TEXT("ue.LhatEditor.LhatDynamicTestObject:Echo64"), TEXT("dynamic")));
	TestTrue(TEXT("Unsupported container reported"), HasReport(Report, TEXT("ue.LhatEditor.LhatDynamicTestObject:UnsupportedArray"), TEXT("skipped")));
	TestTrue(TEXT("Latent function excluded"), HasReport(Report, TEXT("ue.LhatEditor.LhatDynamicTestObject:LatentStub"), TEXT("skipped")));
	TestTrue(TEXT("Wide values in multiple results are excluded before dispatch"), HasReport(Report, TEXT("ue.Actor:GetActorEyesViewPoint"), TEXT("skipped"), TEXT("hostvalue_in_multiple_results")));
	TestTrue(TEXT("Static callback context unchanged"), lhat_lookup_host_context(Program.GetNativeHandle(), "ue.Engine", "KismetMathLibrary", "Abs") == &Program.GetBindings());
	const LhatUnit* Unit = Program.Check(TEXT("Test.lh"));
	if (!TestTrue(*Program.GetDiagnostics(), Unit && lhat_unit_ok(Unit) && Program.Compile())) return false;
	FMachine Machine(Program);
	if (!TestTrue(TEXT("Install reflection snapshot"), Program.Install(Machine.M))) return false;
	TStrongObjectPtr<ULhatDynamicTestObject> Object(NewObject<ULhatDynamicTestObject>());
	Object->AdjacentFlag = true;
	LhatValue Wrapped;
	if (!TestTrue(TEXT("Wrap dynamic-only instance"), Program.GetBindings().WrapObject(Machine.M, Object.Get(), Wrapped)
		&& lhat_machine_set_global(Machine.M, "__dynamic_object", Wrapped))) return false;
	const auto Entry = lhat_run(Machine.M, lhat_unit_proto(Unit));
	if (!TestTrue(TEXT("Root script closure"), Entry.status == LHAT_RUN_OK && lhat_machine_set_global(Machine.M, "__dynamic_script", Entry.value))) return false;
	TestTrue(TEXT("Out/ref, initialized math, property calls compose"), Number(lhat_machine_call(Machine.M, Entry.value, &Wrapped, 1), 41));
	TestEqual(TEXT("Property setter"), Object->Value, 9);
	TestTrue(TEXT("Bitfield setter preserves adjacent bit"), Object->Flag && Object->AdjacentFlag);
	TestTrue(TEXT("Dynamic methods use ProcessEvent"), Object->ProcessEventCount >= 2);
	lhat_machine_collectgarbage(Machine.M);
	CollectGarbage(RF_NoFlags);
	TestTrue(TEXT("Dynamic values survive both GCs"), Number(lhat_machine_call(Machine.M, Entry.value, &Wrapped, 1), 41));
	LhatValue Type;
	if (!TestTrue(TEXT("Dynamic type is in installed registry"), lhat_machine_registered(Machine.M, "ue.LhatEditor", nullptr, "LhatDynamicTestObject", &Type))) return false;
	auto Call = [&](const char* Name, std::initializer_list<LhatValue> Args)
	{
		return lhat_machine_call_member(Machine.M, Type, Name, FCStringAnsi::Strlen(Name), Args.begin(), Args.size());
	};
	const int64 Large = 9007199254740993LL;
	const auto Echo = Call("Echo64", {lhat_integer(Large)});
	TestTrue(TEXT("int64 is not rounded through double"), Echo.status == LHAT_RUN_OK && lhat_is_integer(Echo.value) && lhat_as_integer(Echo.value) == Large);
	TestTrue(TEXT("Reject integer overflow"), Call("ByteIdentity", {lhat_integer(256)}).status != LHAT_RUN_OK);
	TestTrue(TEXT("Reject fractional integer"), Call("ByteIdentity", {lhat_real(1.5)}).status != LHAT_RUN_OK);
	TestTrue(TEXT("Accept byte boundary"), Number(Call("ByteIdentity", {lhat_integer(255)}), 255));
	const auto Null = Call("EchoObject", {lhat_nil()});
	TestTrue(TEXT("Nullable object"), Null.status == LHAT_RUN_OK && lhat_is_nil(Null.value));
	const auto Identity = Call("EchoObject", {Wrapped});
	TestTrue(TEXT("Object identity through base return"), Identity.status == LHAT_RUN_OK && lhat_as_object(Identity.value) == lhat_as_object(Wrapped));
	LhatValue Text;
	const char Utf8[] = "A\0B";
	if (!lhat_machine_make_string(Machine.M, Utf8, 3, &Text)) return false;
	const auto String = Call("Echo", {Text});
	FString Decoded;
	TestTrue(TEXT("FString keeps embedded NUL"), String.status == LHAT_RUN_OK && LhatUEBindings::Read(Machine.M, Program.GetBindings(), String.value, Decoded) && Decoded.Len() == 3 && Decoded[2] == 'B');
	const auto Localized = Call("TextEcho", {Text});
	TestTrue(TEXT("FText string bridge"), Localized.status == LHAT_RUN_OK && LhatUEBindings::Read(Machine.M, Program.GetBindings(), Localized.value, Decoded) && Decoded.Len() == 3 && Decoded[2] == 'B');
	LhatValue Class;
	if (!Program.GetBindings().WrapObject(Machine.M, AActor::StaticClass(), Class)) return false;
	TestTrue(TEXT("UClass argument"), Call("ActorClass", {Class}).status == LHAT_RUN_OK);
	if (!Program.GetBindings().WrapObject(Machine.M, UObject::StaticClass(), Class)) return false;
	TestTrue(TEXT("UClass constraint rejects unrelated class"), Call("ActorClass", {Class}).status != LHAT_RUN_OK);
	LhatValue ReadOnlySetter;
	TestFalse(TEXT("Read-only property has no setter"), lhat_machine_registered(Machine.M, "ue.LhatEditor", "LhatDynamicTestObject", "Set_ReadOnly", &ReadOnlySetter));
	TArray<uint8> Json;
	FString Error;
	if (TestTrue(TEXT("Export dynamic host API"), Program.GetHostApiJson(Json, Error)))
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Json.GetData()), Json.Num());
		TestTrue(TEXT("Host JSON includes dynamic registration"), FString(Converted.Length(), Converted.Get()).Contains(TEXT("\"name\": \"Echo64\"")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLhatDynamicBlueprintTest, "Lhat.Editor.DynamicBlueprintBindings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLhatDynamicBlueprintTest::RunTest(const FString&)
{
	const FString Folder = TEXT("LhatDynamic_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TStrongObjectPtr<UPackage> Package(CreatePackage(*(TEXT("/Temp/") + Folder + TEXT("/BP_Dynamic"))));
	Package->SetFlags(RF_Transient);
	TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(ULhatDynamicTestObject::StaticClass(), Package.Get(), TEXT("BP_Dynamic"), BPTYPE_Normal));
	if (!TestNotNull(TEXT("Create transient Blueprint"), BP.Get())) return false;
	BP->SetFlags(RF_Transient);
	UEdGraphPin* OverrideResult = AddReturnGraph(BP.Get(), TEXT("Compute"), true, TEXT("123"));
	UEdGraphPin* OwnResult = AddReturnGraph(BP.Get(), TEXT("BlueprintOnly"), false, TEXT("321"));
	UEdGraphPin* SpacedResult = AddReturnGraph(BP.Get(), TEXT("Spaced Name"), false, TEXT("71"));
	if (!TestTrue(TEXT("Create Blueprint function bodies"), OverrideResult && OwnResult && SpacedResult)) return false;
	FKismetEditorUtilities::CompileBlueprint(BP.Get());
	if (!TestTrue(TEXT("Compile actual Blueprint bytecode"), BP->Status != BS_Error && BP->GeneratedClass != nullptr)) return false;
	UFunction* OwnFunction = BP->GeneratedClass->FindFunctionByName(TEXT("BlueprintOnly"));
	if (!TestTrue(TEXT("Function is defined only in Blueprint"), OwnFunction && !OwnFunction->HasAnyFunctionFlags(FUNC_Native) && OwnFunction->Script.Num() > 0)) return false;
	FScriptFile File;
	File.Write(TEXT("import^ue\nreturn^p^x:ue.LhatEditor.LhatDynamicTestObject { return^x.Compute(7) }\n"));
	FLhatProgram Program(File.Directory);
	if (!TestTrue(*Program.GetDiagnostics(), Program.IsValid())) return false;
	const LhatUnit* Unit = Program.Check(TEXT("Test.lh"));
	if (!TestTrue(*Program.GetDiagnostics(), Unit && lhat_unit_ok(Unit) && Program.Compile())) return false;
	FMachine Machine(Program);
	if (!Program.Install(Machine.M)) return false;
	TStrongObjectPtr<UObject> Object(NewObject<UObject>(GetTransientPackage(), BP->GeneratedClass));
	LhatValue Wrapped;
	if (!Program.GetBindings().WrapObject(Machine.M, Object.Get(), Wrapped) || !lhat_machine_set_global(Machine.M, "__bp_object", Wrapped)) return false;
	const auto Entry = lhat_run(Machine.M, lhat_unit_proto(Unit));
	if (Entry.status != LHAT_RUN_OK || !lhat_machine_set_global(Machine.M, "__bp_script", Entry.value)) return false;
	TestTrue(TEXT("Blueprint override called through native base signature"), Number(lhat_machine_call(Machine.M, Entry.value, &Wrapped, 1), 123));
	TestTrue(TEXT("Blueprint-only function is automatically registered"), Number(lhat_machine_call_member(Machine.M, Wrapped, "BlueprintOnly", 13, nullptr, 0), 321));
	const char* SpacedName = "_LH_537061636564204E616D65";
	TestTrue(TEXT("Non-identifier Blueprint name is callable by its escaped name"), Number(lhat_machine_call_member(Machine.M, Wrapped, SpacedName, FCStringAnsi::Strlen(SpacedName), nullptr, 0), 71));
	lhat_machine_collectgarbage(Machine.M);
	CollectGarbage(RF_NoFlags);
	TestTrue(TEXT("Blueprint snapshot survives UE/Lhat GC"), Number(lhat_machine_call(Machine.M, Entry.value, &Wrapped, 1), 123));
	// Compiling may reconstruct pins; never retain a raw pin across compilation.
	OverrideResult = nullptr;
	for (UEdGraph* Graph : BP->FunctionGraphs)
		if (Graph->GetFName() == TEXT("Compute"))
			for (UEdGraphNode* Node : Graph->Nodes)
				if (auto* Exit = Cast<UK2Node_FunctionResult>(Node)) OverrideResult = Exit->FindPin(TEXT("ReturnValue"));
	if (!TestNotNull(TEXT("Find current override output pin"), OverrideResult)) return false;
	OverrideResult->DefaultValue = TEXT("456");
	FKismetEditorUtilities::CompileBlueprint(BP.Get());
	TestTrue(TEXT("Old program refuses stale reflection"), lhat_machine_call(Machine.M, Entry.value, &Wrapped, 1).status != LHAT_RUN_OK);
	FLhatProgram Fresh(File.Directory);
	const auto* FreshUnit = Fresh.Check(TEXT("Test.lh"));
	if (!TestTrue(*Fresh.GetDiagnostics(), FreshUnit && lhat_unit_ok(FreshUnit) && Fresh.Compile())) return false;
	FMachine FreshMachine(Fresh);
	if (!Fresh.Install(FreshMachine.M)) return false;
	TStrongObjectPtr<UObject> FreshObject(NewObject<UObject>(GetTransientPackage(), BP->GeneratedClass));
	LhatValue FreshWrapped;
	if (!Fresh.GetBindings().WrapObject(FreshMachine.M, FreshObject.Get(), FreshWrapped)) return false;
	const LhatValue Arg = lhat_integer(7);
	TestTrue(TEXT("New program uses recompiled Blueprint"), Number(lhat_machine_call_member(FreshMachine.M, FreshWrapped, "Compute", 7, &Arg, 1), 456));
	Package->SetDirtyFlag(false); // This is only a transient test package; never saved.
	return true;
}
#endif
