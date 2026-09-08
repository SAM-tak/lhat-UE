#include "LhatInstance.h"

#include "GameFramework/Actor.h"
#include "LhatBindings.h"
#include "StructUtils/PropertyBag.h"

namespace
{
	constexpr int64 ExecutionBudget = 100000;
}

FLhatInstance::FLhatInstance(TSharedRef<FLhatProgram> InProgram) : Program(InProgram)
{
}

FLhatInstance::~FLhatInstance()
{
	// Lifecycle callbacks are explicit; destruction must not re-enter a dying world.
	FLhatProgram::DestroyMachine(Machine);
}

bool FLhatInstance::Accept(const LhatRunResult& Result, const TCHAR* Operation)
{
	if (Result.status == LHAT_RUN_OK)
	{
		return true;
	}
	bFaulted = true;
	Error = FString::Printf(TEXT("%s: %s"), Operation, UTF8_TO_TCHAR(lhat_run_status_message(Result.status)));
	if (Result.status == LHAT_RUN_PANIC)
	{
		const size_t ValueLength = lhat_value_text(Result.value, nullptr, 0);
		TArray<char> ValueText;
		ValueText.SetNumZeroed(static_cast<int32>(ValueLength + 1));
		lhat_value_text(Result.value, ValueText.GetData(), ValueText.Num());
		Error += FString::Printf(TEXT(": %s"), UTF8_TO_TCHAR(ValueText.GetData()));
	}
	if (Result.status == LHAT_RUN_SUSPENDED)
	{
		Error += TEXT(" (script exceeded the per-callback execution budget)");
	}
	const size_t Length = lhat_machine_traceback(Machine, nullptr, 0);
	TArray<char> Trace;
	Trace.SetNumZeroed(static_cast<int32>(Length + 1));
	lhat_machine_traceback(Machine, Trace.GetData(), Trace.Num());
	Error += TEXT("\n");
	Error += UTF8_TO_TCHAR(Trace.GetData());
	return false;
}

