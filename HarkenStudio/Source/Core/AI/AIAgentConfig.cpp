#include "AIAgentConfig.h"

#include "../ISettingsStorage.h"

#include <utility>

namespace VNX {
namespace {
constexpr int kDefaultMaxTokens = 1024;
constexpr int kDefaultContextWindowTokens = 128000;
constexpr int kDefaultTimeoutMs = 30000;
constexpr int kMinMaxTokens = 16;
constexpr int kMaxMaxTokens = 32768;
constexpr int kMinContextWindowTokens = 4096;
constexpr int kMaxContextWindowTokens = 2000000;
constexpr int kMinimumInputTokens = 1024;
constexpr int kMinTimeoutMs = 1000;
constexpr int kMaxTimeoutMs = 300000;

// Test-only backing storage shared by every store constructed while set, so
// stores created internally (e.g. inside the settings pages) also stay off the
// real registry during tests.
std::shared_ptr<ISettingsStorage>& storageOverride()
{
    static std::shared_ptr<ISettingsStorage> instance;
    return instance;
}

QVariant readValue(const AIAgentConfigStore::ValueLoader& loader,
                   ISettingsStorage& storage, const QString& key,
                   const QVariant& fallback)
{
    if (loader)
    {
        const QVariant value = loader(key, fallback);
        return value.isValid() ? value : fallback;
    }
    return storage.value(key, fallback);
}
}

AIAgentConfigStore::AIAgentConfigStore()
    : storage_(storageOverride()
                   ? storageOverride()
                   : std::shared_ptr<ISettingsStorage>(SettingsStorageFactory::create(
                         SettingsStorageFactory::StorageType::QSettings,
                         QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio"))))
{
}

AIAgentConfig AIAgentConfig::defaults()
{
    return {};
}

AIAgentConfig AIAgentConfig::effectiveSnapshot() const
{
    AIAgentConfig result = *this;
    result.baseUrl = result.baseUrl.trimmed();
    result.model = result.model.trimmed();
    result.permissionProfile = parsePermissionMode(toId(result.permissionProfile));
    result.toolProfile = result.toolProfile.trimmed();
    if (result.contextWindowTokens < kMinContextWindowTokens
        || result.contextWindowTokens > kMaxContextWindowTokens)
    {
        result.contextWindowTokens = kDefaultContextWindowTokens;
    }
    if (result.maxTokens < kMinMaxTokens || result.maxTokens > kMaxMaxTokens)
        result.maxTokens = kDefaultMaxTokens;
    if (result.maxTokens > result.contextWindowTokens - kMinimumInputTokens)
        result.maxTokens = qMin(kDefaultMaxTokens,
                                result.contextWindowTokens - kMinimumInputTokens);
    if (result.requestTimeoutMs < kMinTimeoutMs || result.requestTimeoutMs > kMaxTimeoutMs)
        result.requestTimeoutMs = kDefaultTimeoutMs;
    return result;
}

bool AIAgentConfig::operator==(const AIAgentConfig& other) const
{
    return baseUrl == other.baseUrl && model == other.model
        && permissionProfile == other.permissionProfile
        && toolProfile == other.toolProfile
        && contextWindowTokens == other.contextWindowTokens
        && maxTokens == other.maxTokens
        && requestTimeoutMs == other.requestTimeoutMs;
}

AIAgentConfig AIAgentConfigStore::load() const
{
    storage_->beginGroup(QStringLiteral("ai"));
    AIAgentConfig config;
    const AIAgentConfig defaults = AIAgentConfig::defaults();
    config.baseUrl = readValue(valueLoader_, *storage_, QStringLiteral("base_url"), defaults.baseUrl).toString();
    config.model = readValue(valueLoader_, *storage_, QStringLiteral("model"), defaults.model).toString();
    config.permissionProfile = parsePermissionMode(readValue(valueLoader_, *storage_, QStringLiteral("permission_mode"),
                                         toId(defaults.permissionProfile)).toString());
    config.toolProfile = readValue(valueLoader_, *storage_, QStringLiteral("tool_profile"),
                                   defaults.toolProfile).toString();
    config.contextWindowTokens =
        readValue(valueLoader_, *storage_, QStringLiteral("context_window_tokens"),
                  defaults.contextWindowTokens).toInt();
    config.maxTokens = readValue(valueLoader_, *storage_, QStringLiteral("max_tokens"),
                                 defaults.maxTokens).toInt();
    config.requestTimeoutMs = readValue(valueLoader_, *storage_, QStringLiteral("request_timeout_ms"),
                                        defaults.requestTimeoutMs).toInt();
    storage_->endGroup();
    return config;
}

AIAgentConfig AIAgentConfigStore::effectiveSnapshot() const
{
    return load().effectiveSnapshot();
}

bool AIAgentConfigStore::save(const AIAgentConfig& config) const
{
    // Compute the no-op check before opening the group so the nested load()
    // inside effectiveSnapshot() does not re-enter the group on shared storage.
    const AIAgentConfig next = config.effectiveSnapshot();
    const AIAgentConfig current = effectiveSnapshot();
    if (next == current)
        return false;

    storage_->beginGroup(QStringLiteral("ai"));
    storage_->setValue(QStringLiteral("base_url"), next.baseUrl);
    storage_->setValue(QStringLiteral("model"), next.model);
    storage_->setValue(QStringLiteral("permission_mode"), toId(next.permissionProfile));
    storage_->setValue(QStringLiteral("tool_profile"), next.toolProfile);
    storage_->setValue(QStringLiteral("context_window_tokens"),
                       next.contextWindowTokens);
    storage_->setValue(QStringLiteral("max_tokens"), next.maxTokens);
    storage_->setValue(QStringLiteral("request_timeout_ms"), next.requestTimeoutMs);
    storage_->endGroup();
    storage_->sync();
    return true;
}

void AIAgentConfigStore::setValueLoaderForTesting(ValueLoader loader)
{
    valueLoader_ = std::move(loader);
}

void AIAgentConfigStore::setStorageOverrideForTesting(std::shared_ptr<ISettingsStorage> storage)
{
    storageOverride() = std::move(storage);
}

}
