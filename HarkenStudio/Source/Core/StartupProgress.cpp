#include "StartupProgress.h"

namespace VNX {

StartupProgress startupProgressFor(StartupPhase phase)
{
    switch (phase) {
        case StartupPhase::PreparingApplication:
            return {phase, 8, QStringLiteral("Preparing application")};
        case StartupPhase::CreatingWorkspace:
            return {phase, 20, QStringLiteral("Creating workspace")};
        case StartupPhase::StartingServices:
            return {phase, 36, QStringLiteral("Starting editor services")};
        case StartupPhase::ReadingProject:
            return {phase, 48, QStringLiteral("Reading project")};
        case StartupPhase::LoadingProjectContent:
            return {phase, 65, QStringLiteral("Loading project content")};
        case StartupPhase::PreparingRuntime:
            return {phase, 82, QStringLiteral("Preparing audio runtime")};
        case StartupPhase::RestoringWorkspace:
            return {phase, 90, QStringLiteral("Restoring workspace")};
        case StartupPhase::ShowingEditor:
            return {phase, 96, QStringLiteral("Showing editor")};
        case StartupPhase::Ready:
            return {phase, 100, QStringLiteral("Ready")};
    }
    return {phase, 0, QString()};
}

}
