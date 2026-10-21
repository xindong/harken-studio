#include "UI/ProjectsPage.h"
#include "Projects/ProjectStore.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSet>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>

#include <memory>

TEST(ProjectsPageTest, RefreshReturnsBeforeMetadataReadsAndUpdatesRecommendations)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    qputenv("HARKEN_HUB_USER_ROOT", dir.filePath("user").toUtf8());
    qputenv("HARKEN_HUB_MACHINE_ROOT", dir.filePath("machine").toUtf8());
    const auto environment = qScopeGuard([]() {
        qunsetenv("HARKEN_HUB_USER_ROOT");
        qunsetenv("HARKEN_HUB_MACHINE_ROOT");
    });
    VNH::ProjectStore store;
    ASSERT_TRUE(store.open(dir.filePath("projects.db")));
    const auto path = dir.filePath("Demo.hsp");
    const auto writeVersion = [&](const QString& version) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        return file.write(QJsonDocument(QJsonObject{{"harkenDocument", QJsonObject{
            {"type", "project.root"}, {"savedStudioVersion", version}}}}).toJson()) > 0;
    };
    ASSERT_TRUE(writeVersion("1.0"));
    VNH::ProjectRecord record;
    record.canonicalPath = path;
    record.displayName = "Demo";
    ASSERT_TRUE(store.addProject(record));
    VNH::InstalledVersion first;
    first.buildId = "first-build";
    first.semanticVersion = "1.0";
    auto second = first;
    second.buildId = "second-build";
    second.semanticVersion = "2.0.0";

    auto* pool = QThreadPool::globalInstance();
    ASSERT_TRUE(pool->waitForDone(5000));
    const int previousLimit = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore entered;
    QSemaphore release;
    pool->start([&]() { entered.release(); release.acquire(); });
    bool released = false;
    const auto unblock = qScopeGuard([&]() {
        if (!released)
            release.release();
        pool->waitForDone(5000);
        pool->setMaxThreadCount(previousLimit);
    });
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    VNH::ProjectsPage page(&store);
    page.setVersionsProvider([=]() { return std::vector<VNH::InstalledVersion>{first, second}; });
    page.refresh();
    auto* table = page.findChild<QTableWidget*>();
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->rowCount(), 1);
    EXPECT_TRUE(table->item(0, 2)->data(Qt::UserRole + 2).toString().isEmpty());
    ASSERT_TRUE(writeVersion(second.semanticVersion));
    release.release();
    released = true;

    const auto waitForBuild = [&](const QString& build) {
        QElapsedTimer elapsed;
        elapsed.start();
        while (table->item(0, 2)->data(Qt::UserRole + 2).toString() != build
               && elapsed.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
        return table->item(0, 2)->data(Qt::UserRole + 2).toString() == build;
    };
    ASSERT_TRUE(waitForBuild(second.buildId));
    ASSERT_TRUE(pool->waitForDone(5000));
    ASSERT_TRUE(writeVersion(first.semanticVersion));
    page.refresh();
    EXPECT_TRUE(waitForBuild(first.buildId));
}

class ProjectsPageSelectionTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir.filePath("user").toUtf8());
        qputenv("HARKEN_HUB_MACHINE_ROOT", dir.filePath("machine").toUtf8());
        ASSERT_TRUE(store.open(dir.filePath("projects.db")));
        for (const auto* name : {"Alpha", "Bravo", "Charlie"}) {
            VNH::ProjectRecord record;
            record.canonicalPath = dir.filePath(QString::fromLatin1(name) + ".hsp");
            record.displayName = QString::fromLatin1(name);
            QFile file(record.canonicalPath);
            ASSERT_TRUE(file.open(QIODevice::WriteOnly));
            ASSERT_GT(file.write("{}"), 0);
            ASSERT_TRUE(store.addProject(record));
            paths.append(record.canonicalPath);
        }
        page = std::make_unique<VNH::ProjectsPage>(&store);
        page->resize(1100, 600);
        page->show();
        table = page->findChild<QTableWidget*>();
        ASSERT_NE(table, nullptr);
        QCoreApplication::processEvents();
    }

    void TearDown() override
    {
        page.reset();
        QThreadPool::globalInstance()->waitForDone(5000);
        QCoreApplication::processEvents();
        qunsetenv("HARKEN_HUB_USER_ROOT");
        qunsetenv("HARKEN_HUB_MACHINE_ROOT");
    }

    void clickRow(int row, Qt::MouseButton button = Qt::LeftButton,
                  Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        const QPoint pos = table->visualItemRect(table->item(row, 0)).center();
        QTest::mouseClick(page->windowHandle(), button, modifiers,
                         table->viewport()->mapTo(page.get(), pos));
    }

    void clickHeader(int column)
    {
        auto* header = table->horizontalHeader();
        QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier,
                         QPoint(header->sectionViewportPosition(column) + header->sectionSize(column) / 2,
                                header->height() / 2));
        QCoreApplication::processEvents();
    }

    QSet<QString> selectedPaths() const
    {
        QSet<QString> result;
        for (const auto& index : table->selectionModel()->selectedRows(0))
            result.insert(index.data(Qt::UserRole + 1).toString());
        return result;
    }

    QTemporaryDir dir;
    VNH::ProjectStore store;
    QStringList paths;
    std::unique_ptr<VNH::ProjectsPage> page;
    QTableWidget* table = nullptr;
};

