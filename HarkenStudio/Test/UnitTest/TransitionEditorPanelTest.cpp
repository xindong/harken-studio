#include "Widgets/TransitionEditor/TransitionEditorPanel.h"
#include "Widgets/CurveEditor/CurveCanvas.h"
#include "Widgets/TransitionEditor/TransitionFadeButton.h"
#include "ActionManager.h"
#include "Components/VSelection.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QToolButton>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void drainEvents()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

QDialog* openFadeDuration(VNX::TransitionFadePopup* popup)
{
    auto* canvas = popup->findChild<VNX::CurveCanvas*>();
    if (!canvas)
        return nullptr;
    for (const auto& tick : canvas->renderData().textTicks) {
        if (tick.bound != VNX::CurveAxisBound::XMax)
            continue;
        const QPointF point = tick.rect.center();
        QMouseEvent click(QEvent::MouseButtonDblClick, point, canvas->mapToGlobal(point.toPoint()),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &click);
        for (auto* dialog : canvas->findChildren<QDialog*>(QStringLiteral("curveCanvasRangeEditor")))
            if (dialog->isVisible())
                return dialog;
    }
    return nullptr;
}

bool editFadeDuration(VNX::TransitionFadePopup* popup, double value)
{
    auto* dialog = openFadeDuration(popup);
    if (!dialog)
        return false;
    drainEvents();
    auto* input = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("curveCanvasRangeInput"));
    if (!input)
        return false;
    input->setValue(value);
    dialog->accept();
    return true;
}

}

class TransitionEditorPanelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &manager, nullptr, {});

        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("TransitionPanelProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));

        container_ = manager.createItem(nullptr, QStringLiteral("Container"),
                                        VNX::ProjectItem::Type::Container);
        childA_ = manager.createItem(container_, QStringLiteral("ChildA"),
                                     VNX::ProjectItem::Type::Sound);
        childB_ = manager.createItem(container_, QStringLiteral("ChildB"),
                                     VNX::ProjectItem::Type::Sound);
        childC_ = manager.createItem(container_, QStringLiteral("ChildC"),
                                     VNX::ProjectItem::Type::Sound);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(childA_, nullptr);
        ASSERT_NE(childB_, nullptr);
        ASSERT_NE(childC_, nullptr);
        container_->compOrCreate<VNX::VSelection>()->mode = VNX::SelectionMode::Sequence;
        container_->compOrCreate<VNX::VSelection>()->playType = VNX::SelectionPlayType::Queue;
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* container_ = nullptr;
    VNX::ProjectItem* childA_ = nullptr;
    VNX::ProjectItem* childB_ = nullptr;
    VNX::ProjectItem* childC_ = nullptr;
};

TEST_F(TransitionEditorPanelTest, FadePopupCommitFollowsRuleIdentityAfterReorder)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    ASSERT_TRUE(panel.addRule(childA_->guid, std::nullopt));
    int specific = -1;
    for (int i = 0; i < panel.ruleCount(); ++i) {
        if (panel.ruleSource(i) == std::optional<QUuid>(childA_->guid)
            && panel.ruleTarget(i) == std::optional<QUuid>(childB_->guid))
            specific = i;
    }
    ASSERT_GE(specific, 0);
    ASSERT_TRUE(panel.selectRuleDisplayIndex(specific));
    panel.show();
    drainEvents();

    auto* button = panel.findChild<QToolButton*>(QStringLiteral("transitionFadeSourceOut"));
    ASSERT_NE(button, nullptr);
    button->click();
    drainEvents();
    auto* popup = panel.findChild<VNX::TransitionFadePopup*>(
        QStringLiteral("transitionFadePopup"));
    ASSERT_NE(popup, nullptr);

    ASSERT_TRUE(panel.addRule(childA_->guid, childC_->guid));
    drainEvents();

    ASSERT_TRUE(editFadeDuration(popup, 1.75));
    drainEvents();

    int updated = -1;
    for (int i = 0; i < panel.ruleCount(); ++i) {
        if (panel.ruleSource(i) == std::optional<QUuid>(childA_->guid)
            && panel.ruleTarget(i) == std::optional<QUuid>(childB_->guid))
            updated = i;
    }
    ASSERT_GE(updated, 0);
    EXPECT_FLOAT_EQ(
        panel.ruleFade(updated, VNX::TransitionFadeSlot::SourceFadeOut).durationSeconds, 1.75f);
    popup->close();
}
