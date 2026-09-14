#include "LhatDynamicBindings.h"
#include "LhatNativeBindings.h"
#include "LhatModule.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/TextProperty.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
	// Reserve one prefix to make punctuation escaping injective. Ordinary UE
	// names (including Japanese identifiers) keep their existing spelling.
	FString SafeName(const FString& Name)
	{
		bool Safe = !Name.IsEmpty() && !Name.StartsWith(TEXT("_LH_"));
		for (int32 I = 0; I < Name.Len(); ++I)
			Safe &= Name[I] == '_' || FChar::IsAlpha(Name[I]) || (I && FChar::IsDigit(Name[I]));
		if (Safe) return Name;
		FString Result(TEXT("_LH_"));
		FTCHARToUTF8 Utf8(*Name);
		for (int32 I = 0; I < Utf8.Length(); ++I) Result += FString::Printf(TEXT("%02X"), uint8(Utf8.Get()[I]));
		return Result;
	}

	TArray<char> Utf8Copy(const FString& Text)
	{
		FTCHARToUTF8 Converted(*Text);
		TArray<char> Result;
		Result.Append(Converted.Get(), Converted.Length() + 1);
		return Result;
	}

	bool UsableClass(UClass* Class)
	{
		if (!IsValid(Class) || Class->HasAnyClassFlags(CLASS_Interface | CLASS_Deprecated | CLASS_NewerVersionExists)) return false;
		const FString Name = Class->GetName();
		for (const TCHAR* Prefix : {TEXT("SKEL_"), TEXT("REINST_"), TEXT("TRASHCLASS_"), TEXT("HOTRELOADED_")})
			if (Name.StartsWith(Prefix)) return false;
		return Class == UObject::StaticClass() || Class->IsChildOf(UObject::StaticClass());
	}

	void DiscoverProjectBlueprints()
	{
#if WITH_EDITOR
		// Never load disabled plugins or force-load optional native modules.
		// Loading is bounded to project content; Engine/plugin assets already in
		// memory participate too. The cooked runtime uses its loaded class set.
		static bool Discovering = false;
		if (Discovering) return;
		TGuardValue<bool> Guard(Discovering, true);
		auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.SearchAllAssets(true);
		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(TEXT("/Game"));
		for (const auto& Plugin : IPluginManager::Get().GetEnabledPlugins())
			if (Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project && Plugin->CanContainContent())
				Filter.PackagePaths.Add(FName(*Plugin->GetMountedAssetPath()));
		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString(); });
		for (const auto& Asset : Assets) Asset.GetAsset(); // Read only: never compile or save explicitly.
#endif
	}

	FString FunctionExclusion(UFunction* Fn)
	{
		if (!Fn->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)) return TEXT("not_blueprint_callable");
		if (Fn->HasAnyFunctionFlags(FUNC_Private | FUNC_Protected)) return TEXT("nonpublic");
		if (Fn->HasAnyFunctionFlags(FUNC_Net | FUNC_Delegate | FUNC_MulticastDelegate | FUNC_UbergraphFunction)) return TEXT("rpc_delegate_or_ubergraph");
#if WITH_METADATA
		for (const TCHAR* Key : {TEXT("Latent"), TEXT("LatentInfo"), TEXT("CustomThunk"), TEXT("CustomStructureParam"),
			TEXT("ArrayParm"), TEXT("MapParam"), TEXT("SetParam"), TEXT("BlueprintInternalUseOnly")})
			if (Fn->HasMetaData(Key)) return FString(TEXT("metadata:")) + Key;
#else
		// A cooked native CustomThunk can masquerade as an ordinary int pin
		// once metadata is stripped. Do not dispatch an unverified thunk. A
		// cook-time callable catalog is needed to enable these in cooked games.
		if (Fn->HasAnyFunctionFlags(FUNC_Native)) return TEXT("native_metadata_unavailable");
#endif
		return {};
	}
}

struct FLhatDynamicBindings::FImpl
{
	struct FType
	{
		TStrongObjectPtr<UClass> Class;
		FString Module, Name;
		TArray<char> ModuleUtf8, NameUtf8;
		FType(UClass* C, FString M, FString N) : Class(C), Module(MoveTemp(M)), Name(MoveTemp(N)), ModuleUtf8(Utf8Copy(Module)), NameUtf8(Utf8Copy(Name)) {}
		FString Qualified() const { return Module + TEXT(".") + Name; }
	};

