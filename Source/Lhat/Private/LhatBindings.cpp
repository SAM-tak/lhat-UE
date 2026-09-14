#include "LhatBindings.h"

#include "GameFramework/Actor.h"
#include "LhatModule.h"
#include "LhatNativeBindings.h"

namespace
{
	struct FLhatObjectHandle
	{
		TWeakObjectPtr<UObject> Object;
	};

	// Unlike per-program function contexts, a dispose context must be process-stable.
	void DisposeObject(LhatMachine*, void*, const LhatValue* Args, size_t Count, LhatValue*, int*)
	{
		if (Count == 1 && lhat_is_object_kind(Args[0], LHAT_OBJECT_HOSTDATA))
		{
			auto* Data = reinterpret_cast<LhatHostData*>(lhat_as_object(Args[0]));
			delete static_cast<FLhatObjectHandle*>(Data->pointer);
		}
	}

	void Log(LhatMachine*, void*, const LhatValue* Args, size_t, LhatValue*, int*)
	{
		const auto* String = reinterpret_cast<const LhatString*>(lhat_as_object(Args[0]));
		FUTF8ToTCHAR Converted(String->text, static_cast<int32>(String->length));
		UE_LOG(LogLhat, Display, TEXT("%s"), *FString(Converted.Length(), Converted.Get()));
	}

	void GetName(LhatMachine* Machine, void* Context, const LhatValue* Args, size_t, LhatValue* Answers, int* Count)
	{
		auto& Bindings = *static_cast<FLhatBindings*>(Context);
		if (UObject* Object = Bindings.ReadObject(Machine, Args[0], UObject::StaticClass()))
		{
			FTCHARToUTF8 Name(*Object->GetName());
			if (lhat_machine_make_string(Machine, Name.Get(), Name.Length(), &Answers[0]))
			{
				*Count = 1;
			}
		}
	}

	void IsObjectValid(LhatMachine*, void* Context, const LhatValue* Args, size_t, LhatValue* Answers, int* Count)
	{
		auto* Handle = static_cast<FLhatObjectHandle*>(lhat_hostdata_pointer(Args[0], static_cast<FLhatBindings*>(Context)->ObjectTag));
		Answers[0] = lhat_bool(Handle != nullptr && Handle->Object.IsValid());
		*Count = 1;
	}

	void MakeVector(LhatMachine* Machine, void* Context, const LhatValue* Args, size_t ArgumentCount, LhatValue* Answers, int* Count)
	{
		if (ArgumentCount != 3 || !lhat_is_number(Args[0]) || !lhat_is_number(Args[1]) || !lhat_is_number(Args[2]))
		{
			lhat_machine_panic_text(Machine, "MakeVector expects three numbers");
			return;
		}
		const FVector Value(lhat_number_as_real(Args[0]), lhat_number_as_real(Args[1]), lhat_number_as_real(Args[2]));
		if (lhat_make_hostvalue(Machine, static_cast<FLhatBindings*>(Context)->VectorTag, &Value, &Answers[0]))
		{
			*Count = 1;
		}
	}

	void SetLocation(LhatMachine* Machine, void* Context, const LhatValue* Args, size_t, LhatValue* Answers, int* Count)
	{
		auto& Bindings = *static_cast<FLhatBindings*>(Context);
		if (auto* Actor = Cast<AActor>(Bindings.ReadObject(Machine, Args[0], AActor::StaticClass())))
		{
			FVector Value;
			const void* Data = lhat_hostvalue_data(Args[1], Bindings.VectorTag);
			if (Data == nullptr)
			{
				lhat_machine_panic_text(Machine, "SetActorLocation expects ue.Vector");
				return;
			}
			FMemory::Memcpy(&Value, Data, sizeof(Value));
			Answers[0] = lhat_bool(Actor->SetActorLocation(Value, false));
			*Count = 1;
		}
	}

	void GetLocation(LhatMachine* Machine, void* Context, const LhatValue* Args, size_t, LhatValue* Answers, int* Count)
	{
		auto& B = *static_cast<FLhatBindings*>(Context);
		if (auto* Actor = static_cast<AActor*>(B.ReadObject(Machine, Args[0], AActor::StaticClass())))
		{
			const FVector Value = Actor->K2_GetActorLocation();
			if (lhat_make_hostvalue(Machine, B.VectorTag, &Value, &Answers[0])) *Count = 1;
		}
	}
	void SetHidden(LhatMachine* Machine, void* Context, const LhatValue* Args, size_t, LhatValue*, int*)
	{
		auto& B = *static_cast<FLhatBindings*>(Context);
		if (auto* Actor = static_cast<AActor*>(B.ReadObject(Machine, Args[0], AActor::StaticClass())))
		{
			bool Hidden;
			if (LhatUEBindings::Read(Machine, B, Args[1], Hidden)) Actor->SetActorHiddenInGame(Hidden);
		}
	}
	void IsTickEnabled(LhatMachine* Machine, void* Context, const LhatValue* Args, size_t, LhatValue* Answers, int* Count)
	{
		auto& B = *static_cast<FLhatBindings*>(Context);
		if (auto* Actor = static_cast<AActor*>(B.ReadObject(Machine, Args[0], AActor::StaticClass())))
		{
			Answers[0] = lhat_bool(Actor->IsActorTickEnabled());
			*Count = 1;
		}
	}
}

