#include "Core/ToastManager.h"

#include <QApplication>
#include <QEvent>
#include <QScopeGuard>

#include <gtest/gtest.h>

#include <memory>

TEST(ToastManagerTest, DestroyedParentSuppressesToastsUntilAnotherParentIsBound)
{
    using namespace VNX;
    auto& manager = ToastManager::instance();
    const auto previousWidgets = QApplication::topLevelWidgets();
    const auto newToasts = [&]() {
        QList<QToastWidget*> toasts;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* toast = qobject_cast<QToastWidget*>(widget);
                toast && !previousWidgets.contains(widget)) {
                toasts.push_back(toast);
            }
        }
        return toasts;
    };
    const auto cleanup = qScopeGuard([&]() {
        manager.setParentWidget(nullptr);
        for (auto* toast : newToasts()) {
            emit toast->finished();
            toast->close();
        }
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });

    auto parent = std::make_unique<QWidget>();
    manager.setParentWidget(parent.get());
    manager.showToast(QStringLiteral("Before closing"));
    ASSERT_EQ(newToasts().size(), 1);
    parent.reset();
    manager.showToast(QStringLiteral("After closing"));
    EXPECT_EQ(newToasts().size(), 1);

    auto toasts = newToasts();
    for (auto* toast : toasts) {
        emit toast->finished();
        toast->close();
    }
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(newToasts().isEmpty());

    QWidget replacement;
    replacement.setGeometry(100, 100, 800, 600);
    manager.setParentWidget(&replacement);
    manager.showToast(QStringLiteral("After reopening"));
    toasts = newToasts();
    ASSERT_EQ(toasts.size(), 1);
    EXPECT_TRUE(toasts.front()->isVisible());
    EXPECT_TRUE(replacement.geometry().contains(toasts.front()->geometry()));
}