	enum class EKind { Bool, I8, U8, I16, U16, I32, U32, I64, U64, Float, Double, String, Name, Text, Object, Class, Math };
	struct FSlot
	{
		FProperty* Property = nullptr;
		EKind Kind = EKind::Bool;
		int32 Offset = 0;
		uint8 BoolOffset = 0, BoolMask = 0, BoolSet = 0;
		UClass* ObjectClass = nullptr;
		UClass* MetaClass = nullptr;
		const LhatHostValueTag* ValueTag = nullptr;
		UScriptStruct* ValueStruct = nullptr;
		int32 ValueSize = 0;
		FString Signature;

		bool Build(FProperty* P, FImpl& State, FString& Reason)
		{
			Property = P;
			Offset = P->GetOffset_ForInternal();
			if (P->ArrayDim != 1) { Reason = TEXT("fixed_array"); return false; }
			if (auto* Bool = CastField<FBoolProperty>(P))
			{
				Kind = EKind::Bool; Signature = TEXT("bool^");
				BoolOffset = Bool->GetByteOffset(); BoolMask = Bool->GetFieldMask(); BoolSet = Bool->GetByteMask();
			}
			else if (auto* Numeric = CastField<FNumericProperty>(CastField<FEnumProperty>(P) ? CastFieldChecked<FEnumProperty>(P)->GetUnderlyingProperty() : P))
			{
				Signature = TEXT("number^");
				if (CastField<FInt8Property>(Numeric)) Kind = EKind::I8;
				else if (CastField<FByteProperty>(Numeric)) Kind = EKind::U8;
				else if (CastField<FInt16Property>(Numeric)) Kind = EKind::I16;
				else if (CastField<FUInt16Property>(Numeric)) Kind = EKind::U16;
				else if (CastField<FIntProperty>(Numeric)) Kind = EKind::I32;
				else if (CastField<FUInt32Property>(Numeric)) Kind = EKind::U32;
				else if (CastField<FInt64Property>(Numeric)) Kind = EKind::I64;
				else if (CastField<FUInt64Property>(Numeric)) Kind = EKind::U64;
				else if (CastField<FFloatProperty>(Numeric)) Kind = EKind::Float;
				else if (CastField<FDoubleProperty>(Numeric)) Kind = EKind::Double;
				else { Reason = TEXT("numeric_kind"); return false; }
			}
			else if (CastField<FStrProperty>(P)) { Kind = EKind::String; Signature = TEXT("string^"); }
			else if (CastField<FNameProperty>(P)) { Kind = EKind::Name; Signature = TEXT("string^"); }
			else if (CastField<FTextProperty>(P)) { Kind = EKind::Text; Signature = TEXT("string^"); }
			else if (auto* Obj = CastField<FObjectProperty>(P))
			{
				ObjectClass = Obj->PropertyClass;
				const FType* Type = State.FindType(ObjectClass);
				if (!Type) { Reason = TEXT("unavailable_object_class"); return false; }
				Kind = EKind::Object;
				if (auto* Class = CastField<FClassProperty>(P)) { Kind = EKind::Class; MetaClass = Class->MetaClass; }
				Signature = Type->Qualified() + TEXT("|nil^");
			}
			else if (auto* Struct = CastField<FStructProperty>(P))
			{
				Kind = EKind::Math;
				const char* Name = nullptr;
				if (Struct->Struct == TBaseStructure<FVector>::Get()) Name = "Vector";
				else if (Struct->Struct == TBaseStructure<FVector2D>::Get()) Name = "Vector2D";
				else if (Struct->Struct == TBaseStructure<FRotator>::Get()) Name = "Rotator";
				else if (Struct->Struct == TBaseStructure<FQuat>::Get()) Name = "Quat";
				else if (Struct->Struct == TBaseStructure<FTransform>::Get()) Name = "Transform";
				else if (Struct->Struct == TBaseStructure<FLinearColor>::Get()) Name = "LinearColor";
				else if (Struct->Struct == TBaseStructure<FColor>::Get()) Name = "Color";
				if (!Name) { Reason = TEXT("struct:") + Struct->Struct->GetPathName(); return false; }
				ValueTag = State.Bindings.GetValueTag(Name);
				ValueStruct = Struct->Struct;
				ValueSize = Struct->Struct->GetStructureSize();
				Signature = TEXT("ue.") + FString(UTF8_TO_TCHAR(Name));
				if (!ValueTag) { Reason = TEXT("missing_math_tag"); return false; }
			}
			else { Reason = TEXT("property:") + P->GetClass()->GetName(); return false; }
			return true;
		}

