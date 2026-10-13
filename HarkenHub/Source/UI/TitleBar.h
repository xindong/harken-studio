#pragma once

#include <QPushButton>
#include <QWidget>

namespace VNH {

enum class TitleBarButtonKind {
    Minimize,
    Maximize,
    Restore,
    Close
};

class TitleBarButton : public QPushButton
{
public:
    explicit TitleBarButton(TitleBarButtonKind kind, QWidget* parent = nullptr);

    void setButtonKind(TitleBarButtonKind kind);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor iconColor() const;

    TitleBarButtonKind kind_;
};

class TitleBar : public QWidget
{
    Q_OBJECT

public:
    explicit TitleBar(QWidget* parent = nullptr);

    void setMaximized(bool maximized);

    TitleBarButton* minimizeButton() const { return btnMinimize_; }
    TitleBarButton* maximizeButton() const { return btnMaximize_; }
    TitleBarButton* closeButton() const { return btnClose_; }

private:
    TitleBarButton* btnMinimize_ = nullptr;
    TitleBarButton* btnMaximize_ = nullptr;
    TitleBarButton* btnClose_ = nullptr;
};

}
