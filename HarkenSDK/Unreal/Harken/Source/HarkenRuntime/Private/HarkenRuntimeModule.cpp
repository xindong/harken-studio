#include "HarkenRuntime.h"

#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "HarkenCoreRuntime.h"

DEFINE_LOG_CATEGORY(LogHarken);

FHarkenRuntimeModule& FHarkenRuntimeModule::Get()
{
    return FModuleManager::LoadModuleChecked<FHarkenRuntimeModule>(TEXT("HarkenRuntime"));
}

bool FHarkenRuntimeModule::IsAvailable()
{
    return FModuleManager::Get().IsModuleLoaded(TEXT("HarkenRuntime"));
}

void FHarkenRuntimeModule::StartupModule()
{
#if PLATFORM_ANDROID || PLATFORM_IOS
    bCoreLoaded = true;
#else
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Harken"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogHarken, Error, TEXT("The Harken plugin directory could not be resolved."));
        return;
    }

    FString LibraryPath;
#if PLATFORM_WINDOWS
    LibraryPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries/ThirdParty/HarkenCore/Win64/HarkenCore.dll"));
#elif PLATFORM_MAC
    LibraryPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries/ThirdParty/HarkenCore/Mac/libHarkenCore.dylib"));
#elif PLATFORM_LINUX
    LibraryPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries/ThirdParty/HarkenCore/Linux/x86_64/libHarkenCore.so"));
#endif

    CoreLibraryHandle = LibraryPath.IsEmpty() ? nullptr : FPlatformProcess::GetDllHandle(*LibraryPath);
    bCoreLoaded = CoreLibraryHandle != nullptr;
    if (!bCoreLoaded)
    {
        UE_LOG(LogHarken, Error, TEXT("HarkenCore could not be loaded from %s."), *LibraryPath);
    }
#endif

    FHarkenCoreRuntime::Get().SetLibraryReady(bCoreLoaded);
}

void FHarkenRuntimeModule::ShutdownModule()
{
    FHarkenCoreRuntime::Get().Shutdown();
    FHarkenCoreRuntime::Get().SetLibraryReady(false);

    if (CoreLibraryHandle != nullptr)
    {
        FPlatformProcess::FreeDllHandle(CoreLibraryHandle);
        CoreLibraryHandle = nullptr;
    }
    bCoreLoaded = false;
}

bool FHarkenRuntimeModule::IsCoreLoaded() const
{
    return bCoreLoaded;
}

IMPLEMENT_MODULE(FHarkenRuntimeModule, HarkenRuntime)
