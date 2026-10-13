#pragma once

#include <QString>

namespace VNX {

enum class StartupPhase
{
    PreparingApplication,
    CreatingWorkspace,
    StartingServices,
    ReadingProject,
    LoadingProjectContent,
    PreparingRuntime,
    RestoringWorkspace,
    ShowingEditor,
    Ready,
};

struct StartupProgress
{
    StartupPhase phase = StartupPhase::PreparingApplication;
    int percent = 0;
    QString status;
};

StartupProgress startupProgressFor(StartupPhase phase);

}
