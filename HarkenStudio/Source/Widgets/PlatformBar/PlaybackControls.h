#pragma once

#include <QWidget>

class QToolButton;

namespace harken::studio {

struct ProjectItem;

class PlaybackControls : public QWidget
{
    Q_OBJECT
public:
    explicit PlaybackControls(QWidget* parent = nullptr);

    void setAuditionTarget(ProjectItem* item);
    void setPlaying(bool playing);
    void setResetEnabled(bool enabled);
    bool isResetEnabled() const;

signals:
    void playClicked();
    void stopClicked();
    void resetClicked();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void applyTheme();
    void refreshShortcuts();

    QToolButton* playBtn_ = nullptr;
    QToolButton* stopBtn_ = nullptr;
    QToolButton* resetBtn_ = nullptr;
};

}
