// Force the static core's public C API into this DLL's import library. The UE
// header copies use LHAT_API, so consumers cannot link their own allocator/VM.
#include "LhatScript.h"
#include "LhatCoreExports.inl"
