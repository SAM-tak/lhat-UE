#pragma once

#include "LhatBindings.h"
#include <limits>
#include <type_traits>

/** Strings and callback addresses must remain valid for the module's lifetime. */
struct FLhatNativeType
{
	UClass* Class;
	const char* Module;
	const char* Name;
};

struct FLhatNativeBindingProvider
{
	const TCHAR* Name;
	void (*GatherTypes)(TArray<FLhatNativeType>&);
	bool (*RegisterFunctions)(LhatProgram*, FLhatBindings&);
};

namespace LhatUEBindings
{
	LHAT_API bool LoadGeneratedProviders(FString& Error);
	LHAT_API void AddProvider(const FLhatNativeBindingProvider* Provider);
	LHAT_API void RemoveProvider(const FLhatNativeBindingProvider* Provider);
	LHAT_API bool RegisterProviders(LhatProgram* Program, FLhatBindings& Bindings);
	LHAT_API bool RegisterMathTypes(LhatProgram* Program, TMap<FString, const LhatHostValueTag*>& Tags);

	inline bool Fail(LhatMachine* Machine, const char* Message)
	{
		lhat_machine_panic_text(Machine, Message);
		return false;
	}

	// Codecs are selected at generation time. There is no reflected call dispatch.
	template<typename T> bool Read(LhatMachine* M, FLhatBindings& B, LhatValue V, T& Out)
	{
		if constexpr (std::is_same_v<T, bool>)
		{
			if (!lhat_is_bool(V)) return Fail(M, "Expected bool");
			Out = lhat_as_bool(V);
		}
		else if constexpr (std::is_integral_v<T>)
		{
			int64 N;
			if (lhat_is_integer(V)) N = lhat_as_integer(V);
			else if (lhat_is_real(V))
			{
				const double Real = lhat_as_real(V);
				if (!FMath::IsFinite(Real) || Real < -9223372036854775808.0 || Real >= 9223372036854775808.0 || FMath::TruncToDouble(Real) != Real)
					return Fail(M, "Expected an in-range integral number");
				N = static_cast<int64>(Real);
			}
			else return Fail(M, "Expected integer");
			if constexpr (std::is_unsigned_v<T>)
			{
				if (N < 0 || static_cast<uint64>(N) > std::numeric_limits<T>::max()) return Fail(M, "Integer out of range");
			}
			else if (N < std::numeric_limits<T>::lowest() || N > std::numeric_limits<T>::max()) return Fail(M, "Integer out of range");
			Out = static_cast<T>(N);
		}
		else if constexpr (std::is_floating_point_v<T>)
		{
			if (!lhat_is_number(V)) return Fail(M, "Expected number");
			const double N = lhat_number_as_real(V);
			// Preserve IEEE NaN/Infinity for math APIs; only finite narrowing can overflow.
			if (FMath::IsFinite(N) && (N < -std::numeric_limits<T>::max() || N > std::numeric_limits<T>::max())) return Fail(M, "Number out of range");
			Out = static_cast<T>(N);
		}
		else if constexpr (std::is_same_v<T, FString> || std::is_same_v<T, FName>)
		{
			if (!lhat_is_object_kind(V, LHAT_OBJECT_STRING)) return Fail(M, "Expected string");
			const auto* S = reinterpret_cast<const LhatString*>(lhat_as_object(V));
			if (S->length > MAX_int32) return Fail(M, "String too long");
			const FUTF8ToTCHAR Text(S->text, static_cast<int32>(S->length));
			const FString String(Text.Length(), Text.Get());
			if constexpr (std::is_same_v<T, FName>)
			{
				int32 NullIndex;
				// FString::FindChar searches its terminator too. Reject only embedded NULs.
				if (String.Len() >= NAME_SIZE || (String.FindChar(TCHAR(0), NullIndex) && NullIndex < String.Len())) return Fail(M, "Invalid FName");
				Out = FName(*String);
			}
			else Out = String;
		}
		else if constexpr (std::is_pointer_v<T> && std::is_base_of_v<UObject, std::remove_pointer_t<T>>)
		{
			if (lhat_is_nil(V)) { Out = nullptr; return true; }
			Out = static_cast<T>(B.ReadObject(M, V, std::remove_pointer_t<T>::StaticClass()));
			if (!Out) return false;
		}
		else static_assert(sizeof(T) == 0, "Missing native input codec");
		return true;
	}

	template<typename T> bool Write(LhatMachine* M, FLhatBindings& B, const T& V, LhatValue& Out)
	{
		if constexpr (std::is_same_v<T, bool>) { Out = lhat_bool(V); return true; }
		else if constexpr (std::is_integral_v<T>)
		{
			if constexpr (std::is_unsigned_v<T>) if (static_cast<uint64>(V) > MAX_int64) return Fail(M, "Unsigned result exceeds Lhat integer range");
			Out = lhat_integer(static_cast<int64>(V));
			return true;
		}
		else if constexpr (std::is_floating_point_v<T>) { Out = lhat_real(V); return true; }
		else if constexpr (std::is_same_v<T, FString> || std::is_same_v<T, FName>)
		{
			FString String;
			if constexpr (std::is_same_v<T, FName>) String = V.ToString();
			else String = V;
			const FTCHARToUTF8 Text(*String, String.Len());
			return lhat_machine_make_string(M, Text.Get(), Text.Length(), &Out);
		}
		else if constexpr (std::is_pointer_v<T> && std::is_base_of_v<UObject, std::remove_pointer_t<T>>)
		{
			// Blueprint object pins erase pointee constness; UObject instances remain GC-managed, mutable objects.
			return B.WrapObject(M, const_cast<UObject*>(static_cast<const UObject*>(V)), Out);
		}
		else static_assert(sizeof(T) == 0, "Missing native output codec");
	}

	template<typename T> bool ReadValue(LhatMachine* M, FLhatBindings& B, LhatValue V, T& Out, const char* Name)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		const void* Data = lhat_hostvalue_data(V, B.GetValueTag(Name));
		if (!Data) return Fail(M, "Expected UE math value");
		FMemory::Memcpy(&Out, Data, sizeof(T));
		return true;
	}
	template<typename T> bool WriteValue(LhatMachine* M, FLhatBindings& B, const T& V, LhatValue& Out, const char* Name)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		return lhat_make_hostvalue(M, B.GetValueTag(Name), &V, &Out);
	}
}