bool FLhatBindings::Register(LhatProgram* Program)
{
	check(IsInGameThread());
	ObjectTag = lhat_register_hostdata_type(Program, "ue", "Object");
	ActorTag = lhat_register_hostdata_subtype(Program, "ue", "Actor", "ue", "Object");
	VectorTag = lhat_register_hostvalue_type(Program, "ue", "Vector", sizeof(FVector));
	return ObjectTag && ActorTag && VectorTag
		&& lhat_register_member(Program, "ue", "Object", "dispose", "p^self^;", DisposeObject, nullptr)
		&& lhat_register_member(Program, "ue", "Object", "GetName", "f^self^ -> string^;", GetName, this)
		&& lhat_register_member(Program, "ue", "Object", "IsValid", "f^self^ -> bool^;", IsObjectValid, this)
		&& lhat_register_member(Program, "ue", "Actor", "GetActorLocation", "f^self^ -> ue.Vector;", GetLocation, this)
		&& lhat_register_member(Program, "ue", "Actor", "SetActorHiddenInGame", "p^self^, bool^;", SetHidden, this)
		&& lhat_register_member(Program, "ue", "Actor", "IsActorTickEnabled", "f^self^ -> bool^;", IsTickEnabled, this)
		&& lhat_register_member(Program, "ue", "Actor", "SetActorLocation", "p^self^, ue.Vector -> bool^;", SetLocation, this)
		// See Docs/KnownIssues.md: type-level hostvalue methods misplace arguments in the pinned core.
		&& lhat_register_func(Program, "ue", "MakeVector", "f^number^, number^, number^ -> ue.Vector;", MakeVector, this)
		&& lhat_register_hostvalue_field(Program, "ue", "Vector", "X", STRUCT_OFFSET(FVector, X), LHAT_HVFIELD_F64)
		&& lhat_register_hostvalue_field(Program, "ue", "Vector", "Y", STRUCT_OFFSET(FVector, Y), LHAT_HVFIELD_F64)
		&& lhat_register_hostvalue_field(Program, "ue", "Vector", "Z", STRUCT_OFFSET(FVector, Z), LHAT_HVFIELD_F64)
		&& lhat_register_func(Program, "ue", "Log", "p^string^;", Log, this)
		&& lhat_register_annotation(Program, "ue", "EditAnywhere", LHAT_ANNOTATION_FIELD)
		&& lhat_register_annotation_signature(Program, "EditAnywhere", "p^;")
		&& LhatUEBindings::RegisterProviders(Program, *this);
}

bool FLhatBindings::WrapObject(LhatMachine* Machine, UObject* Object, LhatValue& Out) const
{
	check(IsInGameThread());
	if (!IsValid(Object))
	{
		Out = lhat_nil();
		return true;
	}
	Out = lhat_machine_weak_cache_get(Machine, Object);
	if (auto* Cached = static_cast<FLhatObjectHandle*>(lhat_hostdata_pointer(Out, ObjectTag)))
	{
		if (Cached->Object.Get() == Object)
		{
			return true;
		}
	}
	const auto* Tag = Object->IsA<AActor>() ? ActorTag : ObjectTag;
	for (UClass* Class = Object->GetClass(); Class; Class = Class->GetSuperClass())
	{
		if (const FObjectType* Type = ObjectTypes.Find(Class)) { Tag = Type->Tag; break; }
	}
	auto* Handle = new FLhatObjectHandle{Object};
	if (!lhat_machine_make_hostdata(Machine, Tag, Handle, &Out))
	{
		delete Handle;
		return false;
	}
	return lhat_machine_weak_cache_put(Machine, Object, Out);
}

UObject* FLhatBindings::ReadObject(LhatMachine* Machine, LhatValue Value, UClass* ExpectedClass) const
{
	check(IsInGameThread());
	auto* Handle = static_cast<FLhatObjectHandle*>(lhat_hostdata_pointer(Value, ObjectTag));
	UObject* Object = Handle ? Handle->Object.Get() : nullptr;
	if (!Object || !Object->IsA(ExpectedClass))
	{
		lhat_machine_panic_text(Machine, "UE object is destroyed, disposed, or has an incompatible type");
		return nullptr;
	}
	return Object;
}
