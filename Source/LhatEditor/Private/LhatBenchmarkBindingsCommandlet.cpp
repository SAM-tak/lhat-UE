#include "LhatBenchmarkBindingsCommandlet.h"
#include "LhatScript.h"
#include "LhatNativeBindings.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogLhatBenchmark, Log, All);

#if LHAT_WITH_FRONTEND
namespace
{
	// Experimental warm-path bridge, deliberately private to this benchmark.
	// Native static functions only: no Blueprint recompilation/lifetime guarantees.
	// Reflection and codec selection happen once, not inside the measured loop.
	struct FSlot
	{
		FProperty* Property = nullptr;
		int32 Offset = 0;
		FString Signature;
		bool (*Read)(LhatMachine*, FLhatBindings&, LhatValue, void*) = nullptr;
		bool (*Write)(LhatMachine*, FLhatBindings&, const void*, LhatValue&) = nullptr;

		template<typename T> void Set(const TCHAR* Type)
		{
			Signature = Type;
			Read = [](LhatMachine* M, FLhatBindings& B, LhatValue V, void* Out)
			{
				if constexpr (std::is_same_v<T, FVector>) return LhatUEBindings::ReadValue(M, B, V, *static_cast<T*>(Out), "Vector");
				else return LhatUEBindings::Read(M, B, V, *static_cast<T*>(Out));
			};
			Write = [](LhatMachine* M, FLhatBindings& B, const void* V, LhatValue& Out)
			{
				if constexpr (std::is_same_v<T, FVector>) return LhatUEBindings::WriteValue(M, B, *static_cast<const T*>(V), Out, "Vector");
				else return LhatUEBindings::Write(M, B, *static_cast<const T*>(V), Out);
			};
		}
		bool Build(FProperty* P)
		{
			Property = P;
			Offset = P->GetOffset_ForInternal();
			if (P->ArrayDim != 1) return false;
			if (CastField<FIntProperty>(P)) Set<int32>(TEXT("number^"));
			else if (CastField<FFloatProperty>(P)) Set<float>(TEXT("number^"));
			else if (CastField<FDoubleProperty>(P)) Set<double>(TEXT("number^"));
			else if (CastField<FStrProperty>(P)) Set<FString>(TEXT("string^"));
			else if (auto* S = CastField<FStructProperty>(P); S && S->Struct == TBaseStructure<FVector>::Get()) Set<FVector>(TEXT("ue.Vector"));
			else return false;
			return true;
		}
	};

	struct FCachedCall
	{
		FLhatBindings* Bindings = nullptr;
		UFunction* Function = nullptr;
		UObject* Receiver = nullptr; // Native class CDO, stable for this commandlet.
		TArray<FSlot> Inputs;
		FSlot Return;
		TArray<FProperty*> Initialize;
		TArray<FProperty*> Destroy;
		int32 Size = 0;
		int32 Alignment = 0;

		bool Register(FLhatProgram& Program, UClass* Class, const char* Type, const char* Name)
		{
			Bindings = &Program.GetBindings();
			Function = Class->FindFunctionByName(FName(UTF8_TO_TCHAR(Name)));
			if (!Function || !Function->HasAllFunctionFlags(FUNC_Native | FUNC_Static | FUNC_BlueprintPure)
				|| Function->HasAnyFunctionFlags(FUNC_Net | FUNC_Delegate | FUNC_Event)
				|| Function->HasMetaData(TEXT("Latent")) || !Function->GetReturnProperty()) return false;
			Receiver = Class->GetDefaultObject();
			Size = Function->GetStructureSize();
			Alignment = FMath::Max(16, Function->GetMinAlignment());
			if (Size <= 0 || Size > 65536 || Alignment > 256) return false;
			FString Signature = TEXT("f^");
			for (TFieldIterator<FProperty> It(Function); It; ++It)
			{
				FProperty* P = *It;
				if (!P->HasAnyPropertyFlags(CPF_Parm)) continue;
				if (!P->HasAnyPropertyFlags(CPF_ZeroConstructor)) Initialize.Add(P);
				if (!P->HasAnyPropertyFlags(CPF_IsPlainOldData | CPF_NoDestructor)) Destroy.Add(P);
				if (P == Function->GetReturnProperty())
				{
					if (!Return.Build(P)) return false;
					continue;
				}
				if (P->HasAnyPropertyFlags(CPF_OutParm) && !P->HasAnyPropertyFlags(CPF_ConstParm)) return false;
				FSlot Slot;
				if (!Slot.Build(P)) return false;
				if (!Inputs.IsEmpty()) Signature += TEXT(", ");
				Signature += Slot.Signature;
				Inputs.Add(MoveTemp(Slot));
			}
			Signature += TEXT(" -> ") + Return.Signature + TEXT(";");
			const FString Alias = TEXT("BenchCached_") + FString(UTF8_TO_TCHAR(Name));
			return lhat_register_member(Program.GetNativeHandle(), "ue.Engine", Type,
				TCHAR_TO_UTF8(*Alias), TCHAR_TO_UTF8(*Signature), &Invoke, this);
		}

