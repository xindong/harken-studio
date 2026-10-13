#pragma once

#include <QWidget>

class QToolButton;
class QAction;

namespace VNX {

class ThemeManager;

class ListFooter : public QWidget
{
    Q_OBJECT

public:
    enum class Position { Left, Right };
    explicit ListFooter(QWidget* parent = nullptr, bool standardButtons = true,
                        ThemeManager* themeManager = nullptr);
    QToolButton* addButton() const { return addButton_; }
    QToolButton* removeButton() const { return removeButton_; }
    QToolButton* addActionButton(QAction* action, bool compact = false,
                                Position position = Position::Right, const QString& iconAsset = {});
    QToolButton* setRefreshAction(QAction* action);
    void addStatusWidget(QWidget* widget);

protected:
    void changeEvent(QEvent* event) override;

private:
    void applyTheme();

    ThemeManager* themeManager_;
    QToolButton* addButton_ = nullptr;
    QToolButton* removeButton_ = nullptr;
    QToolButton* refreshButton_ = nullptr;
    int leftCount_ = 0;
};

}
