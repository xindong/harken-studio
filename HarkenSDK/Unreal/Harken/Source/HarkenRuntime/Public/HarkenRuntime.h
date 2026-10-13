#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogHarken, Log, All);

class HARKENRUNTIME_API FHarkenRuntimeModule final : public IModuleInterface
{
public:
    static FHarkenRuntimeModule& Get();
    static bool IsAvailable();

    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    bool IsCoreLoaded() const;

private:
    void* CoreLibraryHandle = nullptr;
    bool bCoreLoaded = false;
};