		static void Invoke(LhatMachine* M, void* Context, const LhatValue* Args, size_t Count, LhatValue* Answers, int* AnswerCount)
		{
			check(IsInGameThread());
			auto& C = *static_cast<FCachedCall*>(Context);
			if (Count != static_cast<size_t>(C.Inputs.Num())) { LhatUEBindings::Fail(M, "Cached argument count mismatch"); return; }
			// A distinct, aligned parameter frame for every invocation: reentrant and
			// no per-call parameter-buffer heap allocation. FString codecs still allocate.
			uint8* Data = Align(static_cast<uint8*>(FMemory_Alloca(C.Size + C.Alignment)), C.Alignment);
			FMemory::Memzero(Data, C.Size);
			for (FProperty* P : C.Initialize) P->InitializeValue_InContainer(Data);
			struct FDestroyFrame
			{
				FCachedCall& Call;
				uint8* Data;
				~FDestroyFrame() { for (FProperty* P : Call.Destroy) P->DestroyValue_InContainer(Data); }
			} Frame{C, Data};
			for (int32 I = 0; I < C.Inputs.Num(); ++I)
			{
				const FSlot& S = C.Inputs[I];
				if (!S.Read(M, *C.Bindings, Args[I], Data + S.Offset)) return;
			}
			C.Receiver->ProcessEvent(C.Function, Data);
			if (C.Return.Write(M, *C.Bindings, Data + C.Return.Offset, Answers[0])) *AnswerCount = 1;
		}
	};

	struct FCase
	{
		UClass* Class;
		const char* Type;
		const char* Name;
		const TCHAR* Arguments;
		const TCHAR* ResultSuffix;
		double PerIteration; // Abs has a triangular checksum instead.
		bool bString;
		const LhatUnit* Units[2] = {};
	};

	FString MakeSource(const FCase& C, bool bCached)
	{
		const FString Call = FString::Printf(TEXT("ue.Engine.%s.%s%s(%s)%s"), UTF8_TO_TCHAR(C.Type),
			bCached ? TEXT("BenchCached_") : TEXT(""), UTF8_TO_TCHAR(C.Name), C.Arguments, C.ResultSuffix);
		// Wide host values cannot be captured; keep the vector in the batch's
		// stack frame, outside the loop (one shared setup call per batch).
		return TEXT("import^ue\nreturn^p^n:number^ {\nlet^ vector = ue.MakeVector(3, 4, 0)\n")
			+ FString(C.bString ? TEXT("var^ sink = \"\"\n") : TEXT("var^ sink = 0\n"))
			+ TEXT("var^ i = 0\nrepeat^while^ i < n {\ni := i + 1\nsink := ")
			+ (C.bString ? TEXT("") : TEXT("sink + ")) + Call + TEXT("\n}\nreturn^sink\n}\n");
	}