		// Constructors/destructors are independent of FProperty lifetime. A
		// Blueprint recompilation during ProcessEvent must not dereference old
		// field descriptors even while unwinding its argument frame.
		void Initialize(uint8* Data) const
		{
			if (Kind == EKind::String) new (Data + Offset) FString();
			if (Kind == EKind::Text) new (Data + Offset) FText();
			if (Kind == EKind::Math) ValueStruct->InitializeStruct(Data + Offset);
		}
		void Destroy(uint8* Data) const
		{
			if (Kind == EKind::String) reinterpret_cast<FString*>(Data + Offset)->~FString();
			if (Kind == EKind::Text) reinterpret_cast<FText*>(Data + Offset)->~FText();
		}

		bool Read(LhatMachine* M, FLhatBindings& B, LhatValue V, uint8* Data, TArray<TStrongObjectPtr<UObject>>& KeepAlive) const
		{
			void* Address = Data + Offset;
			switch (Kind)
			{
			case EKind::Bool:
				if (!lhat_is_bool(V)) return LhatUEBindings::Fail(M, "Expected bool");
				static_cast<uint8*>(Address)[BoolOffset] = (static_cast<uint8*>(Address)[BoolOffset] & ~BoolMask) | (lhat_as_bool(V) ? BoolSet : 0);
				return true;
#define LHAT_READ_CASE(K, T) case EKind::K: return LhatUEBindings::Read(M, B, V, *static_cast<T*>(Address));
			LHAT_READ_CASE(I8, int8) LHAT_READ_CASE(U8, uint8) LHAT_READ_CASE(I16, int16) LHAT_READ_CASE(U16, uint16)
			LHAT_READ_CASE(I32, int32) LHAT_READ_CASE(U32, uint32) LHAT_READ_CASE(I64, int64) LHAT_READ_CASE(U64, uint64)
			LHAT_READ_CASE(Float, float) LHAT_READ_CASE(Double, double) LHAT_READ_CASE(String, FString) LHAT_READ_CASE(Name, FName)
#undef LHAT_READ_CASE
			case EKind::Text:
			{
				FString Text;
				if (!LhatUEBindings::Read(M, B, V, Text)) return false;
				*static_cast<FText*>(Address) = FText::FromString(Text);
				return true;
			}
			case EKind::Object: case EKind::Class:
			{
				UObject* Object = lhat_is_nil(V) ? nullptr : B.ReadObject(M, V, ObjectClass);
				if (!Object && !lhat_is_nil(V)) return false;
				if (Kind == EKind::Class && Object && !CastChecked<UClass>(Object)->IsChildOf(MetaClass))
					return LhatUEBindings::Fail(M, "UClass does not satisfy the Blueprint class constraint");
				if (Object) KeepAlive.Emplace(Object);
				CastFieldChecked<FObjectProperty>(Property)->SetObjectPropertyValue(Address, Object);
				return true;
			}
			case EKind::Math:
				if (const void* Value = lhat_hostvalue_data(V, ValueTag)) { FMemory::Memcpy(Address, Value, ValueSize); return true; }
				return LhatUEBindings::Fail(M, "Expected matching UE math value");
			}
			return false;
		}

