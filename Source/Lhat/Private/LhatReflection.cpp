#include "LhatBindings.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

struct FLhatReflectedCall
{
	FLhatBindings* Bindings;
	UClass* OwnerClass;
	UFunction* Function;
	TArray<FProperty*> Inputs;
	FProperty* Return;
};

FLhatBindings::FLhatBindings() = default;
FLhatBindings::~FLhatBindings() = default;

namespace
{
	FString PropertySignature(FProperty* Property)
	{
		if (CastField<FBoolProperty>(Property)) return TEXT("bool^");
		if (CastField<FIntProperty>(Property) || CastField<FFloatProperty>(Property) || CastField<FDoubleProperty>(Property)) return TEXT("number^");
		if (CastField<FStrProperty>(Property) || CastField<FNameProperty>(Property)) return TEXT("string^");
		if (auto* Struct = CastField<FStructProperty>(Property); Struct && Struct->Struct == TBaseStructure<FVector>::Get()) return TEXT("ue.Vector");
		return FString();
	}

	bool WriteArgument(FLhatBindings& Bindings, FProperty* Property, void* Address, LhatValue Value)
	{
		if (auto* Bool = CastField<FBoolProperty>(Property); Bool && lhat_is_bool(Value))
		{
			Bool->SetPropertyValue(Address, lhat_as_bool(Value));
			return true;
		}
		if (lhat_is_number(Value))
		{
			const double Number = lhat_is_integer(Value) ? static_cast<double>(lhat_as_integer(Value)) : lhat_as_real(Value);
			if (auto* Double = CastField<FDoubleProperty>(Property)) { Double->SetPropertyValue(Address, Number); return true; }
			if (auto* Float = CastField<FFloatProperty>(Property)) { Float->SetPropertyValue(Address, static_cast<float>(Number)); return true; }
			if (auto* Int = CastField<FIntProperty>(Property); Int && Number >= MIN_int32 && Number <= MAX_int32 && FMath::FloorToDouble(Number) == Number)
			{
				Int->SetPropertyValue(Address, static_cast<int32>(Number));
				return true;
			}
		}
		if (lhat_is_object_kind(Value, LHAT_OBJECT_STRING))
		{
			const auto* String = reinterpret_cast<const LhatString*>(lhat_as_object(Value));
			FUTF8ToTCHAR Converted(String->text, static_cast<int32>(String->length));
			const FString Text(Converted.Length(), Converted.Get());
			if (auto* Str = CastField<FStrProperty>(Property)) { Str->SetPropertyValue(Address, Text); return true; }
			if (auto* Name = CastField<FNameProperty>(Property)) { Name->SetPropertyValue(Address, FName(*Text)); return true; }
		}
		if (CastField<FStructProperty>(Property))
		{
			if (const void* Data = lhat_hostvalue_data(Value, Bindings.VectorTag))
			{
				FMemory::Memcpy(Address, Data, sizeof(FVector));
				return true;
			}
		}
		return false;
	}

	bool ReadReturn(FLhatBindings& Bindings, LhatMachine* Machine, FProperty* Property, const void* Address, LhatValue& Value)
	{
		if (auto* Bool = CastField<FBoolProperty>(Property)) { Value = lhat_bool(Bool->GetPropertyValue(Address)); return true; }
		if (auto* Int = CastField<FIntProperty>(Property)) { Value = lhat_integer(Int->GetPropertyValue(Address)); return true; }
		if (auto* Float = CastField<FFloatProperty>(Property)) { Value = lhat_real(Float->GetPropertyValue(Address)); return true; }
		if (auto* Double = CastField<FDoubleProperty>(Property)) { Value = lhat_real(Double->GetPropertyValue(Address)); return true; }
		if (CastField<FStrProperty>(Property) || CastField<FNameProperty>(Property))
		{
			const FString Text = CastField<FStrProperty>(Property) ? CastFieldChecked<FStrProperty>(Property)->GetPropertyValue(Address)
				: CastFieldChecked<FNameProperty>(Property)->GetPropertyValue(Address).ToString();
			FTCHARToUTF8 Converted(*Text);
			return lhat_machine_make_string(Machine, Converted.Get(), Converted.Length(), &Value);
		}
		return CastField<FStructProperty>(Property) && lhat_make_hostvalue(Machine, Bindings.VectorTag, Address, &Value);
	}

