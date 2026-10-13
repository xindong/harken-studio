#pragma once

#include <QWidget>

class QToolButton;

namespace VNX {

class PlaybackControls : public QWidget
{
    Q_OBJECT
public:
    explicit PlaybackControls(QWidget* parent = nullptr);

    void setPlaying(bool playing);
    void setResetEnabled(bool enabled);
    bool isResetEnabled() const;

signals:
    void playClicked();
    void stopClicked();
    void resetClicked();

private:
    void applyTheme();
    void refreshShortcuts();

    QToolButton* playBtn_ = nullptr;
    QToolButton* stopBtn_ = nullptr;
    QToolButton* resetBtn_ = nullptr;
};

}