		bool Write(LhatMachine* M, FLhatBindings& B, const uint8* Data, LhatValue& V) const
		{
			const void* Address = Data + Offset;
			switch (Kind)
			{
			case EKind::Bool: V = lhat_bool((static_cast<const uint8*>(Address)[BoolOffset] & BoolMask) != 0); return true;
#define LHAT_WRITE_CASE(K, T) case EKind::K: return LhatUEBindings::Write(M, B, *static_cast<const T*>(Address), V);
			LHAT_WRITE_CASE(I8, int8) LHAT_WRITE_CASE(U8, uint8) LHAT_WRITE_CASE(I16, int16) LHAT_WRITE_CASE(U16, uint16)
			LHAT_WRITE_CASE(I32, int32) LHAT_WRITE_CASE(U32, uint32) LHAT_WRITE_CASE(I64, int64) LHAT_WRITE_CASE(U64, uint64)
			LHAT_WRITE_CASE(Float, float) LHAT_WRITE_CASE(Double, double) LHAT_WRITE_CASE(String, FString) LHAT_WRITE_CASE(Name, FName)
#undef LHAT_WRITE_CASE
			case EKind::Text: return LhatUEBindings::Write(M, B, static_cast<const FText*>(Address)->ToString(), V);
			case EKind::Object: case EKind::Class: return B.WrapObject(M, CastFieldChecked<FObjectProperty>(Property)->GetObjectPropertyValue(Address), V);
			case EKind::Math: return lhat_make_hostvalue(M, ValueTag, Address, &V);
			}
			return false;
		}
	};

	struct FPlan
	{
		TStrongObjectPtr<UFunction> Function;
		TArray<FSlot> Slots;
		TArray<int32> Inputs, Outputs;
		FString Signature;
		int32 Size = 0, Alignment = 16;
		bool Static = false;

		bool Build(UFunction* Fn, FImpl& State, FString& Reason)
		{
			Function.Reset(Fn);
			Size = FMath::Max(1, Fn->GetStructureSize());
			Alignment = FMath::Max(16, Fn->GetMinAlignment());
			Static = Fn->HasAnyFunctionFlags(FUNC_Static);
			if (Size > 65536 || Alignment > 256) { Reason = TEXT("parameter_frame_limit"); return false; }
			FProperty* Return = Fn->GetReturnProperty();
			for (TFieldIterator<FProperty> It(Fn, EFieldIterationFlags::None); It; ++It)
			{
				FProperty* P = *It;
				if (!P->HasAnyPropertyFlags(CPF_Parm)) continue;
				FSlot Slot;
				if (!Slot.Build(P, State, Reason)) { Reason = P->GetName() + TEXT(":") + Reason; return false; }
				if (Slot.Offset < 0 || Slot.Offset + P->GetSize() > Size) { Reason = TEXT("invalid_parameter_offset"); return false; }
				const int32 Index = Slots.Add(MoveTemp(Slot));
				if (P == Return) Outputs.Insert(Index, 0);
				else
				{
					const bool Out = P->HasAnyPropertyFlags(CPF_OutParm) && !P->HasAnyPropertyFlags(CPF_ConstParm);
					if (!Out || P->HasAnyPropertyFlags(CPF_ReferenceParm)) Inputs.Add(Index);
					if (Out) Outputs.Add(Index);
				}
			}
			if (Inputs.Num() > 64 || Outputs.Num() > LHAT_MAX_TUPLE) { Reason = TEXT("argument_or_result_limit"); return false; }
			// The core currently has one wide hostvalue result scratch area and
			// does not support wide values as positions of a multiple-result run.
			if (Outputs.Num() > 1)
				for (int32 Index : Outputs)
					if (Slots[Index].Kind == EKind::Math) { Reason = TEXT("hostvalue_in_multiple_results"); return false; }
			Signature = Fn->HasAnyFunctionFlags(FUNC_BlueprintPure) && Outputs.Num() <= 1 ? TEXT("f^") : TEXT("p^");
			if (!Static) Signature += TEXT("self^");
			for (int32 I = 0; I < Inputs.Num(); ++I)
			{
				if (I || !Static) Signature += TEXT(", ");
				Signature += Slots[Inputs[I]].Signature;
			}
			for (int32 I = 0; I < Outputs.Num(); ++I) Signature += (I ? TEXT(", ") : TEXT(" -> ")) + Slots[Outputs[I]].Signature;
			Signature += TEXT(";");
			return true;
		}
	};

	struct FCall
	{
		FImpl* State = nullptr;
		FType* Owner = nullptr;
		FName Name;
		FPlan Declaration;
		TMap<TWeakObjectPtr<UClass>, TUniquePtr<FPlan>> Overrides;
		FSlot Property;
		bool IsProperty = false, Setter = false;
	};

