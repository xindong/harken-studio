#include "Core/AudioEngine.h"
#include "Widgets/LogPanel/LogPanel.h"

#include <gtest/gtest.h>

#include <set>
#include <utility>
#include <vector>

TEST(LogCategoryHonesty, EnumNamesCoverRequiredCategories)
{
    std::set<QString> names;
    ASSERT_EQ(VNX::logCategoryCount(), 10);
    for (int i = 0; i < VNX::logCategoryCount(); ++i)
        names.insert(QString::fromUtf8(VNX::logCategoryName(VNX::logCategoryAt(i))));

    EXPECT_TRUE(names.count(QStringLiteral("Engine")));
    EXPECT_TRUE(names.count(QStringLiteral("Audio")));
    EXPECT_TRUE(names.count(QStringLiteral("GUI")));
    EXPECT_TRUE(names.count(QStringLiteral("File")));
    EXPECT_TRUE(names.count(QStringLiteral("Layout")));
    EXPECT_TRUE(names.count(QStringLiteral("Import")));
    EXPECT_TRUE(names.count(QStringLiteral("Pack")));
    EXPECT_TRUE(names.count(QStringLiteral("MCP")));
    EXPECT_TRUE(names.count(QStringLiteral("Network")));
    EXPECT_TRUE(names.count(QStringLiteral("RuntimeSync")));
}

TEST(LogCategoryHonesty, EngineLogMessageMapsInfoAndWarnIntoPanel)
{
    EXPECT_EQ(VNX::LogPanel::engineLevelToPanel(0), VNX::LogEntry::Info);
    EXPECT_EQ(VNX::LogPanel::engineLevelToPanel(1), VNX::LogEntry::Warning);
    EXPECT_EQ(VNX::LogPanel::engineLevelToPanel(2), VNX::LogEntry::Error);

    VNX::LogModel model;
    VNX::AudioEngine engine;
    QObject::connect(&engine, &VNX::AudioEngine::logMessage, &model,
                     [&model](int level, const QString& message) {
                         model.addLog(VNX::LogPanel::engineLevelToPanel(level),
                                      VNX::LogCategory::Engine,
                                      message);
                     });

    engine.emitLogMessageForTest(0, QStringLiteral("info-line"));
    engine.emitLogMessageForTest(1, QStringLiteral("warn-line"));

    const auto entries = model.entries();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].category, QStringLiteral("Engine"));
    EXPECT_EQ(entries[0].level, VNX::LogEntry::Info);
    EXPECT_EQ(entries[0].message, QStringLiteral("info-line"));
    EXPECT_EQ(entries[1].level, VNX::LogEntry::Warning);
    EXPECT_EQ(entries[1].message, QStringLiteral("warn-line"));
}

TEST(LogCategoryHonesty, AudioEngineLogSignalCarriesInfoAndWarn)
{
    VNX::AudioEngine engine;
    std::vector<std::pair<int, QString>> seen;
    QObject::connect(&engine, &VNX::AudioEngine::logMessage, &engine,
                     [&seen](int level, const QString& message) {
                         seen.emplace_back(level, message);
                     });
    engine.emitLogMessageForTest(0, QStringLiteral("info-signal"));
    engine.emitLogMessageForTest(1, QStringLiteral("warn-signal"));
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].first, 0);
    EXPECT_EQ(seen[0].second, QStringLiteral("info-signal"));
    EXPECT_EQ(seen[1].first, 1);
}
