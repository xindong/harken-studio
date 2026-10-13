#include "Core/ActionManager.h"
#include "Widgets/Inspector/ObjectReferencePicker.h"
#include "Widgets/Inspector/WidgetFactory.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonObject>

#include <memory>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

std::unique_ptr<QWidget> buildDeclarativePicker(const QString& actionId)
{
    QJsonObject config;
    config[QStringLiteral("filterType")] = QStringLiteral("Bus");
    config[QStringLiteral("field")] = QStringLiteral("outputBusId");
    config[QStringLiteral("action")] = actionId;
    QJsonObject cfg;
    cfg[QStringLiteral("widget")] = QStringLiteral("ObjectReferencePicker");
    cfg[QStringLiteral("config")] = config;
    return std::unique_ptr<QWidget>(
        VNX::WidgetFactory::instance().create(QStringLiteral("custom_widget"), cfg));
}

} // namespace

TEST(ObjectReferencePicker, DeclarativePathBuildsObjectRefWidgetForm)
{
    ensureApp();
    auto w = buildDeclarativePicker(QStringLiteral("test.orp_form"));
    ASSERT_NE(w, nullptr);

    auto* picker = qobject_cast<VNX::ObjectReferencePicker*>(w.get());
    ASSERT_NE(picker, nullptr);
    EXPECT_EQ(picker->filterType(), VNX::ProjectItem::Type::Bus);
    EXPECT_EQ(picker->actionFieldName(), QStringLiteral("outputBusId"));
    EXPECT_EQ(picker->actionId(), QStringLiteral("test.orp_form"));

    auto* ref = w->findChild<VNX::ObjectRefWidget*>();
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->filterType(), VNX::ProjectItem::Type::Bus);
}

TEST(ObjectReferencePicker, SelectionSubmitsThroughBoundAction)
{
    ensureApp();
    const QString actionId = QStringLiteral("test.orp_submit");

    QJsonObject captured;
    int calls = 0;
    VNX::ActionDef def;
    def.id = actionId;
    def.undoable = false;
    def.handler = [&](const QJsonObject& args) -> VNX::ActionResult {
        ++calls;
        captured = args;
        VNX::ActionResult result;
        result.success = true;
        return result;
    };
    VNX::ActionManager::instance().registerAction(def);

    auto w = buildDeclarativePicker(actionId);
    ASSERT_NE(w, nullptr);
    auto* picker = qobject_cast<VNX::ObjectReferencePicker*>(w.get());
    ASSERT_NE(picker, nullptr);
    auto* ref = w->findChild<VNX::ObjectRefWidget*>();
    ASSERT_NE(ref, nullptr);

    emit ref->objectChanged(42u);
    EXPECT_EQ(calls, 0);

    VNX::ProjectItem host(QStringLiteral("Shot"), VNX::ProjectItem::Type::Sound);
    host.objectID = 77u;
    dynamic_cast<VNX::IDeclarativeCustomWidget*>(picker)->bindProjectItem(&host);

    uint32_t forwarded = 0;
    QObject::connect(picker, &VNX::ObjectReferencePicker::objectChanged,
                     [&forwarded](uint32_t id) { forwarded = id; });

    emit ref->objectChanged(42u);
    EXPECT_EQ(forwarded, 42u);
    ASSERT_EQ(calls, 1);
    EXPECT_EQ(captured.value(QStringLiteral("object_id")).toInt(), 77);
    EXPECT_EQ(captured.value(QStringLiteral("output_bus_id")).toInt(), 42);
}
