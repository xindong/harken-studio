#include "AI/AIAgentConfig.h"
#include "MemorySettingsStorage.h"

#include <gtest/gtest.h>

#include <memory>

using namespace VNX;

TEST(AIAgentConfigTest, DefaultsAreAuthoritative)
{
    const AIAgentConfig config = AIAgentConfig::defaults().effectiveSnapshot();
    EXPECT_TRUE(config.baseUrl.isEmpty());
    EXPECT_TRUE(config.model.isEmpty());
    EXPECT_EQ(config.permissionProfile, QStringLiteral("ask"));
    EXPECT_EQ(config.contextWindowTokens, 128000);
    EXPECT_EQ(config.maxTokens, 1024);
    EXPECT_EQ(config.requestTimeoutMs, 30000);
}

TEST(AIAgentConfigTest, InvalidValuesUseSupportedFallbacksAndNormalize)
{
    AIAgentConfig config;
    config.baseUrl = QStringLiteral(" https://example.test/v1 ");
    config.model = QStringLiteral(" model ");
    config.permissionProfile = QStringLiteral(" BYPASS ");
    config.contextWindowTokens = 100;
    config.maxTokens = 1;
    config.requestTimeoutMs = 999999;

    const AIAgentConfig effective = config.effectiveSnapshot();
    EXPECT_EQ(effective.baseUrl, QStringLiteral("https://example.test/v1"));
    EXPECT_EQ(effective.model, QStringLiteral("model"));
    EXPECT_EQ(effective.permissionProfile, QStringLiteral("ask"));
    EXPECT_EQ(effective.contextWindowTokens, 128000);
    EXPECT_EQ(effective.maxTokens, 1024);
    EXPECT_EQ(effective.requestTimeoutMs, 30000);
}

TEST(AIAgentConfigTest, StoreLoadsAndDetectsEqualityWithoutSecrets)
{
    AIAgentConfigStore store;
    store.setValueLoaderForTesting([](const QString& key, const QVariant& fallback) {
        if (key == QLatin1String("base_url")) return QVariant(QStringLiteral("https://example.test"));
        if (key == QLatin1String("model")) return QVariant(QStringLiteral("test-model"));
        if (key == QLatin1String("permission_mode")) return QVariant(QStringLiteral("read_only"));
        if (key == QLatin1String("context_window_tokens")) return QVariant(8192);
        if (key == QLatin1String("max_tokens")) return QVariant(777);
        if (key == QLatin1String("request_timeout_ms")) return QVariant(5000);
        return fallback;
    });
    const AIAgentConfig first = store.effectiveSnapshot();
    const AIAgentConfig second = store.effectiveSnapshot();
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.contextWindowTokens, 8192);
    EXPECT_EQ(first.maxTokens, 777);
    EXPECT_EQ(first.requestTimeoutMs, 5000);
    EXPECT_FALSE(first.operator==(AIAgentConfig::defaults()));
}

TEST(AIAgentConfigTest, OutputBudgetCannotConsumeConfiguredContextWindow)
{
    AIAgentConfig config;
    config.contextWindowTokens = 4096;
    config.maxTokens = 32768;

    const AIAgentConfig effective = config.effectiveSnapshot();
    EXPECT_EQ(effective.contextWindowTokens, 4096);
    EXPECT_EQ(effective.maxTokens, 1024);
}

TEST(AIAgentConfigTest, SavePersistsOrdinaryKeysOnly)
{
    auto storage = std::make_shared<MemorySettingsStorage>();
    AIAgentConfigStore::setStorageOverrideForTesting(storage);

    AIAgentConfig config;
    config.baseUrl = QStringLiteral("https://example.test/v1");
    config.model = QStringLiteral("test-model");
    config.permissionProfile = QStringLiteral("ask");
    config.contextWindowTokens = 65536;
    AIAgentConfigStore store;
    EXPECT_TRUE(store.save(config));
    EXPECT_FALSE(store.save(config));

    const AIAgentConfig loaded = AIAgentConfigStore().load();
    EXPECT_EQ(loaded.baseUrl, config.baseUrl);
    EXPECT_EQ(loaded.model, config.model);
    EXPECT_EQ(loaded.contextWindowTokens, config.contextWindowTokens);
    storage->beginGroup(QStringLiteral("ai"));
    EXPECT_FALSE(storage->contains(QStringLiteral("api_key")));
    storage->endGroup();

    AIAgentConfigStore::setStorageOverrideForTesting(nullptr);
}

TEST(AIAgentConfigTest, UnrecognizedStoredSettingsDoNotChangeEffectiveConfig)
{
    auto storage = std::make_shared<MemorySettingsStorage>();
    storage->beginGroup(QStringLiteral("ai"));
    storage->setValue(QStringLiteral("tool_profile"), QStringLiteral("  future_profile  "));
    storage->endGroup();
    AIAgentConfigStore::setStorageOverrideForTesting(storage);

    const AIAgentConfig effective = AIAgentConfigStore().effectiveSnapshot();
    EXPECT_EQ(effective, AIAgentConfig::defaults().effectiveSnapshot());

    AIAgentConfigStore::setStorageOverrideForTesting(nullptr);
}
