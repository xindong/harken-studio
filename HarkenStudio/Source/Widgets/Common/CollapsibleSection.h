
#pragma once

#include <QWidget>
#include <QFrame>
#include <QToolButton>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QGridLayout>
#include "../../Core/ThemeManager.h"

namespace VNX {

class CollapsibleSection : public QWidget
{
    Q_OBJECT

public:
    explicit CollapsibleSection(const QString& title, 
                                int animationDuration = 150, 
                                QWidget* parent = nullptr);
    ~CollapsibleSection() override;

    void setContentLayout(QLayout* contentLayout);
    void setHeaderWidget(QWidget* widget);
    QWidget* headerWidget() const { return headerWidget_; }
    void setExpanded(bool expanded);
    bool isExpanded() const { return expanded_; }

    void setDynamicContent(bool dynamic);
    QString title() const { return toggleButton_->text(); }

    void setIndicatorColor(const QColor& color);
    void setHeaderTintColor(const QColor& color);

public slots:
    void toggle();
    void expand();
    void collapse();

signals:
    void expandedChanged(bool expanded);

protected:
    void showEvent(QShowEvent* event) override;
    void moveEvent(QMoveEvent* event) override;

private:
    void applyTheme();
    void refreshAppearance();
    int detectNestingLevel() const;

    QToolButton* toggleButton_;
    QGridLayout* headerLayout_;
    QWidget* headerWidget_ = nullptr;
    QFrame* headerLine_;
    QWidget* contentArea_;
    QWidget* innerContent_ = nullptr;
    QFrame* railFrame_ = nullptr;
    QVBoxLayout* mainLayout_;
    QParallelAnimationGroup* toggleAnimation_;

    QColor indicatorColor_;
    QColor headerTintColor_;
    int animationDuration_;
    bool expanded_ = false;
    bool dynamicContent_ = false;
    bool hasCustomIndicator_ = false;
    bool hasTopSeparator_ = false;
    int level_ = -1;
};

}
