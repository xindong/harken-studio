#pragma once

#include <QComboBox>

class QMenu;

namespace harken::studio {

class ToolBarComboBox : public QComboBox
{
    Q_OBJECT

public:
    explicit ToolBarComboBox(QWidget* parent = nullptr);
};

class ToolBarMenuComboBox final : public ToolBarComboBox
{
    Q_OBJECT

public:
    explicit ToolBarMenuComboBox(QWidget* parent = nullptr);

    QMenu* menu() const { return menu_; }
    void setDisplayText(const QString& text);
    QSize minimumSizeHint() const override;
    void showPopup() override;
    void hidePopup() override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    QMenu* menu_;
};

}
