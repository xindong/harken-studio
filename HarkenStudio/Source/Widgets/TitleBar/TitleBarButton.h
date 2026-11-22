#pragma once

#include <QColor>
#include <QAbstractButton>

namespace harken::studio {

enum class TitleBarButtonKind {
    Minimize,
    Maximize,
    Restore,
    Close
};

class TitleBarButton : public QAbstractButton {
public:
    explicit TitleBarButton(TitleBarButtonKind kind, QWidget* parent = nullptr);

    void setButtonKind(TitleBarButtonKind kind);
    TitleBarButtonKind buttonKind() const { return kind_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor iconColor() const;

    TitleBarButtonKind kind_;
};

}
