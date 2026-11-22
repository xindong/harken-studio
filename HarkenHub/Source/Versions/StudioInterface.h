#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace harken::hub {

struct InstalledVersion;

struct StudioIdentity
{
    QString version;
    QString buildId;
    QString channel;
};

struct StudioTemplate
{
    QString id;
    QString displayName;
    QString description;
};

namespace StudioInterface {

bool canCreateProjects(const QString& studioVersion);
bool canListTemplates(const QString& studioVersion);
QString executable(const InstalledVersion& version);
std::optional<StudioIdentity> identify(const QString& executable, QString* error);
std::optional<std::vector<StudioTemplate>> listTemplates(const InstalledVersion& version, QString* error);

}

}
