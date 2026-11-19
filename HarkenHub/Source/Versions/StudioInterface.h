#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace VNH {

struct InstalledVersion;

struct StudioTemplate
{
    QString id;
    QString displayName;
    QString description;
};

namespace StudioInterface {

bool canCreateProjects(const QString& studioVersion);
bool canSelfTest(const QString& studioVersion);
bool canListTemplates(const QString& studioVersion);
QString executable(const InstalledVersion& version);
std::optional<std::vector<StudioTemplate>> listTemplates(const InstalledVersion& version, QString* error);

}

}
