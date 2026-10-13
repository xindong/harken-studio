#pragma once

#include "Core/StartupProgress.h"

#include <QWidget>

#include <memory>

class QLabel;
class QProgressBar;

namespace VNX {

class StartupRenderer;

class StartupLoadingWindow : public QWidget
{
    Q_OBJECT

public:
    explicit StartupLoadingWindow(QWidget* parent = nullptr);
    ~StartupLoadingWindow() override;

    void showAfterInitialFrame();
    void setProjectName(const QString& projectName);
    void setPhase(StartupPhase phase, const QString& statusOverride = {});
    void setProgress(int percent, const QString& status);
    void showFailure(const QString& message);

    StartupPhase phase() const { return phase_; }
    int progressValue() const;
    bool renderThreadRunning() const;
    quintptr renderThreadId() const;
    bool renderSurfaceOwnedByGuiThread() const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void centerOnScreen();

    QLabel* projectLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* percentLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    std::unique_ptr<StartupRenderer> renderer_;
    StartupPhase phase_ = StartupPhase::PreparingApplication;
    int progress_ = 0;
    bool failed_ = false;
    bool waitingForInitialFrame_ = false;
};

}