bool FLhatInstance::Start(const LhatUnit* Unit, AActor* Owner, const FInstancedPropertyBag* Parameters)
{
	check(IsInGameThread());
	if (bStartAttempted || Machine || !Unit || !lhat_unit_proto(Unit) || !IsValid(Owner))
	{
		Error = TEXT("A fresh instance, a compiled unit, and a live Actor are required.");
		return false;
	}
	bStartAttempted = true;
	Machine = Program->CreateMachine();
	if (!Machine || !Program->Install(Machine))
	{
		Error = TEXT("Could not create/install the L^ machine.");
		return false;
	}
	lhat_machine_set_budget(Machine, ExecutionBudget);
	const LhatRunResult Module = lhat_run(Machine, lhat_unit_proto(Unit));
	if (!Accept(Module, TEXT("Module")))
	{
		return false;
	}
	if (!lhat_is_object_kind(Module.value, LHAT_OBJECT_TABLE))
	{
		Error = TEXT("A component script must be a module with exactly one public def^.");
		return false;
	}
	// Root the returned module even if the source returned an unregistered table.
	lhat_machine_set_global(Machine, "__ue_module", Module.value);
	const auto* Exports = reinterpret_cast<const LhatTable*>(lhat_as_object(Module.value));
	LhatValue Definition = lhat_nil();
	int32 Definitions = 0;
	for (size_t Index = 0; Index < Exports->entry_capacity; ++Index)
	{
		const LhatValue Value = Exports->entries[Index].value;
		if (lhat_is_object_kind(Value, LHAT_OBJECT_TABLE) && reinterpret_cast<const LhatTable*>(lhat_as_object(Value))->is_definition)
		{
			Definition = Value;
			++Definitions;
		}
	}
	if (Definitions != 1)
	{
		Error = TEXT("A component script must publish exactly one def^.");
		return false;
	}
	LhatValue Actor = lhat_nil();
	if (!Program->GetBindings().WrapObject(Machine, Owner, Actor) || !lhat_machine_set_global(Machine, "__ue_owner", Actor))
	{
		Error = TEXT("Could not create the Actor handle.");
		return false;
	}
	lhat_machine_set_budget(Machine, ExecutionBudget);
	const LhatRunResult Created = lhat_machine_call_member(Machine, Definition, "new", 3, &Actor, 1);
	if (!Accept(Created, TEXT("new")))
	{
		return false;
	}
	if (!lhat_is_object_kind(Created.value, LHAT_OBJECT_TABLE) || !lhat_machine_set_global(Machine, "__ue_instance", Created.value))
	{
		Error = TEXT("new must return a script instance.");
		return false;
	}
	State = Created.value;
	if (Parameters && Parameters->GetPropertyBagStruct())
	{
		for (const FPropertyBagPropertyDesc& Desc : Parameters->GetPropertyBagStruct()->GetPropertyDescs())
		{
			if (!Desc.ContainerTypes.IsEmpty())
			{
				Error = TEXT("Container-valued script parameters are not supported.");
				return false;
			}
			FTCHARToUTF8 Name(*Desc.Name.ToString());
			LhatValue Key;
			if (!lhat_machine_make_string(Machine, Name.Get(), Name.Length(), &Key))
			{
				Error = TEXT("Could not allocate a parameter name.");
				return false;
			}
			const LhatValue Previous = lhat_table_get(reinterpret_cast<LhatTable*>(lhat_as_object(State)), Key);
			LhatValue Value = lhat_nil();
			if (Desc.ValueType == EPropertyBagPropertyType::Double && lhat_is_number(Previous))
			{
				Value = lhat_real(Parameters->GetValueDouble(Desc.Name).GetValue());
			}
			else if (Desc.ValueType == EPropertyBagPropertyType::Bool && lhat_is_bool(Previous))
			{
				Value = lhat_bool(Parameters->GetValueBool(Desc.Name).GetValue());
			}
			else if (Desc.ValueType == EPropertyBagPropertyType::String && lhat_is_object_kind(Previous, LHAT_OBJECT_STRING))
			{
				const FString String = Parameters->GetValueString(Desc.Name).GetValue();
				FTCHARToUTF8 Text(*String, String.Len());
				if (!lhat_machine_make_string(Machine, Text.Get(), Text.Length(), &Value))
				{
					Error = TEXT("Could not allocate a parameter value.");
					return false;
				}
			}
			else
			{
				Error = FString::Printf(TEXT("Parameter %s no longer matches the script. Refresh Parameters in the editor."), *Desc.Name.ToString());
				return false;
			}
			bool bRefused = false;
			if (!lhat_machine_table_set(Machine, reinterpret_cast<LhatTable*>(lhat_as_object(State)), Key, Value, &bRefused) || bRefused)
			{
				Error = TEXT("Could not apply script parameters.");
				return false;
			}
		}
	}
	bHasTick = HasMember("Tick");
	bStarted = true;
	return !HasMember("BeginPlay") || Call("BeginPlay", nullptr, 0);
}

bool FLhatInstance::HasMember(const char* Name) const
{
	LhatValue Key;
	return lhat_machine_make_string(Machine, Name, FCStringAnsi::Strlen(Name), &Key)
		&& !lhat_is_nil(lhat_table_get(reinterpret_cast<const LhatTable*>(lhat_as_object(State)), Key));
}

bool FLhatInstance::Call(const char* Name, const LhatValue* Arguments, size_t Count)
{
	lhat_machine_set_budget(Machine, ExecutionBudget);
	return Accept(lhat_machine_call_member(Machine, State, Name, FCStringAnsi::Strlen(Name), Arguments, Count), UTF8_TO_TCHAR(Name));
}

bool FLhatInstance::Tick(double DeltaSeconds)
{
	check(IsInGameThread());
	const LhatValue Delta = lhat_real(DeltaSeconds);
	return IsRunning() && (!bHasTick || Call("Tick", &Delta, 1));
}

void FLhatInstance::Stop(bool bDispatchEndPlay)
{
	check(IsInGameThread());
	if (bDispatchEndPlay && IsRunning() && HasMember("EndPlay"))
	{
		Call("EndPlay", nullptr, 0);
	}
	bStarted = false;
	State = lhat_nil();
	FLhatProgram::DestroyMachine(Machine);
	Machine = nullptr;
}