	FLhatBindings& Bindings;
	TMap<UClass*, TUniquePtr<FType>> Types;
	TArray<UClass*> Ordered;
	TArray<TUniquePtr<FCall>> Calls;
	TArray<TSharedPtr<FJsonValue>> Inventory;
	bool Invalidated = false;
	int32 ActiveCalls = 0;
	int32 StaticCount = 0, DynamicCount = 0, SkippedCount = 0;
#if WITH_EDITOR
	FDelegateHandle ReplacedHandle, ReloadHandle;
	TArray<TPair<TWeakObjectPtr<UBlueprint>, FDelegateHandle>> BlueprintHandles;
#endif

	explicit FImpl(FLhatBindings& B) : Bindings(B)
	{
#if WITH_EDITOR
		ReplacedHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddLambda([this](const FCoreUObjectDelegates::FReplacementObjectMap& Map)
		{
			for (const auto& Pair : Map)
				if (Pair.Key && (Pair.Key->IsA<UClass>() || Pair.Key->IsA<UFunction>())) { Invalidated = true; break; }
		});
		ReloadHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddLambda([this](EReloadCompleteReason) { Invalidated = true; });
#endif
	}
	~FImpl()
	{
#if WITH_EDITOR
		FCoreUObjectDelegates::OnObjectsReplaced.Remove(ReplacedHandle);
		FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadHandle);
		for (const auto& Pair : BlueprintHandles) if (UBlueprint* BP = Pair.Key.Get()) BP->OnCompiled().Remove(Pair.Value);
#endif
	}

	const FType* FindType(UClass* Class) const
	{
		// A non-public/unavailable signature type is never silently widened.
		const auto* Found = Types.Find(Class);
		return Found ? Found->Get() : nullptr;
	}

	void Record(const FString& Path, const FString& Member, const FString& Route, const FString& Reason = {})
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("path"), Path);
		Json->SetStringField(TEXT("member"), Member);
		Json->SetStringField(TEXT("route"), Route);
		if (!Reason.IsEmpty()) Json->SetStringField(TEXT("reason"), Reason);
		Inventory.Add(MakeShared<FJsonValueObject>(Json));
		if (Route == TEXT("static")) ++StaticCount;
		else if (Route == TEXT("dynamic")) ++DynamicCount;
		else ++SkippedCount;
	}

	static void Invoke(LhatMachine* M, void* Context, const LhatValue* Args, size_t Count, LhatValue* Answers, int* AnswerCount)
	{
		check(IsInGameThread());
		auto& Call = *static_cast<FCall*>(Context);
		auto& State = *Call.State;
		if (State.Invalidated) { LhatUEBindings::Fail(M, "UE reflection changed; recreate the Lhat program after Blueprint compilation/reload"); return; }
		if (State.ActiveCalls >= 64 || IsGarbageCollecting()) { LhatUEBindings::Fail(M, "UE dynamic call is not allowed during GC or excessive reentry"); return; }
		TGuardValue<int32> Depth(State.ActiveCalls, State.ActiveCalls + 1);
		if (Call.IsProperty)
		{
			if (Count != size_t(Call.Setter ? 2 : 1)) { LhatUEBindings::Fail(M, "Incorrect UE property argument count"); return; }
			UObject* Object = State.Bindings.ReadObject(M, Args[0], Call.Owner->Class.Get());
			if (!Object) return;
			TArray<TStrongObjectPtr<UObject>> KeepAlive;
			KeepAlive.Emplace(Object);
			if (Call.Setter) Call.Property.Read(M, State.Bindings, Args[1], reinterpret_cast<uint8*>(Object), KeepAlive);
			else if (Call.Property.Write(M, State.Bindings, reinterpret_cast<uint8*>(Object), Answers[0])) *AnswerCount = 1;
			return;
		}
		FPlan* Plan = &Call.Declaration;
		if (Count != size_t(Plan->Inputs.Num() + !Plan->Static)) { LhatUEBindings::Fail(M, "Incorrect UE argument count"); return; }
		UObject* Object = Plan->Static ? Call.Owner->Class->GetDefaultObject() : State.Bindings.ReadObject(M, Args[0], Call.Owner->Class.Get());
		if (!IsValid(Object)) { if (Plan->Static) LhatUEBindings::Fail(M, "Invalid UE static receiver"); return; }
		if (!Plan->Static)
		{
			// Resolve Blueprint overrides once per concrete receiver class.
			UClass* Concrete = Object->GetClass();
			const TWeakObjectPtr<UClass> ConcreteKey(Concrete);
			auto* Cached = Call.Overrides.Find(ConcreteKey);
			if (!Cached)
			{
				UFunction* Actual = Object->FindFunction(Call.Name);
				FString Reason;
				auto Override = MakeUnique<FPlan>();
				if (!Actual || !Override->Build(Actual, State, Reason) || Override->Signature != Plan->Signature)
				{ LhatUEBindings::Fail(M, "UE override signature changed; recreate the Lhat program"); return; }
				Cached = &Call.Overrides.Add(ConcreteKey, MoveTemp(Override));
			}
			Plan = Cached->Get();
		}
		if (auto* Actor = Cast<AActor>(Object); Actor && !Actor->HasAnyFlags(RF_ClassDefaultObject))
		{
			bool AllowEditor = GAllowActorScriptExecutionInEditor;
#if WITH_EDITOR
			AllowEditor |= Plan->Function->GetBoolMetaData(TEXT("CallInEditor"));
#endif
			if (!Actor->GetWorld() || (!Actor->GetWorld()->AreActorsInitialized() && !AllowEditor))
			{ LhatUEBindings::Fail(M, "UE Actor world is not initialized for reflected execution"); return; }
		}
		// Per-invocation, aligned storage: recursive calls never share a frame.
		uint8* Data = Align(static_cast<uint8*>(FMemory_Alloca(Plan->Size + Plan->Alignment)), Plan->Alignment);
		FMemory::Memzero(Data, Plan->Size);
		for (const auto& Slot : Plan->Slots) Slot.Initialize(Data);
		struct FFrame
		{
			FPlan& Plan; uint8* Data;
			~FFrame() { for (const auto& Slot : Plan.Slots) Slot.Destroy(Data); }
		} Frame{*Plan, Data};
		TArray<TStrongObjectPtr<UObject>> KeepAlive;
		KeepAlive.Emplace(Object);
		for (int32 I = 0; I < Plan->Inputs.Num(); ++I)
			if (!Plan->Slots[Plan->Inputs[I]].Read(M, State.Bindings, Args[I + !Plan->Static], Data, KeepAlive)) return;
		Object->ProcessEvent(Plan->Function.Get(), Data);
		if (State.Invalidated) { LhatUEBindings::Fail(M, "UE reflection changed during the call; recreate the Lhat program"); return; }
		for (int32 I = 0; I < Plan->Outputs.Num(); ++I)
			if (!Plan->Slots[Plan->Outputs[I]].Write(M, State.Bindings, Data, Answers[I])) return;
		*AnswerCount = Plan->Outputs.Num();
	}

	bool Gather(TArray<FLhatNativeType>& NativeTypes)
	{
		DiscoverProjectBlueprints();
		// Existing native providers own the canonical names. Keep all name buffers
		// alive until the program is destroyed, including for dynamic-only classes.
		Types.Add(UObject::StaticClass(), MakeUnique<FType>(UObject::StaticClass(), TEXT("ue"), TEXT("Object")));
		Types.Add(AActor::StaticClass(), MakeUnique<FType>(AActor::StaticClass(), TEXT("ue"), TEXT("Actor")));
		for (const auto& T : NativeTypes)
			if (T.Class) Types.FindOrAdd(T.Class) = MakeUnique<FType>(T.Class, UTF8_TO_TCHAR(T.Module), UTF8_TO_TCHAR(T.Name));
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!UsableClass(Class) || Types.Contains(Class)) continue;
			const FString Package = Class->GetOutermost()->GetName();
			FString Module;
			if (Package.StartsWith(TEXT("/Script/"))) Module = TEXT("ue.") + SafeName(FPackageName::GetShortName(Package));
			else
			{
				Module = TEXT("ue.BP");
				TArray<FString> Parts;
				// Keep the package leaf too: PIE map classes and multiple assets
				// with same-named generated classes must never share a host tag.
				Package.ParseIntoArray(Parts, TEXT("/"), true);
				for (const FString& Part : Parts) Module += TEXT(".") + SafeName(Part);
			}
			Types.Add(Class, MakeUnique<FType>(Class, Module, SafeName(Class->GetName())));
		}
		Types.GenerateKeyArray(Ordered);
		Ordered.Sort([](const UClass& A, const UClass& B) { return A.GetPathName() < B.GetPathName(); });
		TSet<UBlueprint*> Watched;
		TMap<FString, UClass*> NameOwners;
		for (UClass* Class : Ordered)
		{
			auto& Type = *Types[Class];
			if (UClass** Previous = NameOwners.Find(Type.Qualified()); Previous && *Previous != Class)
			{
				UE_LOG(LogLhat, Error, TEXT("Reflected type name collision: %s and %s => %s"), *(*Previous)->GetPathName(), *Class->GetPathName(), *Type.Qualified());
				return false;
			}
			NameOwners.Add(Type.Qualified(), Class);
			NativeTypes.Add({Class, Type.ModuleUtf8.GetData(), Type.NameUtf8.GetData()});
#if WITH_EDITOR
			if (auto* BP = Cast<UBlueprint>(Class->ClassGeneratedBy); BP && !Watched.Contains(BP))
			{
				Watched.Add(BP);
				BlueprintHandles.Emplace(BP, BP->OnCompiled().AddLambda([this](UBlueprint*) { Invalidated = true; }));
			}
#endif
		}
		// Discovery itself may have triggered load-time Blueprint reinstancing.
		Invalidated = false;
		return true;
	}

	bool Register(LhatProgram* Program)
	{
		// The C API's context lookup cannot distinguish an absent registration
		// from one with a null context. Snapshot names once, not per invocation.
		const size_t Length = lhat_program_dump_host_api(Program, nullptr, 0);
		if (!Length || Length >= MAX_int32) return false;
		TArray<char> JsonBytes;
		JsonBytes.SetNumUninitialized(int32(Length + 1));
		if (lhat_program_dump_host_api(Program, JsonBytes.GetData(), JsonBytes.Num()) != Length) return false;
		TSharedPtr<FJsonObject> Existing;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(UTF8_TO_TCHAR(JsonBytes.GetData()))), Existing)) return false;
		TSet<FString> Taken;
		for (const auto& Value : Existing->GetArrayField(TEXT("functions")))
		{
			const auto& Function = Value->AsObject();
			FString Type;
			if (Function->TryGetStringField(TEXT("type"), Type))
				Taken.Add(Function->GetStringField(TEXT("module")) + TEXT(".") + Type + TEXT(":") + Function->GetStringField(TEXT("name")));
		}
		for (UClass* Class : Ordered)
		{
			FType& Owner = *Types[Class];
			TArray<UFunction*> Functions;
			for (TFieldIterator<UFunction> It(Class, EFieldIterationFlags::None); It; ++It)
				if (It->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)) Functions.Add(*It);
			Functions.Sort([](const UFunction& A, const UFunction& B) { return A.GetName() < B.GetName(); });
			for (UFunction* Fn : Functions)
			{
				const FString Name = SafeName(Fn->GetName());
				const FString Key = Owner.Qualified() + TEXT(":") + Name;
				if (Taken.Contains(Key)) { Record(Fn->GetPathName(), Key, TEXT("static")); continue; }
				FString Reason = FunctionExclusion(Fn);
				auto Call = MakeUnique<FCall>();
				Call->State = this; Call->Owner = &Owner; Call->Name = Fn->GetFName();
				if (!Reason.IsEmpty() || !Call->Declaration.Build(Fn, *this, Reason)) { Record(Fn->GetPathName(), Key, TEXT("skipped"), Reason); continue; }
				if (!lhat_register_member(Program, Owner.ModuleUtf8.GetData(), Owner.NameUtf8.GetData(), TCHAR_TO_UTF8(*Name),
					TCHAR_TO_UTF8(*Call->Declaration.Signature), Invoke, Call.Get()))
				{ UE_LOG(LogLhat, Error, TEXT("Dynamic registration failed: %s [%s]"), *Key, *Call->Declaration.Signature); return false; }
				Taken.Add(Key); Calls.Add(MoveTemp(Call)); Record(Fn->GetPathName(), Key, TEXT("dynamic"));
			}
			TArray<FProperty*> Properties;
			for (TFieldIterator<FProperty> It(Class, EFieldIterationFlags::None); It; ++It)
				if (It->HasAnyPropertyFlags(CPF_BlueprintVisible)) Properties.Add(*It);
			Properties.Sort([](const FProperty& A, const FProperty& B) { return A.GetName() < B.GetName(); });
			for (FProperty* P : Properties)
			{
				for (bool Setter : {false, true})
				{
					if (Setter && P->HasAnyPropertyFlags(CPF_BlueprintReadOnly)) continue;
					const FString Name = (Setter ? TEXT("Set_") : TEXT("Get_")) + SafeName(P->GetName());
					const FString Key = Owner.Qualified() + TEXT(":") + Name;
					FString Reason;
					if (Taken.Contains(Key)) { Record(P->GetPathName(), Key, TEXT("skipped"), TEXT("accessor_name_collision")); continue; }
					bool PublicAccess = !P->HasAnyPropertyFlags(CPF_NativeAccessSpecifierPrivate | CPF_NativeAccessSpecifierProtected);
#if WITH_METADATA
					PublicAccess |= P->GetBoolMetaData(TEXT("AllowPrivateAccess"));
					if (P->GetBoolMetaData(TEXT("Private"))) PublicAccess = false;
#endif
					if (!PublicAccess) Reason = TEXT("nonpublic");
					if (P->HasSetterOrGetter()) Reason = TEXT("native_property_accessor");
					if (Setter && P->HasAnyPropertyFlags(CPF_Net)) Reason = TEXT("replicated_property_setter");
#if WITH_METADATA
					if (P->HasMetaData(TEXT("BlueprintGetter")) || P->HasMetaData(TEXT("BlueprintSetter"))) Reason = TEXT("custom_property_accessor");
#else
					// Blueprint-defined custom accessors/private metadata are stripped too.
					Reason = TEXT("property_metadata_unavailable");
#endif
					auto Call = MakeUnique<FCall>();
					Call->State = this; Call->Owner = &Owner; Call->IsProperty = true; Call->Setter = Setter;
					if (!Reason.IsEmpty() || !Call->Property.Build(P, *this, Reason)) { Record(P->GetPathName(), Key, TEXT("skipped"), Reason); continue; }
					const FString Signature = (Setter ? TEXT("p^self^, ") : TEXT("f^self^ -> ")) + Call->Property.Signature + TEXT(";");
					if (!lhat_register_member(Program, Owner.ModuleUtf8.GetData(), Owner.NameUtf8.GetData(), TCHAR_TO_UTF8(*Name),
						TCHAR_TO_UTF8(*Signature), Invoke, Call.Get())) return false;
					Taken.Add(Key); Calls.Add(MoveTemp(Call)); Record(P->GetPathName(), Key, TEXT("dynamic"));
				}
			}
		}
		UE_LOG(LogLhat, Verbose, TEXT("Blueprint API snapshot: %d types; %d static, %d dynamic, %d unsupported members"),
			Types.Num(), StaticCount, DynamicCount, SkippedCount);
		return true;
	}

	FString Report() const
	{
		auto Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("version"), 1);
		Root->SetNumberField(TEXT("types"), Types.Num());
		Root->SetNumberField(TEXT("static"), StaticCount);
		Root->SetNumberField(TEXT("dynamic"), DynamicCount);
		Root->SetNumberField(TEXT("skipped"), SkippedCount);
		Root->SetBoolField(TEXT("invalidated"), Invalidated);
		Root->SetArrayField(TEXT("members"), Inventory);
		FString Text;
		FJsonSerializer::Serialize(Root, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}
};

FLhatDynamicBindings::FLhatDynamicBindings(FLhatBindings& B) : Impl(MakeUnique<FImpl>(B)) {}
FLhatDynamicBindings::~FLhatDynamicBindings() = default;
bool FLhatDynamicBindings::Gather(TArray<FLhatNativeType>& Types) { return Impl->Gather(Types); }
bool FLhatDynamicBindings::Register(LhatProgram* P) { return Impl->Register(P); }
FString FLhatDynamicBindings::Report() const { return Impl->Report(); }

bool FLhatBindings::GatherDynamicTypes(TArray<FLhatNativeType>& Types)
{
	Dynamic = MakeUnique<FLhatDynamicBindings>(*this);
	return Dynamic->Gather(Types);
}
bool FLhatBindings::RegisterDynamicFunctions(LhatProgram* Program) { return Dynamic && Dynamic->Register(Program); }
FString FLhatBindings::GetDynamicBindingReport() const { return Dynamic ? Dynamic->Report() : TEXT("{}"); }