	struct FRunner
	{
		LhatMachine* Machine = nullptr;
		LhatValue Closure = lhat_nil();
		~FRunner() { FLhatProgram::DestroyMachine(Machine); }
		bool Start(FLhatProgram& Program, const LhatUnit* Unit)
		{
			Machine = Program.CreateMachine();
			if (!Machine || !Program.Install(Machine)) return false;
			const auto Run = lhat_run(Machine, lhat_unit_proto(Unit));
			Closure = Run.value;
			return Run.status == LHAT_RUN_OK && lhat_machine_set_global(Machine, "__benchmark", Closure);
		}
		bool Measure(FLhatProgram& Program, const FCase& C, int32 N, double& Ns)
		{
			// Equal clean starting points, outside timing; automatic GC during the
			// loop remains enabled and is included in the measurement.
			lhat_machine_collectgarbage(Machine);
			const LhatValue Arg = lhat_integer(N);
			const double Start = FPlatformTime::Seconds();
			const auto Run = lhat_machine_call(Machine, Closure, &Arg, 1);
			Ns = (FPlatformTime::Seconds() - Start) * 1.e9 / N;
			if (Run.status != LHAT_RUN_OK) return false;
			if (C.bString)
			{
				FString Text;
				return LhatUEBindings::Read(Machine, Program.GetBindings(), Run.value, Text) && Text == TEXT("Lhat binding benchmark");
			}
			const double Expected = FCStringAnsi::Strcmp(C.Name, "Abs") == 0 ? double(N) * (N + 1) / 2 : N * C.PerIteration;
			return lhat_is_number(Run.value) && lhat_number_as_real(Run.value) == Expected;
		}
	};

	double Median(TArray<double> Values)
	{
		Values.Sort();
		const int32 N = Values.Num();
		return N % 2 ? Values[N / 2] : (Values[N / 2 - 1] + Values[N / 2]) / 2;
	}
	TArray<TSharedPtr<FJsonValue>> JsonNumbers(const TArray<double>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (double V : Values) Result.Add(MakeShared<FJsonValueNumber>(V));
		return Result;
	}
}
#endif

