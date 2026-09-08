#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogLhat, Log, All);

class FLhatModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	// Machines and process-wide L^ type tags retain native callback addresses.
	virtual bool SupportsDynamicReloading() override { return false; }
};
