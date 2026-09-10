#include "LhatScript.h"

bool FLhatProgram::GetHostApiJson(TArray<uint8>& OutUtf8, FString& Error) const
{
	check(IsInGameThread());
	OutUtf8.Reset();
	Error.Empty();
	if (!IsValid())
	{
		Error = GetDiagnostics();
		return false;
	}
	const size_t Length = lhat_program_dump_host_api(Program, nullptr, 0);
	if (Length == 0 || Length >= static_cast<size_t>(MAX_int32))
	{
		Error = TEXT("The host API JSON has an invalid size.");
		return false;
	}
	OutUtf8.SetNumUninitialized(static_cast<int32>(Length + 1));
	if (lhat_program_dump_host_api(Program, reinterpret_cast<char*>(OutUtf8.GetData()), OutUtf8.Num()) != Length)
	{
		OutUtf8.Reset();
		Error = TEXT("The host API changed while it was being exported.");
		return false;
	}
	OutUtf8.SetNum(static_cast<int32>(Length), EAllowShrinking::No);
	return true;
}
