#include "LhatModule.h"

#include "HAL/UnrealMemory.h"

THIRD_PARTY_INCLUDES_START
#include "lhat.h"
THIRD_PARTY_INCLUDES_END

namespace
{
	void* LhatAllocate(void*, size_t Size)
	{
		return FMemory::Malloc(Size);
	}

	void* LhatCallocate(void*, size_t Count, size_t Size)
	{
		if (Size != 0 && Count > (MAX_uint64 / Size))
		{
			return nullptr;
		}

		return FMemory::MallocZeroed(Count * Size);
	}

	void* LhatReallocate(void*, void* Pointer, size_t Size)
	{
		return FMemory::Realloc(Pointer, Size);
	}

	void LhatFree(void*, void* Pointer)
	{
		FMemory::Free(Pointer);
	}
}

DEFINE_LOG_CATEGORY(LogLhat);

void FLhatModule::StartupModule()
{
	const LhatAllocator Allocator =
	{
		&LhatAllocate,
		&LhatCallocate,
		&LhatReallocate,
		&LhatFree,
		nullptr
	};

	if (!lhat_set_allocator(&Allocator))
	{
		UE_LOG(LogLhat, Fatal, TEXT("L^ allocated memory before the Unreal allocator was installed."));
	}

	UE_LOG(LogLhat, Log, TEXT("L^ %s runtime initialized."), UTF8_TO_TCHAR(LHAT_VERSION));
}

void FLhatModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FLhatModule, Lhat)
