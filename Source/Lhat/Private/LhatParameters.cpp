#include "LhatScript.h"

#include "StructUtils/PropertyBag.h"

bool FLhatProgram::ReadParameterDefaults(const FString& EntryPoint, FInstancedPropertyBag& Out, FString& Error)
{
	check(IsInGameThread());
	Error.Empty();
	const LhatUnit* Unit = Check(EntryPoint);
	if (!Unit || !lhat_unit_ok(Unit) || !Compile())
	{
		Error = GetDiagnostics();
		if (Error.IsEmpty()) Error = TEXT("Invalid script path or script could not be compiled.");
		return false;
	}
	LhatMachine* Machine = CreateMachine();
	if (!Machine || !Install(Machine))
	{
		DestroyMachine(Machine);
		Error = TEXT("Cannot create the parameter inspection machine.");
		return false;
	}
	// Inspect declarations only. No Actor is supplied and new/BeginPlay are never called.
	lhat_machine_set_budget(Machine, 100000);
	const LhatRunResult Result = lhat_run(Machine, lhat_unit_proto(Unit));
	bool bSuccess = false;
	FInstancedPropertyBag Defaults;
	if (Result.status == LHAT_RUN_OK && lhat_is_object_kind(Result.value, LHAT_OBJECT_TABLE))
	{
		lhat_machine_set_global(Machine, "__ue_inspect", Result.value);
		const auto* Exports = reinterpret_cast<const LhatTable*>(lhat_as_object(Result.value));
		const LhatTable* Definition = nullptr;
		FString DefinitionName;
		int32 Definitions = 0;
		for (size_t Index = 0; Index < Exports->entry_capacity; ++Index)
		{
			const LhatTableEntry& Entry = Exports->entries[Index];
			if (lhat_is_object_kind(Entry.value, LHAT_OBJECT_TABLE) && lhat_is_object_kind(Entry.key, LHAT_OBJECT_STRING))
			{
				const auto* Candidate = reinterpret_cast<const LhatTable*>(lhat_as_object(Entry.value));
				if (Candidate->is_definition)
				{
					Definition = Candidate;
					DefinitionName = UTF8_TO_TCHAR(reinterpret_cast<const LhatString*>(lhat_as_object(Entry.key))->text);
					++Definitions;
				}
			}
		}
		if (Definitions == 1)
		{
			const LhatValue PrototypeValue = lhat_table_get_bytes(Definition, "self^", 5);
			const LhatTable* Prototype = lhat_is_object_kind(PrototypeValue, LHAT_OBJECT_TABLE)
				? reinterpret_cast<const LhatTable*>(lhat_as_object(PrototypeValue)) : nullptr;
			FTCHARToUTF8 DefinitionUtf8(*DefinitionName);
			bSuccess = true;
			for (size_t Index = 0; Index < lhat_unit_member_count(Unit, DefinitionUtf8.Get()); ++Index)
			{
				const LhatUnitMember Member = lhat_unit_member(Unit, DefinitionUtf8.Get(), Index);
				// Reflection returns a source span, not a null-terminated C string.
				FUTF8ToTCHAR MemberText(Member.name, static_cast<int32>(Member.name_length));
				const FString MemberName(MemberText.Length(), MemberText.Get());
				FTCHARToUTF8 MemberUtf8(*MemberName);
				bool bExported = false;
				for (size_t Annotation = 0; Annotation < lhat_unit_annotation_count(Unit, DefinitionUtf8.Get(), MemberUtf8.Get()); ++Annotation)
				{
					const LhatAnnotation At = lhat_unit_annotation(Unit, DefinitionUtf8.Get(), MemberUtf8.Get(), Annotation);
					bExported |= At.name_length == 12 && FMemory::Memcmp(At.name, "EditAnywhere", 12) == 0;
				}
				if (!bExported) continue;
				const LhatValue Value = Prototype ? lhat_table_get_bytes(Prototype, Member.name, Member.name_length) : lhat_nil();
				const FName Name(*MemberName);
				if (Defaults.GetPropertyBagStruct() && Defaults.GetPropertyBagStruct()->FindPropertyDescByName(Name))
				{
					Error = FString::Printf(TEXT("%s: parameter names must also be unique ignoring case in UE."), *Name.ToString());
					bSuccess = false;
					break;
				}
				EPropertyBagPropertyType Type = EPropertyBagPropertyType::None;
				if (lhat_is_number(Value)) Type = EPropertyBagPropertyType::Double;
				else if (lhat_is_bool(Value)) Type = EPropertyBagPropertyType::Bool;
				else if (lhat_is_object_kind(Value, LHAT_OBJECT_STRING)) Type = EPropertyBagPropertyType::String;
				if (Type == EPropertyBagPropertyType::None)
				{
					Error = FString::Printf(TEXT("%s: EditAnywhere requires a number, bool, or string default."), *Name.ToString());
					bSuccess = false;
					break;
				}
				FPropertyBagPropertyDesc Desc(Name, Type);
				Desc.ID = FGuid::NewDeterministicGuid(EntryPoint + TEXT(":") + Name.ToString());
				Defaults.AddProperties(MakeArrayView(&Desc, 1));
				if (Type == EPropertyBagPropertyType::Double) Defaults.SetValueDouble(Name, lhat_is_integer(Value) ? static_cast<double>(lhat_as_integer(Value)) : lhat_as_real(Value));
				else if (Type == EPropertyBagPropertyType::Bool) Defaults.SetValueBool(Name, lhat_as_bool(Value));
				else
				{
					const auto* String = reinterpret_cast<const LhatString*>(lhat_as_object(Value));
					FUTF8ToTCHAR Text(String->text, static_cast<int32>(String->length));
					Defaults.SetValueString(Name, FString(Text.Length(), Text.Get()));
				}
			}
		}
	}
	DestroyMachine(Machine);
	if (bSuccess) Out = MoveTemp(Defaults);
	else if (Error.IsEmpty()) Error = TEXT("Parameter inspection requires a module with exactly one public def^ and successful top-level execution.");
	return bSuccess;
}