ULhatBenchmarkBindingsCommandlet::ULhatBenchmarkBindingsCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 ULhatBenchmarkBindingsCommandlet::Main(const FString& Params)
{
#if !LHAT_WITH_FRONTEND
	return 2;
#else
	int32 Iterations = 200000, Samples = 11, Warmups = 3;
	FParse::Value(*Params, TEXT("Iterations="), Iterations);
	FParse::Value(*Params, TEXT("Samples="), Samples);
	FParse::Value(*Params, TEXT("Warmups="), Warmups);
	if (Iterations < 1 || Iterations > 10000000 || Samples < 3 || Samples > 101 || Warmups < 1 || Warmups > 20) return 2;
	FString Output;
	FParse::Value(*Params, TEXT("Output="), Output);
	if (Output.IsEmpty()) Output = FPaths::ProjectSavedDir() / TEXT("LhatBenchmarks") / FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")) / TEXT("bindings.json");
	Output = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Output);
	// Refuse to overwrite results; each invocation should have its own directory.
	if (IFileManager::Get().FileExists(*Output)) { UE_LOG(LogLhatBenchmark, Error, TEXT("Output already exists: %s"), *Output); return 2; }
	const FString Directory = FPaths::GetPath(Output);
	const FString ScriptDirectory = Directory / (TEXT("Scripts-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!IFileManager::Get().MakeDirectory(*ScriptDirectory, true)) return 1;
	FLhatProgram Program(ScriptDirectory);
	if (!Program.IsValid()) { UE_LOG(LogLhatBenchmark, Error, TEXT("%s"), *Program.GetDiagnostics()); return 1; }
	FCase Cases[] = {
		{UKismetMathLibrary::StaticClass(), "KismetMathLibrary", "Abs", TEXT("-i"), TEXT(""), 0, false},
		{UKismetMathLibrary::StaticClass(), "KismetMathLibrary", "VSize", TEXT("vector"), TEXT(""), 5, false},
		{UKismetMathLibrary::StaticClass(), "KismetMathLibrary", "Multiply_VectorFloat", TEXT("vector, 2.5"), TEXT(".X"), 7.5, false},
		{UKismetStringLibrary::StaticClass(), "KismetStringLibrary", "Len", TEXT("\"Lhat binding benchmark\""), TEXT(""), 22, false},
		{UKismetStringLibrary::StaticClass(), "KismetStringLibrary", "Concat_StrStr", TEXT("\"Lhat \", \"binding benchmark\""), TEXT(""), 0, true}
	};
	TArray<TUniquePtr<FCachedCall>> Calls;
	for (FCase& C : Cases)
	{
		auto Call = MakeUnique<FCachedCall>();
		if (!Call->Register(Program, C.Class, C.Type, C.Name)) { UE_LOG(LogLhatBenchmark, Error, TEXT("Cannot cache %hs"), C.Name); return 1; }
		Calls.Add(MoveTemp(Call));
	}
	for (FCase& C : Cases)
	{
		for (int32 Mode = 0; Mode != 2; ++Mode)
		{
			const FString Name = FString::Printf(TEXT("%hs-%d.lh"), C.Name, Mode);
			if (!FFileHelper::SaveStringToFile(MakeSource(C, Mode != 0), *(ScriptDirectory / Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 1;
			C.Units[Mode] = Program.Check(Name);
			if (!C.Units[Mode] || !lhat_unit_ok(C.Units[Mode])) { UE_LOG(LogLhatBenchmark, Error, TEXT("%s"), *Program.GetDiagnostics()); return 1; }
		}
	}
	if (!Program.Compile()) { UE_LOG(LogLhatBenchmark, Error, TEXT("%s"), *Program.GetDiagnostics()); return 1; }
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema"), 1);
	Root->SetStringField(TEXT("utc"), FDateTime::UtcNow().ToIso8601());
	Root->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
	Root->SetStringField(TEXT("cpu"), FPlatformMisc::GetCPUBrand());
	Root->SetStringField(TEXT("configuration"), LexToString(FApp::GetBuildConfiguration()));
#if defined(__clang__)
	Root->SetStringField(TEXT("bridge_compiler"), TEXT("Clang ") TEXT(__clang_version__));
#elif defined(_MSC_FULL_VER)
	Root->SetStringField(TEXT("bridge_compiler"), FString::Printf(TEXT("MSVC %d"), _MSC_FULL_VER));
#endif
	Root->SetStringField(TEXT("scope"), TEXT("Warm Lhat loop + codecs + native body; generated direct C++ vs cached ProcessEvent. Not Blueprint bytecode, Shipping, or isolated dispatch latency."));
	Root->SetStringField(TEXT("scripts"), ScriptDirectory);
	Root->SetNumberField(TEXT("iterations"), Iterations);
	Root->SetNumberField(TEXT("samples"), Samples);
	Root->SetNumberField(TEXT("warmups"), Warmups);
	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FCase& C : Cases)
	{
		FRunner Runners[2];
		if (!Runners[0].Start(Program, C.Units[0]) || !Runners[1].Start(Program, C.Units[1])) { UE_LOG(LogLhatBenchmark, Error, TEXT("Cannot start %hs; generate/install native providers first."), C.Name); return 1; }
		TArray<double> Times[2];
		TArray<double> Deltas;
		for (int32 Round = -Warmups; Round < Samples; ++Round)
		{
			double Pair[2];
			for (int32 Order = 0; Order < 2; ++Order)
			{
				const int32 Mode = ((Round + Warmups) % 2) ^ Order;
				if (!Runners[Mode].Measure(Program, C, Iterations, Pair[Mode])) { UE_LOG(LogLhatBenchmark, Error, TEXT("Incorrect result: %hs, mode %d, round %d"), C.Name, Mode, Round); return 1; }
			}
			if (Round >= 0) { Times[0].Add(Pair[0]); Times[1].Add(Pair[1]); Deltas.Add(Pair[1] - Pair[0]); }
		}
		const double Direct = Median(Times[0]), Cached = Median(Times[1]);
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("function"), C.Class->FindFunctionByName(FName(UTF8_TO_TCHAR(C.Name)))->GetPathName());
		Row->SetArrayField(TEXT("static_ns_samples"), JsonNumbers(Times[0]));
		Row->SetArrayField(TEXT("cached_ns_samples"), JsonNumbers(Times[1]));
		Row->SetNumberField(TEXT("static_ns_median"), Direct);
		Row->SetNumberField(TEXT("cached_ns_median"), Cached);
		Row->SetNumberField(TEXT("ratio"), Cached / Direct);
		Row->SetNumberField(TEXT("paired_delta_ns_median"), Median(Deltas));
		Row->SetBoolField(TEXT("checksums_verified"), true);
		Results.Add(MakeShared<FJsonValueObject>(Row));
		UE_LOG(LogLhatBenchmark, Display, TEXT("%hs: static %.1f ns, cached %.1f ns, %.2fx, paired delta %.1f ns"), C.Name, Direct, Cached, Cached / Direct, Median(Deltas));
	}
	Root->SetArrayField(TEXT("results"), Results);
	FString Json;
	if (!FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json)) || !FFileHelper::SaveStringToFile(Json, *Output, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 1;
	UE_LOG(LogLhatBenchmark, Display, TEXT("LHAT BENCHMARK COMPLETE: %s"), *Output);
	return 0;
#endif
}