TEST_F(ProjectsPageSelectionTest, SelectionSurvivesReorderingAndMetadataRefresh)
{
    VNH::InstalledVersion newer;
    newer.buildId = "newer";
    newer.semanticVersion = "1.10.0";
    auto older = newer;
    older.buildId = "older";
    older.semanticVersion = "1.9.0";
    page->setVersionsProvider([=]() { return std::vector<VNH::InstalledVersion>{newer, older}; });
    ASSERT_TRUE(store.setPreferredBuild(paths[0], newer.buildId));
    ASSERT_TRUE(store.setPreferredBuild(paths[1], older.buildId));
    ASSERT_TRUE(store.setLastOpened(paths[0], "2026-10-21T10:00:00+08:00"));
    ASSERT_TRUE(store.setLastOpened(paths[1], "2026-10-21T03:00:00Z"));
    clickRow(0);
    clickRow(1, Qt::LeftButton, Qt::ShiftModifier);
    const auto selected = selectedPaths();
    ASSERT_EQ(selected.size(), 2);
    const QString currentPath = table->item(table->currentRow(), 0)->data(Qt::UserRole + 1).toString();
    ASSERT_TRUE(store.recordSuccessfulOpen(paths[2], {}, QStringLiteral("Aardvark"), 1, {}, {}));
    page->refresh();
    EXPECT_EQ(table->item(0, 0)->data(Qt::UserRole + 1).toString(), paths[2]);
    EXPECT_EQ(selectedPaths(), selected);
    EXPECT_EQ(table->item(table->currentRow(), 0)->data(Qt::UserRole + 1).toString(), currentPath);
    ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
    QCoreApplication::processEvents();
    EXPECT_EQ(selectedPaths(), selected);
    const std::vector<std::pair<int, QStringList>> orders{
        {0, {paths[1], paths[0], paths[2]}},
        {1, {paths[0], paths[1], paths[2]}},
        {1, {paths[1], paths[0], paths[2]}},
        {2, {paths[1], paths[0], paths[2]}},
        {2, {paths[0], paths[1], paths[2]}},
        {3, {paths[0], paths[1], paths[2]}},
    };
    for (const auto& [column, expectedPaths] : orders) {
        clickHeader(column);
        for (int row = 0; row < table->rowCount(); ++row)
            EXPECT_EQ(table->item(row, 0)->data(Qt::UserRole + 1).toString(), expectedPaths[row]);
        page->refresh();
        ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
        QCoreApplication::processEvents();
        for (int row = 0; row < table->rowCount(); ++row)
            EXPECT_EQ(table->item(row, 0)->data(Qt::UserRole + 1).toString(), expectedPaths[row]);
        EXPECT_EQ(selectedPaths(), selected);
        EXPECT_EQ(table->item(table->currentRow(), 0)->data(Qt::UserRole + 1).toString(), currentPath);
    }
    page.reset();
    VNH::ProjectsPage reopened(&store);
    reopened.setVersionsProvider([=]() { return std::vector<VNH::InstalledVersion>{newer, older}; });
    reopened.refresh();
    const auto* reopenedTable = reopened.findChild<QTableWidget*>();
    EXPECT_EQ(reopenedTable->horizontalHeader()->sortIndicatorSection(), 2);
    EXPECT_EQ(reopenedTable->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
    EXPECT_EQ(reopenedTable->item(0, 0)->data(Qt::UserRole + 1).toString(), paths[0]);
}

TEST_F(ProjectsPageSelectionTest, BatchMenuOnlyRemovesSelectedRecordsAndKeepsFiles)
{
    clickRow(0);
    clickRow(1, Qt::LeftButton, Qt::ControlModifier);
    const auto selected = selectedPaths();
    ASSERT_EQ(selected.size(), 2);
    bool visited = false;
    QTimer::singleShot(0, page.get(), [&]() {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        ASSERT_NE(menu, nullptr);
        const auto close = qScopeGuard([&]() { menu->close(); });
        visited = true;
        EXPECT_EQ(selectedPaths(), selected);
        ASSERT_EQ(menu->actions().size(), 1);
        EXPECT_TRUE(menu->actions().front()->isEnabled());
        ASSERT_TRUE(store.recordSuccessfulOpen(paths[2], {}, QStringLiteral("Aardvark"), 1, {}, {}));
        page->refresh();
        menu->actions().front()->trigger();
    });
    clickRow(1, Qt::RightButton);
    EXPECT_TRUE(visited);
    EXPECT_EQ(store.listProjects().size(), 1u);
    for (const auto& path : paths) {
        EXPECT_EQ(store.contains(path), !selected.contains(path));
        EXPECT_TRUE(QFileInfo::exists(path));
    }
}

TEST_F(ProjectsPageSelectionTest, UnselectedRowMenuKeepsProjectIdentityAfterSorting)
{
    clickRow(0);
    clickRow(1, Qt::LeftButton, Qt::ControlModifier);
    ASSERT_EQ(selectedPaths().size(), 2);
    clickHeader(0);
    const QString clickedPath = table->item(0, 0)->data(Qt::UserRole + 1).toString();
    ASSERT_EQ(clickedPath, paths[2]);
    QTimer::singleShot(0, page.get(), [&]() {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        ASSERT_NE(menu, nullptr);
        const auto close = qScopeGuard([&]() { menu->close(); });
        ASSERT_EQ(menu->actions().size(), 4);
        EXPECT_EQ(selectedPaths(), QSet<QString>{clickedPath});
        menu->actions().last()->trigger();
    });
    auto* menuButton = table->cellWidget(0, 3)->findChild<QToolButton*>(QStringLiteral("rowMenuButton"));
    ASSERT_NE(menuButton, nullptr);
    menuButton->click();
    EXPECT_EQ(store.listProjects().size(), 2u);
    EXPECT_FALSE(store.contains(clickedPath));
    EXPECT_TRUE(QFileInfo::exists(clickedPath));
}

TEST_F(ProjectsPageSelectionTest, NewProjectTemplateRowIsHidden)
{
    VNH::InstalledVersion version;
    version.buildId = QStringLiteral("test-build");
    version.semanticVersion = QStringLiteral("1.0.0");
    version.installPath = dir.path();
    page->setVersionsProvider([version]() { return std::vector<VNH::InstalledVersion>{version}; });
    page->showNewProjectPage();
    auto* templates = page->findChild<QComboBox*>(QStringLiteral("newProjectTemplate"));
    ASSERT_NE(templates, nullptr);
    EXPECT_TRUE(templates->parentWidget()->isHidden());
    EXPECT_FALSE(templates->isVisible());
}