	void ReflectedCall(LhatMachine* Machine, void* Context, const LhatValue* Args, size_t Count, LhatValue* Answers, int* AnswerCount)
	{
		auto& Call = *static_cast<FLhatReflectedCall*>(Context);
		if (Count != static_cast<size_t>(Call.Inputs.Num() + 1)) { lhat_machine_panic_text(Machine, "Incorrect UE argument count"); return; }
		UObject* Object = Call.Bindings->ReadObject(Machine, Args[0], Call.OwnerClass);
		if (!Object) return;
		if (const auto* Actor = Cast<AActor>(Object); Actor && (!Actor->GetWorld() || !Actor->GetWorld()->AreActorsInitialized()))
		{
			lhat_machine_panic_text(Machine, "UE Actor world is not initialized for play");
			return;
		}
		// Initialize/destroy nontrivial properties; return offsets come from reflection.
		FStructOnScope Parameters(Call.Function);
		for (int32 Index = 0; Index < Call.Inputs.Num(); ++Index)
		{
			FProperty* Property = Call.Inputs[Index];
			if (!WriteArgument(*Call.Bindings, Property, Property->ContainerPtrToValuePtr<void>(Parameters.GetStructMemory()), Args[Index + 1]))
			{
				lhat_machine_panic_text(Machine, "UE argument has an incompatible type or range");
				return;
			}
		}
		Object->ProcessEvent(Call.Function, Parameters.GetStructMemory());
		if (Call.Return)
		{
			if (ReadReturn(*Call.Bindings, Machine, Call.Return, Call.Return->ContainerPtrToValuePtr<void>(Parameters.GetStructMemory()), Answers[0])) *AnswerCount = 1;
			else lhat_machine_panic_text(Machine, "Could not marshal the UE return value");
		}
	}
}

bool FLhatBindings::RegisterFunction(LhatProgram* Program, UClass* Class, const char* Type, const char* Name, const TCHAR* EngineName)
{
	if (!Class || !Class->HasAnyClassFlags(CLASS_Native)) return false;
	UFunction* Function = Class->FindFunctionByName(EngineName);
	// Initial scope: explicit native, synchronous instance methods. No arbitrary discovery.
	if (!Function || !Function->HasAnyFunctionFlags(FUNC_Native) || Function->HasAnyFunctionFlags(FUNC_Static | FUNC_Net | FUNC_Delegate)) return false;
	auto Call = MakeUnique<FLhatReflectedCall>();
	Call->Bindings = this;
	Call->OwnerClass = Class;
	Call->Function = Function;
	Call->Return = Function->GetReturnProperty();
	FString Signature = Function->HasAnyFunctionFlags(FUNC_BlueprintPure) ? TEXT("f^self^") : TEXT("p^self^");
	for (TFieldIterator<FProperty> Property(Function); Property; ++Property)
	{
		if (!Property->HasAnyPropertyFlags(CPF_Parm) || *Property == Call->Return) continue;
		if (Property->ArrayDim != 1 || (Property->HasAnyPropertyFlags(CPF_OutParm) && !Property->HasAnyPropertyFlags(CPF_ConstParm))) return false;
		const FString TypeText = PropertySignature(*Property);
		if (TypeText.IsEmpty()) return false;
		Call->Inputs.Add(*Property);
		Signature += TEXT(", ") + TypeText;
	}
	if (Call->Return)
	{
		const FString TypeText = PropertySignature(Call->Return);
		if (TypeText.IsEmpty() || Call->Return->ArrayDim != 1) return false;
		Signature += TEXT(" -> ") + TypeText;
	}
	Signature += TEXT(";");
	FTCHARToUTF8 Text(*Signature);
	if (!lhat_register_member(Program, "ue", Type, Name, Text.Get(), ReflectedCall, Call.Get())) return false;
	Calls.Add(MoveTemp(Call));
	return true;
}
