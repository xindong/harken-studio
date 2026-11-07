
#pragma once

#include <QWidget>
#include <QPropertyAnimation>
#include <QLabel>

class QSpacerItem;

namespace VNX {

class ToggleSwitch : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(bool checked READ isChecked WRITE setChecked NOTIFY toggled)
    Q_PROPERTY(qreal position READ position WRITE setPosition)

public:
    explicit ToggleSwitch(const QString& label = QString(), QWidget* parent = nullptr);
    ~ToggleSwitch() override;

    bool isChecked() const { return checked_; }
    qreal position() const { return position_; }

public slots:
    void setChecked(bool checked);
    void toggle();

signals:
    void toggled(bool checked);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    void applyTheme();
    void setPosition(qreal pos);
    void animateToggle();

    QLabel* label_;
    bool checked_ = false;
    qreal position_ = 0.0;
    
    QPropertyAnimation* animation_;
    
    QSpacerItem* switchSpace_ = nullptr;
};

}
