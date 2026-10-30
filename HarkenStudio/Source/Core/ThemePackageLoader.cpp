#include "ThemePackageLoader.h"

#include "BuiltinTheme.h"
#include "ThemeColorResolver.h"
#include "ComponentRecipeRegistry.h"
#include "ParameterPresenterRegistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <type_traits>

namespace VNX {

namespace {

constexpr auto kBuiltinThemeId = "harken.builtin.dark";

struct BuiltinThemeDefinition
{
    const char* id;
    const char* displayName;
    ThemeSnapshot (*create)();
};

constexpr BuiltinThemeDefinition kBuiltinThemes[]{
    {kBuiltinThemeId, "Harken Dark", createBuiltinDarkTheme},
};

const BuiltinThemeDefinition* findBuiltinTheme(const QString& id)
{
    for (const auto& theme : kBuiltinThemes)
        if (id == QLatin1String(theme.id))
            return &theme;
    return nullptr;
}

ThemePackageDiagnostic diagnostic(ThemePackageErrorCode code,
                                  const QString& file,
                                  const QString& path,
                                  const QString& message)
{
    return ThemePackageDiagnostic{code, file, path, message};
}

class JsonGuard
{
public:
    JsonGuard(const QByteArray& json, int maximumDepth, int maximumNodes)
        : json_(json)
        , maximumDepth_(maximumDepth)
        , maximumNodes_(maximumNodes)
    {
    }

    std::optional<ThemePackageDiagnostic> inspect(const QString& file)
    {
        file_ = file;
        skipWhitespace();
        if (!parseValue(1, QStringLiteral("$")))
            return error_;
        skipWhitespace();
        if (position_ != json_.size())
            fail(ThemePackageErrorCode::InvalidJson, QStringLiteral("$"),
                 QStringLiteral("Unexpected trailing data."));
        return error_;
    }

private:
    bool parseValue(int depth, const QString& path)
    {
        if (depth > maximumDepth_)
            return fail(ThemePackageErrorCode::ResourceLimit, path,
                        QStringLiteral("JSON nesting depth exceeds the limit."));
        if (++nodes_ > maximumNodes_)
            return fail(ThemePackageErrorCode::ResourceLimit, path,
                        QStringLiteral("JSON node count exceeds the limit."));
        skipWhitespace();
        if (position_ >= json_.size())
            return failInvalid(path);
        const char value = json_.at(position_);
        if (value == '{')
            return parseObject(depth, path);
        if (value == '[')
            return parseArray(depth, path);
        if (value == '"')
        {
            QString ignored;
            return parseString(ignored, path);
        }
        const qsizetype start = position_;
        while (position_ < json_.size())
        {
            const char current = json_.at(position_);
            if (current == ',' || current == ']' || current == '}'
                || current == ' ' || current == '\t' || current == '\r'
                || current == '\n')
                break;
            ++position_;
        }
        return position_ > start || failInvalid(path);
    }

    bool parseObject(int depth, const QString& path)
    {
        ++position_;
        skipWhitespace();
        if (take('}'))
            return true;
        QSet<QString> keys;
        while (position_ < json_.size())
        {
            QString key;
            if (!parseString(key, path))
                return false;
            const QString keyPath = path + QLatin1Char('.') + key;
            if (keys.contains(key))
                return fail(ThemePackageErrorCode::DuplicateJsonKey, keyPath,
                            QStringLiteral("Duplicate JSON object key."));
            keys.insert(key);
            skipWhitespace();
            if (!take(':'))
                return failInvalid(keyPath);
            if (!parseValue(depth + 1, keyPath))
                return false;
            skipWhitespace();
            if (take('}'))
                return true;
            if (!take(','))
                return failInvalid(path);
            skipWhitespace();
        }
        return failInvalid(path);
    }

    bool parseArray(int depth, const QString& path)
    {
        ++position_;
        skipWhitespace();
        if (take(']'))
            return true;
        int index = 0;
        while (position_ < json_.size())
        {
            const QString itemPath = path + QLatin1Char('[')
                + QString::number(index++) + QLatin1Char(']');
            if (!parseValue(depth + 1, itemPath))
                return false;
            skipWhitespace();
            if (take(']'))
                return true;
            if (!take(','))
                return failInvalid(path);
            skipWhitespace();
        }
        return failInvalid(path);
    }

    bool parseString(QString& output, const QString& path)
    {
        if (position_ >= json_.size() || json_.at(position_) != '"')
            return failInvalid(path);
        const qsizetype start = position_++;
        bool escaped = false;
        while (position_ < json_.size())
        {
            const char current = json_.at(position_++);
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (current == '\\')
            {
                escaped = true;
                continue;
            }
            if (current == '"')
            {
                const QByteArray token = json_.mid(start, position_ - start);
                QJsonParseError parseError;
                const QJsonDocument decoded = QJsonDocument::fromJson(
                    QByteArray("[") + token + QByteArray("]"), &parseError);
                if (parseError.error != QJsonParseError::NoError
                    || !decoded.isArray() || decoded.array().size() != 1
                    || !decoded.array().at(0).isString())
                    return failInvalid(path);
                output = decoded.array().at(0).toString();
                return true;
            }
        }
        return failInvalid(path);
    }

    void skipWhitespace()
    {
        if (position_ == 0 && json_.startsWith("\xEF\xBB\xBF"))
            position_ = 3;
        while (position_ < json_.size())
        {
            const char value = json_.at(position_);
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
                break;
            ++position_;
        }
    }

    bool take(char expected)
    {
        if (position_ >= json_.size() || json_.at(position_) != expected)
            return false;
        ++position_;
        return true;
    }

    bool failInvalid(const QString& path)
    {
        return fail(ThemePackageErrorCode::InvalidJson, path,
                    QStringLiteral("Malformed JSON."));
    }

    bool fail(ThemePackageErrorCode code,
              const QString& path,
              const QString& message)
    {
        if (!error_)
            error_ = diagnostic(code, file_, path, message);
        return false;
    }

    const QByteArray& json_;
    int maximumDepth_;
    int maximumNodes_;
    qsizetype position_ = 0;
    int nodes_ = 0;
    QString file_;
    std::optional<ThemePackageDiagnostic> error_;
};

bool isContained(const QString& parent, const QString& child)
{
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    const QString normalizedParent = QDir::fromNativeSeparators(parent);
    const QString normalizedChild = QDir::fromNativeSeparators(child);
    if (QString::compare(normalizedParent, normalizedChild, sensitivity) == 0)
        return true;
    QString prefix = normalizedParent;
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix += QLatin1Char('/');
    return normalizedChild.startsWith(prefix, sensitivity);
}

std::optional<QString> constrainedPath(const QString& packageRoot,
                                       const QString& relativePath,
                                       bool mustExist,
                                       const QString& file,
                                       const QString& fieldPath,
                                       QList<ThemePackageDiagnostic>& diagnostics)
{
    const QString normalized = QDir::cleanPath(
        QDir::fromNativeSeparators(relativePath));
    if (relativePath.isEmpty() || QDir::isAbsolutePath(relativePath)
        || normalized == QStringLiteral("..")
        || normalized.startsWith(QStringLiteral("../")))
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::PathEscape, file, fieldPath,
            QStringLiteral("Theme paths must remain relative to the package root.")));
        return std::nullopt;
    }

    const QString packageCanonical = QFileInfo(packageRoot).canonicalFilePath();
    const QString absolute = QDir(packageRoot).absoluteFilePath(normalized);
    QFileInfo target(absolute);
    if (mustExist && (!target.exists() || !target.isFile()))
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::FileNotFound, file, fieldPath,
            QStringLiteral("Referenced theme file does not exist.")));
        return std::nullopt;
    }

    QFileInfo ancestor = target;
    while (!ancestor.exists() && !ancestor.absoluteFilePath().isEmpty())
    {
        const QDir parent = ancestor.absoluteDir();
        if (parent.absolutePath() == ancestor.absoluteFilePath())
            break;
        ancestor = QFileInfo(parent.absolutePath());
    }
    const QString resolved = target.exists()
        ? target.canonicalFilePath()
        : ancestor.canonicalFilePath();
    if (packageCanonical.isEmpty() || resolved.isEmpty()
        || !isContained(packageCanonical, resolved))
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::PathEscape, file, fieldPath,
            QStringLiteral("Theme path resolves outside the package root.")));
        return std::nullopt;
    }
    return mustExist ? target.canonicalFilePath() : normalized;
}

std::optional<QByteArray> readJsonFile(
    const QString& absolutePath,
    const QString& displayPath,
    const ThemePackageLoadLimits& limits,
    qsizetype& totalBytes,
    QList<ThemePackageDiagnostic>& diagnostics)
{
    QFile file(absolutePath);
    const qint64 size = file.size();
    if (size < 0 || size > limits.maximumFileBytes
        || totalBytes > limits.maximumTotalBytes - size)
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::ResourceLimit, displayPath,
            QStringLiteral("$"),
            QStringLiteral("Theme JSON read budget exceeded.")));
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::FileNotFound, displayPath,
            QStringLiteral("$"), QStringLiteral("Theme JSON file cannot be read.")));
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    totalBytes += bytes.size();
    if (const auto guard = JsonGuard(bytes, limits.maximumJsonDepth,
                                     limits.maximumJsonNodes)
                               .inspect(displayPath))
    {
        diagnostics.append(*guard);
        return std::nullopt;
    }
    return bytes;
}

struct SemanticDocuments
{
    ThemeTokenDto tokens;
    ThemeVisualDto curve;
    ThemeVisualDto meter;
    ThemeVisualDto waveform;
    ThemeVisualDto nodeGraph;
    ThemeVisualDto blueprint;
    ThemeVisualDto timeline;
    ThemeVisualDto music;
    ThemeVisualDto log;
    ThemeVisualDto monitor;
    ThemeVisualDto ai;
    ThemeVisualDto scene3d;
    ThemeVisualDto spatial;
    ThemeVisualDto feedback;
    ThemePresenterMapDto presenters;
    ThemeRecipeMapDto recipes;
    ThemeAssetMapDto assets;
};

struct DiscoveredPackage
{
    QString root;
    QString displayRoot;
    ThemeManifestDto manifest;
    qsizetype manifestBytes = 0;
};

template<typename T>
void overlayMap(QMap<QString, T>& destination, const QMap<QString, T>& source)
{
    for (auto it = source.constBegin(); it != source.constEnd(); ++it)
        destination.insert(it.key(), it.value());
}

void overlay(SemanticDocuments& destination, const SemanticDocuments& source)
{
    overlayMap(destination.tokens.colors, source.tokens.colors);
    overlayMap(destination.tokens.metrics, source.tokens.metrics);
    for (auto it = source.tokens.fonts.constBegin();
         it != source.tokens.fonts.constEnd(); ++it)
    {
        ThemeTokenDto::FontRole role = destination.tokens.fonts.value(it.key());
        if (it->family)
            role.family = it->family;
        if (it->pointSize)
            role.pointSize = it->pointSize;
        if (it->weight)
            role.weight = it->weight;
        destination.tokens.fonts.insert(it.key(), std::move(role));
    }
    overlayMap(destination.curve.colors, source.curve.colors);
    overlayMap(destination.curve.metrics, source.curve.metrics);
    overlayMap(destination.meter.colors, source.meter.colors);
    overlayMap(destination.meter.metrics, source.meter.metrics);
    overlayMap(destination.waveform.colors, source.waveform.colors);
    overlayMap(destination.waveform.metrics, source.waveform.metrics);
#define OVERLAY_VISUAL(name)                                                      \
    overlayMap(destination.name.colors, source.name.colors);                      \
    overlayMap(destination.name.metrics, source.name.metrics)
    OVERLAY_VISUAL(nodeGraph);
    OVERLAY_VISUAL(blueprint);
    OVERLAY_VISUAL(timeline);
    OVERLAY_VISUAL(music);
    OVERLAY_VISUAL(log);
    OVERLAY_VISUAL(monitor);
    OVERLAY_VISUAL(ai);
    OVERLAY_VISUAL(scene3d);
    OVERLAY_VISUAL(spatial);
    OVERLAY_VISUAL(feedback);
#undef OVERLAY_VISUAL
    overlayMap(destination.presenters.contexts, source.presenters.contexts);
    for (const ThemeComponentRecipeDto& sourceRecipe : source.recipes.recipes)
    {
        auto destinationIt = std::find_if(
            destination.recipes.recipes.begin(),
            destination.recipes.recipes.end(),
            [&sourceRecipe](const ThemeComponentRecipeDto& candidate) {
                return candidate.host == sourceRecipe.host
                    && candidate.breakpoint == sourceRecipe.breakpoint;
            });
        if (destinationIt == destination.recipes.recipes.end())
            destination.recipes.recipes.append(sourceRecipe);
        else
            *destinationIt = sourceRecipe;
    }
    overlayMap(destination.assets.assets, source.assets.assets);
}

struct Version
{
    int major = 0;
    int minor = 0;
    int patch = 0;
};

std::optional<Version> parseVersion(QString text)
{
    const qsizetype suffix = text.indexOf(QLatin1Char('-'));
    if (suffix >= 0)
        text.truncate(suffix);
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.isEmpty() || parts.size() > 3)
        return std::nullopt;
    Version version;
    int* fields[] = {&version.major, &version.minor, &version.patch};
    for (qsizetype index = 0; index < parts.size(); ++index)
    {
        bool ok = false;
        const int value = parts.at(index).toInt(&ok);
        if (!ok || value < 0)
            return std::nullopt;
        *fields[index] = value;
    }
    return version;
}

bool isStableId(const QString& value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"));
    return pattern.match(value).hasMatch();
}

int compareVersion(const Version& left, const Version& right)
{
    if (left.major != right.major)
        return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor)
        return left.minor < right.minor ? -1 : 1;
    if (left.patch != right.patch)
        return left.patch < right.patch ? -1 : 1;
    return 0;
}

bool versionMatches(const QString& range, const QString& current)
{
    if (range.trimmed() == QStringLiteral("*"))
        return true;
    const auto actual = parseVersion(current);
    if (!actual)
        return false;
    const QStringList clauses = range.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (clauses.isEmpty())
        return false;
    for (QString clause : clauses)
    {
        QString operation = QStringLiteral("=");
        if (clause.startsWith(QStringLiteral(">="))
            || clause.startsWith(QStringLiteral("<=")))
        {
            operation = clause.left(2);
            clause.remove(0, 2);
        }
        else if (clause.startsWith(QLatin1Char('>'))
                 || clause.startsWith(QLatin1Char('<'))
                 || clause.startsWith(QLatin1Char('=')))
        {
            operation = clause.left(1);
            clause.remove(0, 1);
        }
        const auto expected = parseVersion(clause);
        if (!expected)
            return false;
        const int comparison = compareVersion(*actual, *expected);
        if ((operation == QStringLiteral("=") && comparison != 0)
            || (operation == QStringLiteral(">=") && comparison < 0)
            || (operation == QStringLiteral("<=") && comparison > 0)
            || (operation == QStringLiteral(">") && comparison <= 0)
            || (operation == QStringLiteral("<") && comparison >= 0))
            return false;
    }
    return true;
}

bool compatible(const DiscoveredPackage& package,
                const ThemePackageLoadOptions& options,
                QList<ThemePackageDiagnostic>& diagnostics)
{
    if (package.manifest.compatibility.harkenVersionRange
        && !versionMatches(*package.manifest.compatibility.harkenVersionRange,
                           options.harkenVersion))
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::UnsupportedCompatibility,
            package.displayRoot + QStringLiteral("/manifest.json"),
            QStringLiteral("$.compatibility.harkenVersion"),
            QStringLiteral("Theme does not support this Harken version.")));
        return false;
    }
    for (qsizetype index = 0;
         index < package.manifest.compatibility.capabilities.size(); ++index)
    {
        const QString& capability =
            package.manifest.compatibility.capabilities.at(index);
        if (!options.supportedCapabilities.contains(capability))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::UnsupportedCompatibility,
                package.displayRoot + QStringLiteral("/manifest.json"),
                QStringLiteral("$.compatibility.capabilities[")
                    + QString::number(index) + QLatin1Char(']'),
                QStringLiteral("Theme requires an unsupported capability.")));
            return false;
        }
    }
    return true;
}

template<typename Dto>
bool appendParsed(const ThemePackageParseResult<Dto>& parsed,
                  Dto& output,
                  QList<ThemePackageDiagnostic>& diagnostics)
{
    if (!parsed.ok())
    {
        diagnostics.append(parsed.diagnostics);
        return false;
    }
    output = *parsed.value;
    return true;
}

std::optional<SemanticDocuments> loadDocuments(
    const DiscoveredPackage& package,
    const ThemePackageLoadOptions& options,
    qsizetype& totalBytes,
    QList<ThemePackageDiagnostic>& diagnostics)
{
    SemanticDocuments documents;
    const auto load = [&](const QString& relative,
                          const QString& fieldPath) -> std::optional<QByteArray> {
        const QString manifestFile =
            package.displayRoot + QStringLiteral("/manifest.json");
        const auto path = constrainedPath(package.root, relative, true,
                                          manifestFile, fieldPath, diagnostics);
        if (!path)
            return std::nullopt;
        return readJsonFile(*path, package.displayRoot + QLatin1Char('/')
                                      + QDir::fromNativeSeparators(relative),
                            options.limits, totalBytes, diagnostics);
    };

    const auto tokens = load(package.manifest.entries.tokens,
                             QStringLiteral("$.entries.tokens"));
    const auto curve = load(package.manifest.entries.visuals.curve,
                            QStringLiteral("$.entries.visuals.curve"));
    const auto meter = load(package.manifest.entries.visuals.meter,
                            QStringLiteral("$.entries.visuals.meter"));
    const auto waveform = load(package.manifest.entries.visuals.waveform,
                               QStringLiteral("$.entries.visuals.waveform"));
    QMap<QString, QByteArray> domainVisuals;
    const auto loadDomainVisual = [&](const QString& name,
                                      const std::optional<QString>& relative) {
        if (!relative)
            return true;
        const auto bytes = load(*relative, QStringLiteral("$.entries.visuals.")
                                               + name);
        if (!bytes)
            return false;
        domainVisuals.insert(name, *bytes);
        return true;
    };
    const bool domainVisualsLoaded =
        loadDomainVisual(QStringLiteral("nodeGraph"),
                         package.manifest.entries.visuals.nodeGraph)
        && loadDomainVisual(QStringLiteral("blueprint"),
                            package.manifest.entries.visuals.blueprint)
        && loadDomainVisual(QStringLiteral("timeline"),
                            package.manifest.entries.visuals.timeline)
        && loadDomainVisual(QStringLiteral("music"),
                            package.manifest.entries.visuals.music)
        && loadDomainVisual(QStringLiteral("log"),
                            package.manifest.entries.visuals.log)
        && loadDomainVisual(QStringLiteral("monitor"),
                            package.manifest.entries.visuals.monitor)
        && loadDomainVisual(QStringLiteral("ai"),
                            package.manifest.entries.visuals.ai)
        && loadDomainVisual(QStringLiteral("scene3d"),
                            package.manifest.entries.visuals.scene3d)
        && loadDomainVisual(QStringLiteral("spatial"),
                            package.manifest.entries.visuals.spatial)
        && loadDomainVisual(QStringLiteral("feedback"),
                            package.manifest.entries.visuals.feedback);
    const auto assets = load(package.manifest.entries.assetMap,
                             QStringLiteral("$.entries.assetMap"));
    std::optional<QByteArray> presenters;
    if (package.manifest.entries.presenters)
    {
        presenters = load(*package.manifest.entries.presenters,
                          QStringLiteral("$.entries.presenters"));
    }
    std::optional<QByteArray> recipes;
    if (package.manifest.entries.recipes)
    {
        recipes = load(*package.manifest.entries.recipes,
                       QStringLiteral("$.entries.recipes"));
    }
    if (!tokens || !curve || !meter || !waveform || !domainVisualsLoaded
        || !assets
        || (package.manifest.entries.presenters && !presenters)
        || (package.manifest.entries.recipes && !recipes))
        return std::nullopt;

    const QString prefix = package.displayRoot + QLatin1Char('/');
    if (!appendParsed(parseThemeTokens(*tokens, prefix
                                      + package.manifest.entries.tokens),
                      documents.tokens, diagnostics)
        || !appendParsed(parseThemeVisual(
                             *curve, prefix
                                 + package.manifest.entries.visuals.curve),
                         documents.curve, diagnostics)
        || !appendParsed(parseThemeVisual(
                             *meter, prefix
                                 + package.manifest.entries.visuals.meter),
                         documents.meter, diagnostics)
        || !appendParsed(parseThemeVisual(
                             *waveform, prefix
                                 + package.manifest.entries.visuals.waveform),
                         documents.waveform, diagnostics))
        return std::nullopt;

    const auto parseDomainVisual = [&](const QString& name,
                                       const std::optional<QString>& relative,
                                       ThemeVisualDto& output) {
        if (!relative)
            return true;
        return appendParsed(
            parseThemeVisual(domainVisuals.value(name), prefix + *relative),
            output, diagnostics);
    };
    if (!parseDomainVisual(QStringLiteral("nodeGraph"),
                           package.manifest.entries.visuals.nodeGraph,
                           documents.nodeGraph)
        || !parseDomainVisual(QStringLiteral("blueprint"),
                              package.manifest.entries.visuals.blueprint,
                              documents.blueprint)
        || !parseDomainVisual(QStringLiteral("timeline"),
                              package.manifest.entries.visuals.timeline,
                              documents.timeline)
        || !parseDomainVisual(QStringLiteral("music"),
                              package.manifest.entries.visuals.music,
                              documents.music)
        || !parseDomainVisual(QStringLiteral("log"),
                              package.manifest.entries.visuals.log,
                              documents.log)
        || !parseDomainVisual(QStringLiteral("monitor"),
                              package.manifest.entries.visuals.monitor,
                              documents.monitor)
        || !parseDomainVisual(QStringLiteral("ai"),
                              package.manifest.entries.visuals.ai,
                              documents.ai)
        || !parseDomainVisual(QStringLiteral("scene3d"),
                              package.manifest.entries.visuals.scene3d,
                              documents.scene3d)
        || !parseDomainVisual(QStringLiteral("spatial"),
                              package.manifest.entries.visuals.spatial,
                              documents.spatial)
        || !parseDomainVisual(QStringLiteral("feedback"),
                              package.manifest.entries.visuals.feedback,
                              documents.feedback)
        || (presenters
            && !appendParsed(parseThemePresenters(
                                 *presenters, prefix
                                     + *package.manifest.entries.presenters),
                             documents.presenters, diagnostics))
        || (recipes
            && !appendParsed(parseThemeRecipes(
                                 *recipes, prefix
                                     + *package.manifest.entries.recipes),
                             documents.recipes, diagnostics))
        || !appendParsed(parseThemeAssetMap(
                             *assets, prefix
                                 + package.manifest.entries.assetMap),
                         documents.assets, diagnostics))
        return std::nullopt;

    for (auto assetIt = documents.assets.assets.begin();
         assetIt != documents.assets.assets.end(); ++assetIt)
    {
        if (!isStableId(assetIt.key()))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::InvalidValue,
                prefix + package.manifest.entries.assetMap,
                QStringLiteral("$.assets.") + assetIt.key(),
                QStringLiteral("Logical asset ID is not stable.")));
            return std::nullopt;
        }
        for (qsizetype index = 0; index < assetIt->sources.size(); ++index)
        {
            auto& source = assetIt->sources[index];
            const QString path = QStringLiteral("$.assets.") + assetIt.key()
                + QStringLiteral(".sources[") + QString::number(index)
                + QStringLiteral("].path");
            const auto normalized = constrainedPath(
                package.root, source.path, false,
                prefix + package.manifest.entries.assetMap, path, diagnostics);
            if (!normalized)
                return std::nullopt;
            source.path = package.manifest.id + QLatin1Char('/') + *normalized;
        }
    }
    return documents;
}

template<typename T>
bool assignNumber(T& destination, double value)
{
    if (!std::isfinite(value) || value < -4096.0 || value > 4096.0)
        return false;
    if constexpr (std::is_integral_v<T>)
    {
        if (std::floor(value) != value)
            return false;
    }
    destination = static_cast<T>(value);
    return true;
}

bool applyTokenColor(const QString& key,
                     const QColor& value,
                     ThemeSnapshot& theme)
{
    auto& colors = theme.ui.colors;
    if (key == QStringLiteral("background.primary"))
    {
        colors.bgPrimary = value;
        theme.qtPalette.window = value;
        return true;
    }
    if (key == QStringLiteral("background.secondary"))
    {
        colors.bgSecondary = value;
        theme.qtPalette.base = value;
        theme.qtPalette.toolTipBase = value;
        return true;
    }
    if (key == QStringLiteral("background.tertiary"))
    {
        colors.bgTertiary = value;
        theme.qtPalette.alternateBase = value;
        theme.qtPalette.button = value;
        return true;
    }
    if (key == QStringLiteral("text.secondary"))
    {
        colors.textSecondary = value;
        theme.qtPalette.mid = value;
        return true;
    }
    if (key == QStringLiteral("text.muted"))
    {
        colors.textMuted = value;
        theme.qtPalette.dark = value;
        return true;
    }
#define TOKEN_COLOR(name, field)                                                \
    if (key == QStringLiteral(name))                                            \
    {                                                                           \
        colors.field = value;                                                   \
        return true;                                                            \
    }
    TOKEN_COLOR("accent.primary", accent)
    TOKEN_COLOR("accent.hover", accentHover)
    TOKEN_COLOR("accent.text", accentText)
    TOKEN_COLOR("text.primary", textPrimary)
    TOKEN_COLOR("text.disabled", textDisabled)
    TOKEN_COLOR("palette.categorical0", categoricalPalette[0])
    TOKEN_COLOR("palette.categorical1", categoricalPalette[1])
    TOKEN_COLOR("palette.categorical2", categoricalPalette[2])
    TOKEN_COLOR("palette.categorical3", categoricalPalette[3])
    TOKEN_COLOR("palette.categorical4", categoricalPalette[4])
    TOKEN_COLOR("palette.categorical5", categoricalPalette[5])
    TOKEN_COLOR("palette.categorical6", categoricalPalette[6])
    TOKEN_COLOR("background.hover", bgHover)
    TOKEN_COLOR("background.selected", bgSelected)
    TOKEN_COLOR("border.primary", borderPrimary)
    TOKEN_COLOR("border.secondary", borderSecondary)
    TOKEN_COLOR("border.view", viewBorder)
    TOKEN_COLOR("status.success", success)
    TOKEN_COLOR("status.warning", warning)
    TOKEN_COLOR("status.error", error)
    TOKEN_COLOR("status.info", info)
    TOKEN_COLOR("track.background", trackBackground)
    TOKEN_COLOR("track.backgroundAlternate", trackBackgroundAlt)
    TOKEN_COLOR("bus.background", busBackground)
    TOKEN_COLOR("bus.backgroundActive", busBackgroundActive)
    TOKEN_COLOR("slider.track", sliderTrack)
    TOKEN_COLOR("knob.indicator", knobIndicator)
    TOKEN_COLOR("panel.background", panelBackground)
    TOKEN_COLOR("section.separator", sectionSeparator)
    TOKEN_COLOR("header.background", headerBackground)
    TOKEN_COLOR("audition.background", auditionBg)
    TOKEN_COLOR("audition.playActive", auditionPlayActive)
    TOKEN_COLOR("audition.stopActive", auditionStopActive)
    TOKEN_COLOR("shell.menuText", shellMenuText)
    TOKEN_COLOR("shell.menuHoverText", shellMenuHoverText)
    TOKEN_COLOR("shell.buttonText", shellButtonText)
    TOKEN_COLOR("shell.buttonHoverText", shellButtonHoverText)
    TOKEN_COLOR("shell.closeHover", shellCloseHover)
    TOKEN_COLOR("shell.closePressed", shellClosePressed)
#undef TOKEN_COLOR
    return false;
}

bool applyTokenMetric(const QString& key, double value, ThemeSnapshot& theme)
{
    auto& metrics = theme.ui.metrics;
#define TOKEN_METRIC(name, field)                                               \
    if (key == QStringLiteral(name))                                            \
        return value >= 0.0 && assignNumber(metrics.field, value)
    TOKEN_METRIC("radius.small", radiusSmall);
    TOKEN_METRIC("radius.medium", radiusMedium);
    TOKEN_METRIC("radius.large", radiusLarge);
    TOKEN_METRIC("spacing.xSmall", spacingXSmall);
    TOKEN_METRIC("spacing.small", spacingSmall);
    TOKEN_METRIC("spacing.medium", spacingMedium);
    TOKEN_METRIC("spacing.large", spacingLarge);
    TOKEN_METRIC("control.height", controlHeight);
    TOKEN_METRIC("section.headerHeight", sectionHeaderHeight);
    if (key == QStringLiteral("navigation.chevronExtent"))
        return value >= 1.0 && assignNumber(metrics.navigationChevronExtent, value);
    if (key == QStringLiteral("object.iconExtent"))
        return value >= 1.0 && assignNumber(metrics.objectIconExtent, value);
    TOKEN_METRIC("inspector.pageMargin", inspector.pageMargin);
    TOKEN_METRIC("inspector.sectionSpacing", inspector.sectionSpacing);
    TOKEN_METRIC("inspector.rowSpacing", inspector.rowSpacing);
    TOKEN_METRIC("inspector.columnPadding", inspector.columnPadding);
    TOKEN_METRIC("table.headerHeight", table.headerHeight);
    TOKEN_METRIC("table.rowHeight", table.rowHeight);
    TOKEN_METRIC("table.footerHeight", table.footerHeight);
    TOKEN_METRIC("table.cellPaddingX", table.cellPaddingX);
    TOKEN_METRIC("table.cellPaddingY", table.cellPaddingY);
    TOKEN_METRIC("table.buttonWidth", table.buttonWidth);
    TOKEN_METRIC("table.buttonHeight", table.buttonHeight);
    TOKEN_METRIC("table.buttonSpacing", table.buttonSpacing);
    TOKEN_METRIC("table.compactColumnWidth", table.compactColumnWidth);
    TOKEN_METRIC("table.minimumColumnWidth", table.minimumColumnWidth);
    TOKEN_METRIC("table.resizeHandleRadius", table.resizeHandleRadius);
    TOKEN_METRIC("inspector.rowHeight", inspector.rowHeight);
    TOKEN_METRIC("inspector.labelColumnShare", inspector.labelColumnShare);
    TOKEN_METRIC("inspector.valueColumnShare", inspector.valueColumnShare);
    TOKEN_METRIC("inspector.formLabelWidth", inspector.formLabelWidth);
    TOKEN_METRIC("inspector.valueColumnWidth", inspector.valueColumnWidth);
    TOKEN_METRIC("inspector.linkSlotWidth", inspector.linkSlotWidth);
    TOKEN_METRIC("inspector.sourceSlotWidth", inspector.sourceSlotWidth);
    TOKEN_METRIC("inspector.platformSlotWidth", inspector.platformSlotWidth);
    TOKEN_METRIC("inspector.bindingSlotWidth", inspector.bindingSlotWidth);
    TOKEN_METRIC("inspector.slotHeight", inspector.slotHeight);
    TOKEN_METRIC("inspector.slotSpacing", inspector.slotSpacing);
    TOKEN_METRIC("inspector.separatorHeight", inspector.separatorHeight);
    TOKEN_METRIC("inspector.rangeSeparatorWidth", inspector.rangeSeparatorWidth);
    TOKEN_METRIC("parameter.knobExtent", parameter.knobExtent);
    TOKEN_METRIC("parameter.faderMinimumHeight", parameter.faderMinimumHeight);
    TOKEN_METRIC("parameter.labelMinimumWidth", parameter.labelMinimumWidth);
    TOKEN_METRIC("parameter.valueMinimumWidth", parameter.valueMinimumWidth);
    TOKEN_METRIC("parameter.compactValueMinimumWidth",
                 parameter.compactValueMinimumWidth);
    TOKEN_METRIC("parameter.sliderGrooveThickness",
                 parameter.sliderGrooveThickness);
    TOKEN_METRIC("parameter.sliderHandleThickness",
                 parameter.sliderHandleThickness);
    TOKEN_METRIC("parameter.sliderHandleLength", parameter.sliderHandleLength);
    TOKEN_METRIC("timeline.laneHeaderWidth", timeline.laneHeaderWidth);
    TOKEN_METRIC("timeline.laneHeaderMinimumWidth", timeline.laneHeaderMinimumWidth);
    TOKEN_METRIC("timeline.laneHeight", timeline.laneHeight);
    TOKEN_METRIC("timeline.blankRowHeight", timeline.blankRowHeight);
    TOKEN_METRIC("timeline.laneSpacing", timeline.laneSpacing);
    TOKEN_METRIC("timeline.rulerHeight", timeline.rulerHeight);
    TOKEN_METRIC("timeline.markerLaneHeight", timeline.markerLaneHeight);
    TOKEN_METRIC("timeline.tickLength", timeline.tickLength);
    TOKEN_METRIC("timeline.majorTickLength", timeline.majorTickLength);
    TOKEN_METRIC("timeline.minLabelSpacing", timeline.minLabelSpacing);
    TOKEN_METRIC("timeline.minTickSpacing", timeline.minTickSpacing);
    TOKEN_METRIC("timeline.clipVerticalInset", timeline.clipVerticalInset);
    TOKEN_METRIC("timeline.clipRadius", timeline.clipRadius);
    TOKEN_METRIC("timeline.clipMinimumWidth", timeline.clipMinimumWidth);
    TOKEN_METRIC("timeline.resizeHandleWidth", timeline.resizeHandleWidth);
    TOKEN_METRIC("timeline.markerHitRadius", timeline.markerHitRadius);
    TOKEN_METRIC("timeline.statusBarHeight", timeline.statusBarHeight);
    TOKEN_METRIC("timeline.compactHeaderWidth", timeline.compactHeaderWidth);
    TOKEN_METRIC("timeline.queueLaneLabelWidth", timeline.queueLaneLabelWidth);
    TOKEN_METRIC("timeline.headerGripWidth", timeline.headerGripWidth);
#undef TOKEN_METRIC
    return false;
}

ThemeFontStyle* fontRole(const QString& key, ThemeSnapshot& theme)
{
    auto& typography = theme.ui.typography;
    if (key == QStringLiteral("heading"))
        return &typography.heading;
    if (key == QStringLiteral("title"))
        return &typography.title;
    if (key == QStringLiteral("body"))
        return &typography.body;
    if (key == QStringLiteral("caption"))
        return &typography.caption;
    if (key == QStringLiteral("axis"))
        return &typography.axis;
    if (key == QStringLiteral("mono"))
        return &typography.mono;
    if (key == QStringLiteral("label"))
        return &typography.label;
    return nullptr;
}

bool applyCurveColor(const QString& key,
                     const QColor& value,
                     ThemeSnapshot& theme)
{
    auto& colors = theme.domains.curve.colors;
#define CURVE_COLOR(name, field)                                                \
    if (key == QStringLiteral("curve." name))                                  \
    {                                                                           \
        colors.field = value;                                                   \
        return true;                                                            \
    }
    CURVE_COLOR("line", attenuationVolume)
    CURVE_COLOR("attenuationVolume", attenuationVolume)
    CURVE_COLOR("attenuationPitch", attenuationPitch)
    CURVE_COLOR("parameterBinding", parameterBinding)
    CURVE_COLOR("gridFade", gridFade)
    CURVE_COLOR("ghost", ghost)
    CURVE_COLOR("lassoBorder", lassoBorder)
    CURVE_COLOR("lassoFill", lassoFill)
    CURVE_COLOR("crosshair", crosshair)
    CURVE_COLOR("playhead", playhead)
#undef CURVE_COLOR
    return false;
}

bool applyCurveMetric(const QString& key, double value, ThemeSnapshot& theme)
{
    auto& metrics = theme.domains.curve.metrics;
#define CURVE_METRIC(name, field)                                               \
    if (key == QStringLiteral("curve." name))                                  \
        return assignNumber(metrics.field, value)
    CURVE_METRIC("linearGraphLeftInset", linearGraphLeftInset);
    CURVE_METRIC("amplitudeGraphLeftInset", amplitudeGraphLeftInset);
    CURVE_METRIC("graphTopInset", graphTopInset);
    CURVE_METRIC("graphRightInset", graphRightInset);
    CURVE_METRIC("graphBottomInset", graphBottomInset);
    CURVE_METRIC("gridColumnCount", gridColumnCount);
    CURVE_METRIC("linearGridRowCount", linearGridRowCount);
    CURVE_METRIC("amplitudeGridTargetRowSpacing", amplitudeGridTargetRowSpacing);
    CURVE_METRIC("amplitudeGridMinimumRows", amplitudeGridMinimumRows);
    CURVE_METRIC("amplitudeGridMaximumRows", amplitudeGridMaximumRows);
    CURVE_METRIC("gridLineWidth", gridLineWidth);
    CURVE_METRIC("plotBorderWidth", plotBorderWidth);
    CURVE_METRIC("lineWidth", curveLineWidth);
    CURVE_METRIC("curveLineWidth", curveLineWidth);
    CURVE_METRIC("discreteGuideLineWidth", discreteGuideLineWidth);
    CURVE_METRIC("discreteGuideAlpha", discreteGuideAlpha);
    CURVE_METRIC("ghostLineWidth", ghostLineWidth);
    CURVE_METRIC("lassoBorderWidth", lassoBorderWidth);
    CURVE_METRIC("handleLineWidth", handleLineWidth);
    CURVE_METRIC("handleBorderWidth", handleBorderWidth);
    CURVE_METRIC("pointBorderWidth", pointBorderWidth);
    CURVE_METRIC("crosshairLineWidth", crosshairLineWidth);
    CURVE_METRIC("ghostRadius", ghostRadius);
    CURVE_METRIC("handleRadius", handleRadius);
    CURVE_METRIC("pointRadius", pointRadius);
    CURVE_METRIC("pointSelectedRadius", pointSelectedRadius);
    CURVE_METRIC("crosshairDashLength", crosshairDashLength);
    CURVE_METRIC("crosshairDashGap", crosshairDashGap);
    CURVE_METRIC("xAxisLabelTopOffset", xAxisLabelTopOffset);
    CURVE_METRIC("xAxisLabelHeight", xAxisLabelHeight);
    CURVE_METRIC("yAxisLabelCenterX", yAxisLabelCenterX);
    CURVE_METRIC("yAxisLabelWidth", yAxisLabelWidth);
    CURVE_METRIC("yAxisLabelHeight", yAxisLabelHeight);
    CURVE_METRIC("amplitudeTickLabelLeft", amplitudeTickLabelLeft);
    CURVE_METRIC("amplitudeTickLabelRightGap", amplitudeTickLabelRightGap);
    CURVE_METRIC("amplitudeTickLabelHalfHeight", amplitudeTickLabelHalfHeight);
    CURVE_METRIC("amplitudeTickLabelHeight", amplitudeTickLabelHeight);
    CURVE_METRIC("linearYTickLeftOffset", linearYTickLeftOffset);
    CURVE_METRIC("linearYTickBaselineOffset", linearYTickBaselineOffset);
    CURVE_METRIC("xMinTickLeftOffset", xMinTickLeftOffset);
    CURVE_METRIC("xTickBaselineOffset", xTickBaselineOffset);
    CURVE_METRIC("xMaxLabelWidth", xMaxLabelWidth);
    CURVE_METRIC("xMaxLabelTopOffset", xMaxLabelTopOffset);
    CURVE_METRIC("xMaxLabelHeight", xMaxLabelHeight);
    CURVE_METRIC("xMaxEditorVerticalExpansion", xMaxEditorVerticalExpansion);
    CURVE_METRIC("readoutFontPixelSize", readoutFontPixelSize);
    CURVE_METRIC("readoutHorizontalPadding", readoutHorizontalPadding);
    CURVE_METRIC("readoutHeight", readoutHeight);
    CURVE_METRIC("readoutHorizontalOffset", readoutHorizontalOffset);
    CURVE_METRIC("readoutAboveOffset", readoutAboveOffset);
    CURVE_METRIC("readoutBelowOffset", readoutBelowOffset);
    CURVE_METRIC("readoutRadius", readoutRadius);
#undef CURVE_METRIC
    return false;
}

bool applyWaveformColor(const QString& key,
                        const QColor& value,
                        ThemeSnapshot& theme)
{
    auto& waveform = theme.domains.waveform;
#define WAVEFORM_COLOR(name, field)                                             \
    if (key == QStringLiteral("waveform." name))                               \
    {                                                                           \
        waveform.field = value;                                                 \
        return true;                                                            \
    }
    WAVEFORM_COLOR("line", line)
    WAVEFORM_COLOR("fill", fill)
    WAVEFORM_COLOR("selectionHighlight", selectionHighlight)
    WAVEFORM_COLOR("cursor", cursor)
    WAVEFORM_COLOR("regionMarker", regionMarker)
    WAVEFORM_COLOR("trimHandle", trimHandle)
    WAVEFORM_COLOR("loopHandle", loopHandle)
    WAVEFORM_COLOR("static.quiet", staticMode.quiet)
    WAVEFORM_COLOR("static.peak", staticMode.peak)
    WAVEFORM_COLOR("splitStereo.divider", splitStereo.divider)
    WAVEFORM_COLOR("splitStereo.label", splitStereo.label)
    WAVEFORM_COLOR("rawPeaks.background", rawPeaks.background)
    WAVEFORM_COLOR("rawPeaks.centerLine", rawPeaks.centerLine)
    if (key.startsWith(QStringLiteral("waveform.multiBand.color")))
    {
        bool ok = false;
        const int index = key.sliced(24).toInt(&ok);
        if (ok && index >= 0 && index < 3)
        {
            waveform.multiBandMode.colors[static_cast<size_t>(index)] = value;
            return true;
        }
    }
#undef WAVEFORM_COLOR
    return false;
}

bool applyWaveformMetric(const QString& key,
                         double value,
                         ThemeSnapshot& theme)
{
    auto& waveform = theme.domains.waveform;
#define WAVEFORM_METRIC(name, field)                                            \
    if (key == QStringLiteral("waveform." name))                               \
        return assignNumber(waveform.field, value)
    WAVEFORM_METRIC("static.accentQuietAlpha", staticMode.accentQuietAlpha);
    WAVEFORM_METRIC("static.accentPeakAlpha", staticMode.accentPeakAlpha);
    WAVEFORM_METRIC("multiBand.alpha", multiBandMode.alpha);
    WAVEFORM_METRIC("splitStereo.dividerWidth", splitStereo.dividerWidth);
    WAVEFORM_METRIC("splitStereo.labelPointSize", splitStereo.labelPointSize);
    WAVEFORM_METRIC("splitStereo.labelRightInset", splitStereo.labelRightInset);
    WAVEFORM_METRIC("splitStereo.labelBaselineOffset", splitStereo.labelBaselineOffset);
    WAVEFORM_METRIC("rawPeaks.cornerRadius", rawPeaks.cornerRadius);
    WAVEFORM_METRIC("rawPeaks.centerLineWidth", rawPeaks.centerLineWidth);
    WAVEFORM_METRIC("dbScale.gridWidth", dbScale.gridWidth);
    WAVEFORM_METRIC("dbScale.labelPointSize", dbScale.labelPointSize);
    WAVEFORM_METRIC("dbScale.amplitudeScale", dbScale.amplitudeScale);
    WAVEFORM_METRIC("dbScale.labelLeftInset", dbScale.labelLeftInset);
    WAVEFORM_METRIC("dbScale.labelBaselineOffset", dbScale.labelBaselineOffset);
    WAVEFORM_METRIC("amplitudeScale", amplitudeScale);
#undef WAVEFORM_METRIC
    const struct ArrayBinding
    {
        QString prefix;
        float* values;
        int count;
    } arrays[] = {
        {QStringLiteral("waveform.multiBand.gain"),
         waveform.multiBandMode.gains.data(), 3},
        {QStringLiteral("waveform.colorMap.offset"),
         waveform.colorMapMode.offset.data(), 4},
        {QStringLiteral("waveform.colorMap.slope"),
         waveform.colorMapMode.slope.data(), 4}};
    for (const auto& binding : arrays)
    {
        if (!key.startsWith(binding.prefix))
            continue;
        bool ok = false;
        const int index = key.sliced(binding.prefix.size()).toInt(&ok);
        if (ok && index >= 0 && index < binding.count)
            return assignNumber(binding.values[index], value);
    }
    return false;
}

bool applyMeterColor(const QString& key,
                     const QColor& value,
                     ThemeSnapshot& theme)
{
    auto& meter = theme.domains.meter;
#define METER_COLOR(name, field)                                                \
    if (key == QStringLiteral("meter." name))                                  \
    {                                                                           \
        meter.field = value;                                                    \
        return true;                                                            \
    }
    METER_COLOR("green", green)
    METER_COLOR("yellow", yellow)
    METER_COLOR("red", red)
    METER_COLOR("peak", peak)
    METER_COLOR("rms", rms)
    METER_COLOR("clipOn", clipOn)
    METER_COLOR("clipOff", clipOff)
    METER_COLOR("background", background)
    METER_COLOR("lufsMomentary", lufsMomentary)
    METER_COLOR("lufsShortTerm", lufsShortTerm)
    METER_COLOR("lufsIntegrated", lufsIntegrated)
    METER_COLOR("interaction.accentGuide", interaction.colors.accentGuide)
    METER_COLOR("peakMeter.gradientError", peakMeter.colors.gradientError)
    METER_COLOR("peakMeter.gradientWarning", peakMeter.colors.gradientWarning)
    METER_COLOR("peakMeter.gradientYellow", peakMeter.colors.gradientYellow)
    METER_COLOR("peakMeter.gradientAccent", peakMeter.colors.gradientAccent)
    METER_COLOR("peakMeter.gradientGreen", peakMeter.colors.gradientAccent)
    METER_COLOR("peakMeter.rmsOverlay", peakMeter.colors.rmsOverlay)
    METER_COLOR("peakMeter.holdHalo", peakMeter.colors.holdHalo)
    METER_COLOR("peakMeter.holdLine", peakMeter.colors.holdLine)
    METER_COLOR("peakMeter.clipBackground", peakMeter.colors.clipBackground)
    METER_COLOR("peakMeter.clipText", peakMeter.colors.clipText)
    METER_COLOR("loudness.targetLine", loudnessMeter.colors.targetLine)
    METER_COLOR("vu.analogPositiveTick", vuMeter.colors.analogPositiveTick)
    METER_COLOR("vu.analogOverloadArc", vuMeter.colors.analogOverloadArc)
    METER_COLOR("vu.analogNeedleGlow", vuMeter.colors.analogNeedleGlow)
    METER_COLOR("vu.analogNeedle", vuMeter.colors.analogNeedle)
    METER_COLOR("vu.analogPivot", vuMeter.colors.analogPivot)
    METER_COLOR("vu.barWarning", vuMeter.colors.barWarning)
    METER_COLOR("spectrum.fillTop", spectrumMeter.colors.fillTop)
    METER_COLOR("spectrum.fillUpperMiddle", spectrumMeter.colors.fillUpperMiddle)
    METER_COLOR("spectrum.fillLowerMiddle", spectrumMeter.colors.fillLowerMiddle)
    METER_COLOR("spectrum.fillBottom", spectrumMeter.colors.fillBottom)
    METER_COLOR("spectrum.lineGlowWide", spectrumMeter.colors.lineGlowWide)
    METER_COLOR("spectrum.lineGlowMedium", spectrumMeter.colors.lineGlowMedium)
    METER_COLOR("spectrum.lineGlowNarrow", spectrumMeter.colors.lineGlowNarrow)
    METER_COLOR("spectrum.lineHighlight", spectrumMeter.colors.lineHighlight)
    METER_COLOR("spectrum.peakLine", spectrumMeter.colors.peakLine)
    METER_COLOR("spectrogram.emptyHistory", spectrogramMeter.colors.emptyHistory)
#undef METER_COLOR
    return false;
}

bool applyMeterMetric(const QString& key, double value, ThemeSnapshot& theme)
{
    auto& meter = theme.domains.meter;
#define METER_METRIC(name, field)                                               \
    if (key == QStringLiteral("meter." name))                                  \
        return assignNumber(meter.field, value)
    METER_METRIC("interaction.crosshairLineWidth", interaction.metrics.crosshairLineWidth);
    METER_METRIC("interaction.accentGuideLineWidth", interaction.metrics.accentGuideLineWidth);
    METER_METRIC("interaction.horizontalPadding", interaction.metrics.horizontalPadding);
    METER_METRIC("interaction.lineHeight", interaction.metrics.lineHeight);
    METER_METRIC("interaction.extraHeight", interaction.metrics.extraHeight);
    METER_METRIC("interaction.horizontalOffset", interaction.metrics.horizontalOffset);
    METER_METRIC("interaction.aboveGap", interaction.metrics.aboveGap);
    METER_METRIC("interaction.belowOffset", interaction.metrics.belowOffset);
    METER_METRIC("interaction.radius", interaction.metrics.radius);
    METER_METRIC("peakMeter.outerInsetLeft", peakMeter.metrics.outerInsetLeft);
    METER_METRIC("peakMeter.outerInsetTop", peakMeter.metrics.outerInsetTop);
    METER_METRIC("peakMeter.outerInsetRight", peakMeter.metrics.outerInsetRight);
    METER_METRIC("peakMeter.outerInsetBottom", peakMeter.metrics.outerInsetBottom);
    METER_METRIC("peakMeter.channelGap", peakMeter.metrics.channelGap);
    METER_METRIC("peakMeter.reservedWidth", peakMeter.metrics.reservedWidth);
    METER_METRIC("peakMeter.barHorizontalInset", peakMeter.metrics.barHorizontalInset);
    METER_METRIC("peakMeter.gradientErrorStop", peakMeter.metrics.gradientErrorStop);
    METER_METRIC("peakMeter.gradientWarningStop", peakMeter.metrics.gradientWarningStop);
    METER_METRIC("peakMeter.gradientYellowStop", peakMeter.metrics.gradientYellowStop);
    METER_METRIC("peakMeter.gradientAccentStop", peakMeter.metrics.gradientAccentStop);
    METER_METRIC("peakMeter.gradientGreenStop", peakMeter.metrics.gradientAccentStop);
    METER_METRIC("peakMeter.rmsLeftInsetRatio", peakMeter.metrics.rmsLeftInsetRatio);
    METER_METRIC("peakMeter.rmsWidthRatio", peakMeter.metrics.rmsWidthRatio);
    METER_METRIC("peakMeter.holdHaloTopOffset", peakMeter.metrics.holdHaloTopOffset);
    METER_METRIC("peakMeter.holdHaloHeight", peakMeter.metrics.holdHaloHeight);
    METER_METRIC("peakMeter.holdLineTopOffset", peakMeter.metrics.holdLineTopOffset);
    METER_METRIC("peakMeter.holdLineHeight", peakMeter.metrics.holdLineHeight);
    METER_METRIC("peakMeter.gridLineWidth", peakMeter.metrics.gridLineWidth);
    METER_METRIC("peakMeter.tickLeftOffset", peakMeter.metrics.tickLeftOffset);
    METER_METRIC("peakMeter.tickWidth", peakMeter.metrics.tickWidth);
    METER_METRIC("peakMeter.tickHalfHeight", peakMeter.metrics.tickHalfHeight);
    METER_METRIC("peakMeter.tickHeight", peakMeter.metrics.tickHeight);
    METER_METRIC("peakMeter.clipHeight", peakMeter.metrics.clipHeight);
    METER_METRIC("peakMeter.channelLabelBottomOffset", peakMeter.metrics.channelLabelBottomOffset);
    METER_METRIC("peakMeter.valueLabelBottomOffset", peakMeter.metrics.valueLabelBottomOffset);
    METER_METRIC("peakMeter.labelHeight", peakMeter.metrics.labelHeight);
    METER_METRIC("loudness.outerInsetLeft", loudnessMeter.metrics.outerInsetLeft);
    METER_METRIC("loudness.outerInsetTop", loudnessMeter.metrics.outerInsetTop);
    METER_METRIC("loudness.outerInsetRight", loudnessMeter.metrics.outerInsetRight);
    METER_METRIC("loudness.outerInsetBottom", loudnessMeter.metrics.outerInsetBottom);
    METER_METRIC("loudness.reservedWidth", loudnessMeter.metrics.reservedWidth);
    METER_METRIC("loudness.barGap", loudnessMeter.metrics.barGap);
    METER_METRIC("loudness.minimumBarWidth", loudnessMeter.metrics.minimumBarWidth);
    METER_METRIC("loudness.gradientTopStop", loudnessMeter.metrics.gradientTopStop);
    METER_METRIC("loudness.gradientBottomStop", loudnessMeter.metrics.gradientBottomStop);
    METER_METRIC("loudness.gradientTopLighterFactor", loudnessMeter.metrics.gradientTopLighterFactor);
    METER_METRIC("loudness.gradientBottomDarkerFactor", loudnessMeter.metrics.gradientBottomDarkerFactor);
    METER_METRIC("loudness.highlightLighterFactor", loudnessMeter.metrics.highlightLighterFactor);
    METER_METRIC("loudness.highlightAlpha", loudnessMeter.metrics.highlightAlpha);
    METER_METRIC("loudness.highlightMinimumBarHeight", loudnessMeter.metrics.highlightMinimumBarHeight);
    METER_METRIC("loudness.highlightHeight", loudnessMeter.metrics.highlightHeight);
    METER_METRIC("loudness.targetLineWidth", loudnessMeter.metrics.targetLineWidth);
    METER_METRIC("loudness.targetLabelHorizontalInset", loudnessMeter.metrics.targetLabelHorizontalInset);
    METER_METRIC("loudness.targetLabelTopOffset", loudnessMeter.metrics.targetLabelTopOffset);
    METER_METRIC("loudness.targetLabelHeight", loudnessMeter.metrics.targetLabelHeight);
    METER_METRIC("loudness.channelLabelHeight", loudnessMeter.metrics.channelLabelHeight);
    METER_METRIC("loudness.valueLabelBottomOffset", loudnessMeter.metrics.valueLabelBottomOffset);
    METER_METRIC("loudness.valueLabelHeight", loudnessMeter.metrics.valueLabelHeight);
    METER_METRIC("vu.outerInsetLeft", vuMeter.metrics.outerInsetLeft);
    METER_METRIC("vu.outerInsetTop", vuMeter.metrics.outerInsetTop);
    METER_METRIC("vu.outerInsetRight", vuMeter.metrics.outerInsetRight);
    METER_METRIC("vu.outerInsetBottom", vuMeter.metrics.outerInsetBottom);
    METER_METRIC("vu.titleHeight", vuMeter.metrics.titleHeight);
    METER_METRIC("vu.contentTopInset", vuMeter.metrics.contentTopInset);
    METER_METRIC("vu.analogGap", vuMeter.metrics.analogGap);
    METER_METRIC("vu.analogCornerRadius", vuMeter.metrics.analogCornerRadius);
    METER_METRIC("vu.analogInnerInsetLeft", vuMeter.metrics.analogInnerInsetLeft);
    METER_METRIC("vu.analogInnerInsetTop", vuMeter.metrics.analogInnerInsetTop);
    METER_METRIC("vu.analogInnerInsetRight", vuMeter.metrics.analogInnerInsetRight);
    METER_METRIC("vu.analogInnerInsetBottom", vuMeter.metrics.analogInnerInsetBottom);
    METER_METRIC("vu.analogLabelStripBottomOffset", vuMeter.metrics.analogLabelStripBottomOffset);
    METER_METRIC("vu.analogLabelStripHeight", vuMeter.metrics.analogLabelStripHeight);
    METER_METRIC("vu.analogPivotBottomOffset", vuMeter.metrics.analogPivotBottomOffset);
    METER_METRIC("vu.analogRadiusScale", vuMeter.metrics.analogRadiusScale);
    METER_METRIC("vu.analogArcStartPiFactor", vuMeter.metrics.analogArcStartPiFactor);
    METER_METRIC("vu.analogArcEndPiFactor", vuMeter.metrics.analogArcEndPiFactor);
    METER_METRIC("vu.analogArcSegments", vuMeter.metrics.analogArcSegments);
    METER_METRIC("vu.analogArcLineWidth", vuMeter.metrics.analogArcLineWidth);
    METER_METRIC("vu.analogTickInnerRadiusScale", vuMeter.metrics.analogTickInnerRadiusScale);
    METER_METRIC("vu.analogTickOuterRadiusScale", vuMeter.metrics.analogTickOuterRadiusScale);
    METER_METRIC("vu.analogTickLineWidth", vuMeter.metrics.analogTickLineWidth);
    METER_METRIC("vu.analogTickLabelRadiusScale", vuMeter.metrics.analogTickLabelRadiusScale);
    METER_METRIC("vu.analogTickLabelHalfWidth", vuMeter.metrics.analogTickLabelHalfWidth);
    METER_METRIC("vu.analogTickLabelTopOffset", vuMeter.metrics.analogTickLabelTopOffset);
    METER_METRIC("vu.analogTickLabelWidth", vuMeter.metrics.analogTickLabelWidth);
    METER_METRIC("vu.analogTickLabelHeight", vuMeter.metrics.analogTickLabelHeight);
    METER_METRIC("vu.analogOverloadRadiusScale", vuMeter.metrics.analogOverloadRadiusScale);
    METER_METRIC("vu.analogOverloadSegments", vuMeter.metrics.analogOverloadSegments);
    METER_METRIC("vu.analogOverloadLineWidth", vuMeter.metrics.analogOverloadLineWidth);
    METER_METRIC("vu.analogNeedleLengthScale", vuMeter.metrics.analogNeedleLengthScale);
    METER_METRIC("vu.analogNeedleGlowWidth", vuMeter.metrics.analogNeedleGlowWidth);
    METER_METRIC("vu.analogNeedleWidth", vuMeter.metrics.analogNeedleWidth);
    METER_METRIC("vu.analogPivotRadius", vuMeter.metrics.analogPivotRadius);
    METER_METRIC("vu.analogValueHeight", vuMeter.metrics.analogValueHeight);
    METER_METRIC("vu.analogLabelTopOffset", vuMeter.metrics.analogLabelTopOffset);
    METER_METRIC("vu.analogLabelHeight", vuMeter.metrics.analogLabelHeight);
    METER_METRIC("vu.barGap", vuMeter.metrics.barGap);
    METER_METRIC("vu.barCornerRadius", vuMeter.metrics.barCornerRadius);
    METER_METRIC("vu.barHorizontalInset", vuMeter.metrics.barHorizontalInset);
    METER_METRIC("vu.barOverloadStop", vuMeter.metrics.barOverloadStop);
    METER_METRIC("vu.barWarningStop", vuMeter.metrics.barWarningStop);
    METER_METRIC("vu.barYellowStop", vuMeter.metrics.barYellowStop);
    METER_METRIC("vu.barBrightGreenStop", vuMeter.metrics.barBrightGreenStop);
    METER_METRIC("vu.barGreenStop", vuMeter.metrics.barGreenStop);
    METER_METRIC("vu.barOverloadAlpha", vuMeter.metrics.barOverloadAlpha);
    METER_METRIC("vu.barGreenLighterFactor", vuMeter.metrics.barGreenLighterFactor);
    METER_METRIC("vu.barGridLineWidth", vuMeter.metrics.barGridLineWidth);
    METER_METRIC("vu.barChannelLabelBottomOffset", vuMeter.metrics.barChannelLabelBottomOffset);
    METER_METRIC("vu.barChannelLabelHeight", vuMeter.metrics.barChannelLabelHeight);
    METER_METRIC("vu.barValueLabelBottomOffset", vuMeter.metrics.barValueLabelBottomOffset);
    METER_METRIC("vu.barValueLabelHeight", vuMeter.metrics.barValueLabelHeight);
    METER_METRIC("spectrum.plotInsetLeft", spectrumMeter.metrics.plotInsetLeft);
    METER_METRIC("spectrum.plotInsetTop", spectrumMeter.metrics.plotInsetTop);
    METER_METRIC("spectrum.plotInsetRight", spectrumMeter.metrics.plotInsetRight);
    METER_METRIC("spectrum.plotInsetBottom", spectrumMeter.metrics.plotInsetBottom);
    METER_METRIC("spectrum.fillTopStop", spectrumMeter.metrics.fillTopStop);
    METER_METRIC("spectrum.fillUpperMiddleStop", spectrumMeter.metrics.fillUpperMiddleStop);
    METER_METRIC("spectrum.fillLowerMiddleStop", spectrumMeter.metrics.fillLowerMiddleStop);
    METER_METRIC("spectrum.fillBottomStop", spectrumMeter.metrics.fillBottomStop);
    METER_METRIC("spectrum.curveControlRatio", spectrumMeter.metrics.curveControlRatio);
    METER_METRIC("spectrum.lineGlowWideWidth", spectrumMeter.metrics.lineGlowWideWidth);
    METER_METRIC("spectrum.lineGlowMediumWidth", spectrumMeter.metrics.lineGlowMediumWidth);
    METER_METRIC("spectrum.lineGlowNarrowWidth", spectrumMeter.metrics.lineGlowNarrowWidth);
    METER_METRIC("spectrum.lineHighlightWidth", spectrumMeter.metrics.lineHighlightWidth);
    METER_METRIC("spectrum.peakLineWidth", spectrumMeter.metrics.peakLineWidth);
    METER_METRIC("spectrum.gridLineWidth", spectrumMeter.metrics.gridLineWidth);
    METER_METRIC("spectrum.dbLabelLeft", spectrumMeter.metrics.dbLabelLeft);
    METER_METRIC("spectrum.dbLabelHalfHeight", spectrumMeter.metrics.dbLabelHalfHeight);
    METER_METRIC("spectrum.dbLabelWidth", spectrumMeter.metrics.dbLabelWidth);
    METER_METRIC("spectrum.dbLabelHeight", spectrumMeter.metrics.dbLabelHeight);
    METER_METRIC("spectrum.frequencyLabelHalfWidth", spectrumMeter.metrics.frequencyLabelHalfWidth);
    METER_METRIC("spectrum.frequencyLabelTopOffset", spectrumMeter.metrics.frequencyLabelTopOffset);
    METER_METRIC("spectrum.frequencyLabelWidth", spectrumMeter.metrics.frequencyLabelWidth);
    METER_METRIC("spectrum.frequencyLabelHeight", spectrumMeter.metrics.frequencyLabelHeight);
    METER_METRIC("spectrogram.plotInsetLeft", spectrogramMeter.metrics.plotInsetLeft);
    METER_METRIC("spectrogram.plotInsetTop", spectrogramMeter.metrics.plotInsetTop);
    METER_METRIC("spectrogram.plotInsetRight", spectrogramMeter.metrics.plotInsetRight);
    METER_METRIC("spectrogram.plotInsetBottom", spectrogramMeter.metrics.plotInsetBottom);
    METER_METRIC("spectrogram.gridLineWidth", spectrogramMeter.metrics.gridLineWidth);
    METER_METRIC("spectrogram.frequencyLabelLeft", spectrogramMeter.metrics.frequencyLabelLeft);
    METER_METRIC("spectrogram.frequencyLabelHalfHeight", spectrogramMeter.metrics.frequencyLabelHalfHeight);
    METER_METRIC("spectrogram.frequencyLabelWidth", spectrogramMeter.metrics.frequencyLabelWidth);
    METER_METRIC("spectrogram.frequencyLabelHeight", spectrogramMeter.metrics.frequencyLabelHeight);
    METER_METRIC("spectrogram.timeLabelTopOffset", spectrogramMeter.metrics.timeLabelTopOffset);
    METER_METRIC("spectrogram.timeLabelHeight", spectrogramMeter.metrics.timeLabelHeight);
#undef METER_METRIC
    return false;
}

bool applyDomainColor(const QString& key,
                      const QColor& value,
                      ThemeSnapshot& theme)
{
#define DOMAIN_COLOR(name, field)                                                \
    if (key == QStringLiteral(name))                                              \
    {                                                                             \
        theme.domains.field = value;                                              \
        return true;                                                              \
    }
    DOMAIN_COLOR("startup.wave.shadow", startup.wavePalette[0])
    DOMAIN_COLOR("startup.wave.low", startup.wavePalette[1])
    DOMAIN_COLOR("startup.wave.high", startup.wavePalette[2])
    DOMAIN_COLOR("startup.wave.highlight", startup.wavePalette[3])

    DOMAIN_COLOR("feedback.overlayBackground", feedback.overlayBackground)
    DOMAIN_COLOR("feedback.overlayErrorBackground", feedback.overlayErrorBackground)

    DOMAIN_COLOR("nodeGraph.pin.audio", pinDomain.audio)
    DOMAIN_COLOR("nodeGraph.pin.control", pinDomain.control)
    DOMAIN_COLOR("nodeGraph.pin.value", pinDomain.value)
    DOMAIN_COLOR("nodeGraph.pin.ring", pinDomain.ring)
    DOMAIN_COLOR("nodeGraph.pin.targetOk", pinDomain.targetOk)
    DOMAIN_COLOR("nodeGraph.pin.targetBad", pinDomain.targetBad)
    DOMAIN_COLOR("nodeGraph.node.borderNormal", nodeGraphNode.borderNormal)
    DOMAIN_COLOR("nodeGraph.node.borderHoverMix", nodeGraphNode.borderHoverMix)
    DOMAIN_COLOR("nodeGraph.node.dropTarget", nodeGraphNode.dropTarget)
    DOMAIN_COLOR("nodeGraph.node.shadow", nodeGraphNode.shadow)
    DOMAIN_COLOR("nodeGraph.node.sandboxBadgeBg", nodeGraphNode.sandboxBadgeBg)
    DOMAIN_COLOR("nodeGraph.node.sandboxBadgeText", nodeGraphNode.sandboxBadgeText)
    DOMAIN_COLOR("nodeGraph.node.badgeOverride", nodeGraphNode.badgeOverride)
    DOMAIN_COLOR("nodeGraph.node.badgeChild", nodeGraphNode.badgeChild)
    DOMAIN_COLOR("nodeGraph.node.badgeReceive", nodeGraphNode.badgeReceive)
    DOMAIN_COLOR("nodeGraph.node.waveformBg", nodeGraphNode.waveformBg)
    DOMAIN_COLOR("nodeGraph.node.waveformCenter", nodeGraphNode.waveformCenter)
    DOMAIN_COLOR("nodeGraph.node.blueprintWaveform", nodeGraphNode.blueprintWaveform)

    DOMAIN_COLOR("blueprint.canvasBackground", blueprint.canvasBackground)
    DOMAIN_COLOR("blueprint.gridDot", blueprint.gridDot)
    DOMAIN_COLOR("blueprint.edgeLabelBg", blueprint.edgeLabelBg)
    DOMAIN_COLOR("blueprint.placementChain", blueprint.placementChain)
    DOMAIN_COLOR("blueprint.organizationChain", blueprint.organizationChain)
    DOMAIN_COLOR("blueprint.containment", blueprint.containment)
    DOMAIN_COLOR("blueprint.busRoute", blueprint.busRoute)
    DOMAIN_COLOR("blueprint.send", blueprint.send)
    DOMAIN_COLOR("blueprint.overrideRoute", blueprint.overrideRoute)

    DOMAIN_COLOR("timeline.entry", timeline.entry)
    DOMAIN_COLOR("timeline.exit", timeline.exit)
    DOMAIN_COLOR("timeline.transition", timeline.transition)
    DOMAIN_COLOR("timeline.clip", timeline.clip)
    DOMAIN_COLOR("timeline.clipSelected", timeline.clipSelected)
    DOMAIN_COLOR("timeline.clipBorder", timeline.clipBorder)
    DOMAIN_COLOR("timeline.clipSelectedBorder", timeline.clipSelectedBorder)
    DOMAIN_COLOR("timeline.clipText", timeline.clipText)
    DOMAIN_COLOR("timeline.badge", timeline.badge)
    DOMAIN_COLOR("timeline.emergenceMarker", timeline.emergenceMarker)
    DOMAIN_COLOR("timeline.unavailableOverlay", timeline.unavailableOverlay)
    DOMAIN_COLOR("timeline.unavailableText", timeline.unavailableText)
    DOMAIN_COLOR("timeline.outOfSpanVeil", timeline.outOfSpanVeil)

    DOMAIN_COLOR("music.beatGridMajor", music.beatGridMajor)
    DOMAIN_COLOR("music.beatGridMinor", music.beatGridMinor)
    DOMAIN_COLOR("music.cursor", music.cursor)
    DOMAIN_COLOR("music.loopRegion", music.loopRegion)
    if (key.startsWith(QStringLiteral("music.trackColor")))
    {
        bool ok = false;
        const int index = key.sliced(16).toInt(&ok);
        if (ok && index >= 0 && index < DomainColors::Music::kTrackColorCount)
        {
            theme.domains.music.trackColors[static_cast<size_t>(index)] = value;
            return true;
        }
    }

    DOMAIN_COLOR("log.debug", log.debug)
    DOMAIN_COLOR("log.info", log.info)
    DOMAIN_COLOR("log.warning", log.warning)
    DOMAIN_COLOR("log.error", log.error)
    DOMAIN_COLOR("log.critical", log.critical)

    DOMAIN_COLOR("monitor.stateStolen", monitor.stateStolen)
    DOMAIN_COLOR("monitor.statePlaying", monitor.statePlaying)
    DOMAIN_COLOR("monitor.stateStarting", monitor.stateStarting)
    DOMAIN_COLOR("monitor.stateVirtual", monitor.stateVirtual)
    DOMAIN_COLOR("monitor.stateReleasing", monitor.stateReleasing)
    DOMAIN_COLOR("monitor.stateStopping", monitor.stateStopping)
    DOMAIN_COLOR("monitor.statePaused", monitor.statePaused)
    DOMAIN_COLOR("monitor.stateDefault", monitor.stateDefault)
    DOMAIN_COLOR("monitor.virtualDim", monitor.virtualDim)
    DOMAIN_COLOR("monitor.mutedText", monitor.mutedText)
    DOMAIN_COLOR("monitor.lowVolume", monitor.lowVolume)
    DOMAIN_COLOR("monitor.busIdText", monitor.busIdText)
    DOMAIN_COLOR("monitor.peakClip", monitor.peakClip)
    DOMAIN_COLOR("monitor.peakCaution", monitor.peakCaution)
    DOMAIN_COLOR("monitor.peakSafe", monitor.peakSafe)
    DOMAIN_COLOR("monitor.pitchHigh", monitor.pitchHigh)
    DOMAIN_COLOR("monitor.pitchLow", monitor.pitchLow)
    DOMAIN_COLOR("monitor.cpuCritical", monitor.cpuCritical)
    DOMAIN_COLOR("monitor.cpuWarning", monitor.cpuWarning)
    DOMAIN_COLOR("monitor.cpuSafe", monitor.cpuSafe)
    DOMAIN_COLOR("monitor.bufferWarning", monitor.bufferWarning)
    DOMAIN_COLOR("monitor.bufferNormal", monitor.bufferNormal)
    DOMAIN_COLOR("monitor.underrunFlash", monitor.underrunFlash)
    DOMAIN_COLOR("monitor.underrunIdle", monitor.underrunIdle)
    DOMAIN_COLOR("monitor.underrunNone", monitor.underrunNone)
    DOMAIN_COLOR("monitor.spatialGainHigh", monitor.spatialGainHigh)
    DOMAIN_COLOR("monitor.spatialGainLow", monitor.spatialGainLow)
    DOMAIN_COLOR("monitor.spatialLpf", monitor.spatialLpf)
    DOMAIN_COLOR("monitor.spatialWidth", monitor.spatialWidth)
    DOMAIN_COLOR("monitor.spatialDistance", monitor.spatialDistance)
    DOMAIN_COLOR("monitor.spatialReverb", monitor.spatialReverb)
    DOMAIN_COLOR("monitor.spatialDopplerHi", monitor.spatialDopplerHi)
    DOMAIN_COLOR("monitor.spatialDopplerLo", monitor.spatialDopplerLo)
    DOMAIN_COLOR("monitor.spatialDot", monitor.spatialDot)

    DOMAIN_COLOR("ai.brandAccent", ai.brandAccent)
    DOMAIN_COLOR("ai.userAccent", ai.userAccent)

    DOMAIN_COLOR("scene3d.listener", scene3d.listener)
    DOMAIN_COLOR("scene3d.listenerCone", scene3d.listenerCone)
    DOMAIN_COLOR("scene3d.room", scene3d.room)
    DOMAIN_COLOR("scene3d.emitterMin", scene3d.emitterMin)
    DOMAIN_COLOR("scene3d.emitterMax", scene3d.emitterMax)
    DOMAIN_COLOR("scene3d.hudBackground", scene3d.hudBackground)
    DOMAIN_COLOR("scene3d.hudTitle", scene3d.hudTitle)
    DOMAIN_COLOR("scene3d.hudLabel", scene3d.hudLabel)
    DOMAIN_COLOR("scene3d.hudGain", scene3d.hudGain)
    DOMAIN_COLOR("scene3d.hudGainLow", scene3d.hudGainLow)
    DOMAIN_COLOR("scene3d.hudLpf", scene3d.hudLpf)
    DOMAIN_COLOR("scene3d.hudWidth", scene3d.hudWidth)
    DOMAIN_COLOR("scene3d.dopplerIdle", scene3d.dopplerIdle)

    DOMAIN_COLOR("spatial.background", spatial.background)
    DOMAIN_COLOR("spatial.grid", spatial.grid)
    DOMAIN_COLOR("spatial.axis", spatial.axis)
    DOMAIN_COLOR("spatial.axisLabel", spatial.axisLabel)
    DOMAIN_COLOR("spatial.tangentLine", spatial.tangentLine)
    DOMAIN_COLOR("spatial.tangentHover", spatial.tangentHover)
    DOMAIN_COLOR("spatial.tangentActive", spatial.tangentActive)
    DOMAIN_COLOR("spatial.pointSelected", spatial.pointSelected)
    DOMAIN_COLOR("spatial.previewMarker", spatial.previewMarker)
    DOMAIN_COLOR("spatial.infoText", spatial.infoText)
    DOMAIN_COLOR("spatial.legendBg", spatial.legendBg)
    DOMAIN_COLOR("spatial.layerVolume", spatial.layerVolume)
    DOMAIN_COLOR("spatial.layerLpf", spatial.layerLpf)
    DOMAIN_COLOR("spatial.layerWidth", spatial.layerWidth)
    DOMAIN_COLOR("spatial.layerReverb", spatial.layerReverb)
#undef DOMAIN_COLOR
    return false;
}

bool applyNoDomainMetric(const QString&, double, ThemeSnapshot&)
{
    return false;
}

using ColorApplier = bool (*)(const QString&, const QColor&, ThemeSnapshot&);
using MetricApplier = bool (*)(const QString&, double, ThemeSnapshot&);

bool applyDomainColorForPrefix(const QString& prefix,
                               const QString& key,
                               const QColor& value,
                               ThemeSnapshot& theme)
{
    if (!key.startsWith(prefix + QLatin1Char('.')))
        return false;
    return applyDomainColor(key, value, theme);
}

#define DOMAIN_COLOR_APPLIER(name, Name)                                         \
    bool apply##Name##Color(const QString& key, const QColor& value,              \
                            ThemeSnapshot& theme)                                 \
    {                                                                             \
        return applyDomainColorForPrefix(QStringLiteral(#name), key, value,       \
                                         theme);                                  \
    }
DOMAIN_COLOR_APPLIER(nodeGraph, NodeGraph)
DOMAIN_COLOR_APPLIER(blueprint, Blueprint)
DOMAIN_COLOR_APPLIER(timeline, Timeline)
DOMAIN_COLOR_APPLIER(music, Music)
DOMAIN_COLOR_APPLIER(log, Log)
DOMAIN_COLOR_APPLIER(monitor, Monitor)
DOMAIN_COLOR_APPLIER(ai, Ai)
DOMAIN_COLOR_APPLIER(scene3d, Scene3d)
DOMAIN_COLOR_APPLIER(spatial, Spatial)
DOMAIN_COLOR_APPLIER(feedback, Feedback)
#undef DOMAIN_COLOR_APPLIER

bool metricValueWithinBounds(const QString& key, double value)
{
    if (!std::isfinite(value) || value < -4096.0 || value > 4096.0)
        return false;
    const QString lower = key.toLower();
    if (lower.endsWith(QStringLiteral("stop")))
        return value >= 0.0 && value <= 1.0;
    if (lower.endsWith(QStringLiteral("ratio"))
        || lower.endsWith(QStringLiteral("scale")))
        return value >= 0.0 && value <= 4.0;
    if (lower.endsWith(QStringLiteral("alpha")))
        return value >= 0.0
            && value <= (lower.contains(QStringLiteral("multiband")) ? 1.0
                                                                      : 255.0);
    const QStringList nonNegativeTerms{
        QStringLiteral("width"), QStringLiteral("height"),
        QStringLiteral("radius"), QStringLiteral("count"),
        QStringLiteral("segments"), QStringLiteral("padding"),
        QStringLiteral("gap"), QStringLiteral("inset")};
    for (const QString& term : nonNegativeTerms)
    {
        if (lower.contains(term) && value < 0.0)
            return false;
    }
    return true;
}

bool applySemanticDocument(const ThemeVisualDto& document,
                           const QString& file,
                           ColorApplier colorApplier,
                           MetricApplier metricApplier,
                           ThemeSnapshot& theme,
                           QList<ThemePackageDiagnostic>& diagnostics)
{
    for (auto it = document.colors.constBegin(); it != document.colors.constEnd(); ++it)
    {
        const QColor color(it.value());
        const QString path = QStringLiteral("$.colors.") + it.key();
        if (!color.isValid())
        {
            diagnostics.append(diagnostic(ThemePackageErrorCode::InvalidColor,
                                          file, path,
                                          QStringLiteral("Invalid color value.")));
            return false;
        }
        if (!colorApplier(it.key(), color, theme))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::UnknownSemanticKey, file, path,
                QStringLiteral("Unknown visual color key.")));
            return false;
        }
    }
    for (auto it = document.metrics.constBegin(); it != document.metrics.constEnd(); ++it)
    {
        const QString path = QStringLiteral("$.metrics.") + it.key();
        ThemeSnapshot probe = theme;
        if (!metricApplier(it.key(), 1.0, probe))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::UnknownSemanticKey, file, path,
                QStringLiteral("Unknown visual metric key.")));
            return false;
        }
        if (!metricValueWithinBounds(it.key(), it.value())
            || !metricApplier(it.key(), it.value(), theme))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::MetricOutOfRange, file, path,
                QStringLiteral("Visual metric value is out of range.")));
            return false;
        }
    }
    return true;
}

bool applyPresenterSelections(
    const ThemePresenterMapDto& presenters,
    const DiscoveredPackage& package,
    ThemeSnapshot& theme,
    QList<ThemePackageDiagnostic>& diagnostics)
{
    if (presenters.contexts.isEmpty())
        return true;
    const QString file = package.displayRoot + QLatin1Char('/')
        + package.manifest.entries.presenters.value_or(
            QStringLiteral("presenters.json"));
    for (auto it = presenters.contexts.constBegin();
         it != presenters.contexts.constEnd(); ++it)
    {
        const QString path = QStringLiteral("$.contexts.") + it.key();
        const auto context = parseParameterPresenterContext(it.key());
        if (!context)
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::UnknownSemanticKey, file, path,
                QStringLiteral("Unknown parameter presenter context.")));
            return false;
        }
        const auto presenter = parseParameterPresenterId(it.value());
        if (!presenter)
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::InvalidValue, file, path,
                QStringLiteral("Unknown built-in parameter presenter ID.")));
            return false;
        }
        if (!parameterPresenterSupportsContext(*presenter, *context))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::InvalidValue, file, path,
                QStringLiteral("Presenter does not support this context.")));
            return false;
        }
        auto& selection = theme.ui.parameterPresenters;
        switch (*context)
        {
            case ParameterPresenterContext::InspectorFloat:
                selection.inspectorFloat = *presenter;
                break;
            case ParameterPresenterContext::EffectParameter:
                selection.effectParameter = *presenter;
                break;
        }
    }
    return true;
}

bool applyRecipeSelections(
    const ThemeRecipeMapDto& recipes,
    const DiscoveredPackage& package,
    const ThemeAssetMapDto& mergedAssets,
    ThemeSnapshot& theme,
    QList<ThemePackageDiagnostic>& diagnostics)
{
    if (recipes.recipes.isEmpty())
        return true;
    const QString file = package.displayRoot + QLatin1Char('/')
        + package.manifest.entries.recipes.value_or(
            QStringLiteral("recipes.json"));
    QSet<QString> keys;
    for (qsizetype recipeIndex = 0; recipeIndex < recipes.recipes.size();
         ++recipeIndex)
    {
        const ThemeComponentRecipeDto& source = recipes.recipes.at(recipeIndex);
        const QString recipePath = QStringLiteral("$.recipes[")
            + QString::number(recipeIndex) + QLatin1Char(']');
        const auto host = parseComponentRecipeHost(source.host);
        if (!host)
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::UnknownRecipeHost, file,
                recipePath + QStringLiteral(".host"),
                QStringLiteral("Unknown component recipe host.")));
            return false;
        }
        const auto breakpoint = parseComponentRecipeBreakpoint(
            source.breakpoint);
        if (!breakpoint)
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::InvalidValue, file,
                recipePath + QStringLiteral(".breakpoint"),
                QStringLiteral("Unsupported component recipe breakpoint.")));
            return false;
        }
        const auto variant = parseComponentRecipeVariant(source.variant);
        if (!variant || !componentRecipeVariantSupportsHost(*variant, *host))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::UnsupportedRecipeVariant, file,
                recipePath + QStringLiteral(".variant"),
                QStringLiteral("Variant does not support this component host.")));
            return false;
        }
        const QString key = componentRecipeKey(*host, *breakpoint);
        if (keys.contains(key))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::DuplicateRecipe, file, recipePath,
                QStringLiteral("Host and breakpoint recipe is duplicated.")));
            return false;
        }
        keys.insert(key);

        ComponentRecipe recipe;
        recipe.host = *host;
        recipe.breakpoint = *breakpoint;
        recipe.variant = *variant;
        QSet<ComponentRecipeSlotId> seenSlots;
        for (qsizetype slotIndex = 0; slotIndex < source.items.size();
             ++slotIndex)
        {
            const ThemeRecipeSlotDto& sourceSlot = source.items.at(slotIndex);
            const QString slotPath = recipePath + QStringLiteral(".slots[")
                + QString::number(slotIndex) + QLatin1Char(']');
            const auto slotId = parseComponentRecipeSlot(sourceSlot.id);
            if (!slotId || !componentRecipeHostSupportsSlot(*host, *slotId))
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::UnknownRecipeSlot, file,
                    slotPath + QStringLiteral(".id"),
                    QStringLiteral("Slot is not public for this component host.")));
                return false;
            }
            if (seenSlots.contains(*slotId))
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::DuplicateRecipeSlot, file,
                    slotPath + QStringLiteral(".id"),
                    QStringLiteral("Recipe slot is duplicated.")));
                return false;
            }
            seenSlots.insert(*slotId);

            const int minimum = sourceSlot.minimum.value_or(-1);
            const int preferred = sourceSlot.preferred.value_or(-1);
            const int maximum = sourceSlot.maximum.value_or(-1);
            if (minimum < -1 || preferred < -1 || maximum < -1
                || minimum > 4096 || preferred > 4096 || maximum > 4096
                || (minimum >= 0 && preferred >= 0 && minimum > preferred)
                || (preferred >= 0 && maximum >= 0 && preferred > maximum)
                || (minimum >= 0 && maximum >= 0 && minimum > maximum))
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::RecipeSizeConflict, file, slotPath,
                    QStringLiteral("Slot size constraints conflict.")));
                return false;
            }
            const auto alignment = parseComponentRecipeAlignment(
                sourceSlot.alignment);
            if (!alignment)
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::InvalidValue, file,
                    slotPath + QStringLiteral(".alignment"),
                    QStringLiteral("Unsupported slot alignment.")));
                return false;
            }

            ComponentRecipeSlot slot;
            slot.id = *slotId;
            slot.visible = sourceSlot.visible;
            slot.minimum = minimum;
            slot.preferred = preferred;
            slot.maximum = maximum;
            slot.alignment = *alignment;
            if (sourceSlot.presenter)
            {
                const auto context = componentRecipePresenterContext(
                    *host, *slotId, *variant);
                const auto presenter = parseParameterPresenterId(
                    *sourceSlot.presenter);
                if (!context || !presenter
                    || !parameterPresenterSupportsContext(*presenter, *context))
                {
                    diagnostics.append(diagnostic(
                        ThemePackageErrorCode::UnsupportedRecipePresenter, file,
                        slotPath + QStringLiteral(".presenter"),
                        QStringLiteral("Presenter does not support this recipe slot.")));
                    return false;
                }
                slot.presenter = *presenter;
            }
            if (sourceSlot.asset)
            {
                if (!isStableId(*sourceSlot.asset)
                    || !mergedAssets.assets.contains(*sourceSlot.asset))
                {
                    diagnostics.append(diagnostic(
                        ThemePackageErrorCode::AssetNotFound, file,
                        slotPath + QStringLiteral(".asset"),
                        QStringLiteral("Recipe asset ID is not present in the merged asset map.")));
                    return false;
                }
                slot.asset = *sourceSlot.asset;
            }
            recipe.items.append(std::move(slot));
        }
        theme.ui.recipes.entries.insert(key, std::move(recipe));
    }
    return true;
}

bool applySemantics(const SemanticDocuments& documents,
                    const DiscoveredPackage& package,
                    const ThemeAssetMapDto& mergedAssets,
                    ThemeSnapshot& theme,
                    QList<ThemePackageDiagnostic>& diagnostics)
{
    const QString prefix = package.displayRoot + QLatin1Char('/');
    const QString tokenFile = prefix + package.manifest.entries.tokens;
    for (auto it = documents.tokens.colors.constBegin();
         it != documents.tokens.colors.constEnd(); ++it)
    {
        const QColor color(it.value());
        const QString path = QStringLiteral("$.colors.") + it.key();
        if (!color.isValid())
        {
            diagnostics.append(diagnostic(ThemePackageErrorCode::InvalidColor,
                                          tokenFile, path,
                                          QStringLiteral("Invalid color value.")));
            return false;
        }
        const bool applied = it.key().startsWith(QStringLiteral("startup."))
            ? applyDomainColor(it.key(), color, theme)
            : applyTokenColor(it.key(), color, theme);
        if (!applied)
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::UnknownSemanticKey, tokenFile, path,
                QStringLiteral("Unknown token color key.")));
            return false;
        }
    }
    for (auto it = documents.tokens.metrics.constBegin();
         it != documents.tokens.metrics.constEnd(); ++it)
    {
        const QString path = QStringLiteral("$.metrics.") + it.key();
        ThemeSnapshot probe = theme;
        if (!applyTokenMetric(it.key(), 1.0, probe))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::UnknownSemanticKey, tokenFile, path,
                QStringLiteral("Unknown token metric key.")));
            return false;
        }
        if (!metricValueWithinBounds(it.key(), it.value())
            || !applyTokenMetric(it.key(), it.value(), theme))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::MetricOutOfRange, tokenFile, path,
                QStringLiteral("Token metric value is out of range.")));
            return false;
        }
    }
    for (auto it = documents.tokens.fonts.constBegin();
         it != documents.tokens.fonts.constEnd(); ++it)
    {
        ThemeFontStyle* role = fontRole(it.key(), theme);
        const QString path = QStringLiteral("$.fonts.") + it.key();
        if (!role)
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::UnknownSemanticKey, tokenFile, path,
                QStringLiteral("Unknown font role.")));
            return false;
        }
        if (it->family)
        {
            if (it->family->size() > 128)
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::InvalidValue, tokenFile,
                    path + QStringLiteral(".family"),
                    QStringLiteral("Font family is too long.")));
                return false;
            }
            role->family = *it->family;
        }
        if (it->pointSize)
        {
            if (*it->pointSize < 6 || *it->pointSize > 72)
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::MetricOutOfRange, tokenFile,
                    path + QStringLiteral(".pointSize"),
                    QStringLiteral("Font point size is out of range.")));
                return false;
            }
            role->pointSize = *it->pointSize;
        }
        if (it->weight)
        {
            if (*it->weight < 1 || *it->weight > 1000)
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::MetricOutOfRange, tokenFile,
                    path + QStringLiteral(".weight"),
                    QStringLiteral("Font weight is out of range.")));
                return false;
            }
            role->weight = *it->weight;
        }
    }

    ThemeVisualDto curve = documents.curve;
    ThemeVisualDto meter = documents.meter;
    ThemeVisualDto waveform = documents.waveform;
    const auto applyDomainVisual = [&](const ThemeVisualDto& document,
                                       const std::optional<QString>& relative,
                                       ColorApplier colorApplier) {
        return !relative
            || applySemanticDocument(document, prefix + *relative,
                                     colorApplier, applyNoDomainMetric,
                                     theme, diagnostics);
    };

    if (!applySemanticDocument(curve,
                               prefix + package.manifest.entries.visuals.curve,
                               applyCurveColor, applyCurveMetric, theme, diagnostics)
        || !applySemanticDocument(meter,
                                  prefix + package.manifest.entries.visuals.meter,
                                  applyMeterColor, applyMeterMetric, theme, diagnostics)
        || !applySemanticDocument(
            waveform, prefix + package.manifest.entries.visuals.waveform,
            applyWaveformColor, applyWaveformMetric, theme, diagnostics)
        || !applyDomainVisual(documents.nodeGraph,
                              package.manifest.entries.visuals.nodeGraph,
                              applyNodeGraphColor)
        || !applyDomainVisual(documents.blueprint,
                              package.manifest.entries.visuals.blueprint,
                              applyBlueprintColor)
        || !applyDomainVisual(documents.timeline,
                              package.manifest.entries.visuals.timeline,
                              applyTimelineColor)
        || !applyDomainVisual(documents.music,
                              package.manifest.entries.visuals.music,
                              applyMusicColor)
        || !applyDomainVisual(documents.log,
                              package.manifest.entries.visuals.log,
                              applyLogColor)
        || !applyDomainVisual(documents.monitor,
                              package.manifest.entries.visuals.monitor,
                              applyMonitorColor)
        || !applyDomainVisual(documents.ai,
                              package.manifest.entries.visuals.ai,
                              applyAiColor)
        || !applyDomainVisual(documents.scene3d,
                              package.manifest.entries.visuals.scene3d,
                              applyScene3dColor)
        || !applyDomainVisual(documents.spatial,
                              package.manifest.entries.visuals.spatial,
                              applySpatialColor)
        || !applyDomainVisual(documents.feedback,
                              package.manifest.entries.visuals.feedback,
                              applyFeedbackColor)
        || !applyPresenterSelections(documents.presenters, package, theme,
                                     diagnostics)
        || !applyRecipeSelections(documents.recipes, package, mergedAssets,
                                  theme, diagnostics))
        return false;

    const auto& curveMetrics = theme.domains.curve.metrics;
    if (curveMetrics.gridColumnCount <= 0 || curveMetrics.linearGridRowCount <= 0
        || curveMetrics.amplitudeGridMinimumRows <= 0
        || curveMetrics.amplitudeGridMinimumRows
            > curveMetrics.amplitudeGridMaximumRows
        || curveMetrics.curveLineWidth < 0.0 || curveMetrics.pointRadius < 0.0
        || curveMetrics.pointSelectedRadius < curveMetrics.pointRadius)
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::MetricOutOfRange,
            prefix + package.manifest.entries.visuals.curve,
            QStringLiteral("$.metrics"),
            QStringLiteral("Curve metrics violate semantic bounds.")));
        return false;
    }
    return true;
}

}

QString builtinThemeId()
{
    return QString::fromLatin1(kBuiltinThemeId);
}

bool applyThemeColor(ThemeSnapshot& theme, const QString& key, const QColor& color)
{
    return color.isValid()
        && (applyTokenColor(key, color, theme)
            || applyCurveColor(key, color, theme)
            || applyWaveformColor(key, color, theme)
            || applyMeterColor(key, color, theme)
            || applyDomainColor(key, color, theme));
}

ThemePackageDiscoveryResult discoverThemePackages(
    const QString& themesRoot,
    const ThemePackageLoadOptions& options)
{
    ThemePackageDiscoveryResult result;
    for (const auto& theme : kBuiltinThemes)
        result.themes.append(ThemePackageDescriptor{
            QString::fromLatin1(theme.id), QString::fromLatin1(theme.displayName),
            options.harkenVersion, true});

    const QFileInfo rootInfo(themesRoot);
    const QString rootCanonical = rootInfo.canonicalFilePath();
    if (!rootInfo.exists())
        return result;
    if (!rootInfo.isDir() || rootCanonical.isEmpty())
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::FileNotFound, themesRoot,
            QStringLiteral("$"), QStringLiteral("Theme root is not a directory.")));
        return result;
    }

    const QFileInfoList directories = QDir(rootCanonical).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    int packageCount = 0;
    for (const QFileInfo& directory : directories)
    {
        if (QFileInfo::exists(QDir(directory.absoluteFilePath()).filePath(
                QStringLiteral("manifest.json"))))
            ++packageCount;
    }
    if (packageCount > options.limits.maximumThemeCount)
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::ResourceLimit, themesRoot,
            QStringLiteral("$"), QStringLiteral("Theme count exceeds the limit.")));
        return result;
    }

    qsizetype totalBytes = 0;
    QMap<QString, ThemePackageDescriptor> discovered;
    QSet<QString> duplicateIds;
    for (const QFileInfo& directory : directories)
    {
        const QString packageRoot = directory.canonicalFilePath();
        if (packageRoot.isEmpty() || !isContained(rootCanonical, packageRoot))
        {
            result.diagnostics.append(diagnostic(
                ThemePackageErrorCode::PathEscape, directory.fileName(),
                QStringLiteral("$"),
                QStringLiteral("Theme directory resolves outside the theme root.")));
            continue;
        }
        const QString displayPath = directory.fileName()
            + QStringLiteral("/manifest.json");
        const QString manifestPath = QDir(packageRoot).filePath(
            QStringLiteral("manifest.json"));
        if (!QFileInfo::exists(manifestPath))
            continue;
        const auto constrainedManifest = constrainedPath(
            packageRoot, QStringLiteral("manifest.json"), true, displayPath,
            QStringLiteral("$"), result.diagnostics);
        if (!constrainedManifest)
            continue;
        const auto bytes = readJsonFile(*constrainedManifest, displayPath,
                                        options.limits, totalBytes,
                                        result.diagnostics);
        if (!bytes)
            continue;
        const auto parsed = parseThemeManifest(*bytes, displayPath);
        if (!parsed.ok())
        {
            result.diagnostics.append(parsed.diagnostics);
            continue;
        }
        if (!isStableId(parsed.value->id) || !parseVersion(parsed.value->version))
        {
            result.diagnostics.append(diagnostic(
                ThemePackageErrorCode::InvalidValue, displayPath,
                QStringLiteral("$.id"),
                QStringLiteral("Theme IDs and versions must use stable formats.")));
            continue;
        }
        if (findBuiltinTheme(parsed.value->id)
            || duplicateIds.contains(parsed.value->id)
            || discovered.contains(parsed.value->id))
        {
            result.diagnostics.append(diagnostic(
                ThemePackageErrorCode::DuplicateThemeId, displayPath,
                QStringLiteral("$.id"),
                QStringLiteral("Theme ID is duplicated or reserved.")));
            discovered.remove(parsed.value->id);
            duplicateIds.insert(parsed.value->id);
            continue;
        }
        discovered.insert(parsed.value->id, ThemePackageDescriptor{
            parsed.value->id, parsed.value->displayName,
            parsed.value->version, false});
    }

    for (const ThemePackageDescriptor& descriptor : discovered)
        result.themes.append(descriptor);
    return result;
}

ThemePackageLoadResult loadThemePackageCandidate(
    const QString& themesRoot,
    const QString& themeId,
    const ThemePackageLoadOptions& options)
{
    ThemePackageLoadResult result;
    if (const auto* builtin = findBuiltinTheme(themeId))
    {
        ThemePackageCandidate candidate;
        candidate.manifest.id = themeId;
        candidate.manifest.displayName = QString::fromLatin1(builtin->displayName);
        candidate.manifest.version = options.harkenVersion;
        candidate.snapshot = std::make_shared<const ThemeSnapshot>(
            builtin->create());
        candidate.inheritanceOrder.append(themeId);
        result.candidate = std::move(candidate);
        return result;
    }

    const QFileInfo rootInfo(themesRoot);
    const QString rootCanonical = rootInfo.canonicalFilePath();
    if (!rootInfo.isDir() || rootCanonical.isEmpty())
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::FileNotFound, themesRoot,
            QStringLiteral("$"), QStringLiteral("Theme root does not exist.")));
        return result;
    }

    QDir root(rootCanonical);
    const QFileInfoList directories = root.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    int packageCount = 0;
    for (const QFileInfo& directory : directories)
    {
        if (QFileInfo::exists(QDir(directory.absoluteFilePath()).filePath(
                QStringLiteral("manifest.json"))))
            ++packageCount;
    }
    if (packageCount > options.limits.maximumThemeCount)
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::ResourceLimit, themesRoot,
            QStringLiteral("$"), QStringLiteral("Theme count exceeds the limit.")));
        return result;
    }

    QMap<QString, DiscoveredPackage> packages;
    QSet<QString> duplicateIds;
    QList<ThemePackageDiagnostic> firstSkippedDiagnostics;
    const auto rememberSkipped = [&firstSkippedDiagnostics](
                                     const QList<ThemePackageDiagnostic>& diagnostics) {
        if (firstSkippedDiagnostics.isEmpty() && !diagnostics.isEmpty())
            firstSkippedDiagnostics = diagnostics;
    };
    for (const QFileInfo& directory : directories)
    {
        QList<ThemePackageDiagnostic> packageDiagnostics;
        qsizetype packageManifestBytes = 0;
        const QString packageRoot = directory.canonicalFilePath();
        if (packageRoot.isEmpty() || !isContained(rootCanonical, packageRoot))
            continue;
        const QString displayPath = directory.fileName()
            + QStringLiteral("/manifest.json");
        const QString manifestPath = QDir(packageRoot).filePath(
            QStringLiteral("manifest.json"));
        if (!QFileInfo::exists(manifestPath))
            continue;
        const auto constrainedManifest = constrainedPath(
            packageRoot, QStringLiteral("manifest.json"), true, displayPath,
            QStringLiteral("$"), packageDiagnostics);
        if (!constrainedManifest)
        {
            rememberSkipped(packageDiagnostics);
            continue;
        }
        const auto bytes = readJsonFile(*constrainedManifest, displayPath,
                                        options.limits,
                                        packageManifestBytes,
                                        packageDiagnostics);
        if (!bytes)
        {
            rememberSkipped(packageDiagnostics);
            continue;
        }
        const auto parsed = parseThemeManifest(*bytes, displayPath);
        if (!parsed.ok())
        {
            rememberSkipped(parsed.diagnostics);
            continue;
        }
        if (!isStableId(parsed.value->id)
            || (parsed.value->baseThemeId
                && !isStableId(*parsed.value->baseThemeId))
            || !parseVersion(parsed.value->version))
        {
            rememberSkipped({diagnostic(
                ThemePackageErrorCode::InvalidValue, displayPath,
                QStringLiteral("$.id"),
                QStringLiteral("Theme IDs and versions must use stable formats."))});
            continue;
        }
        if (findBuiltinTheme(parsed.value->id)
            || duplicateIds.contains(parsed.value->id)
            || packages.contains(parsed.value->id))
        {
            packages.remove(parsed.value->id);
            duplicateIds.insert(parsed.value->id);
            continue;
        }
        packages.insert(parsed.value->id,
                        DiscoveredPackage{packageRoot, directory.fileName(),
                                          *parsed.value, packageManifestBytes});
    }

    if (!packages.contains(themeId))
    {
        const bool duplicate = duplicateIds.contains(themeId);
        if (!duplicate && packages.isEmpty() && !firstSkippedDiagnostics.isEmpty())
        {
            result.diagnostics = firstSkippedDiagnostics;
            return result;
        }
        result.diagnostics.append(diagnostic(
            duplicate ? ThemePackageErrorCode::DuplicateThemeId
                      : ThemePackageErrorCode::FileNotFound,
            themesRoot, QStringLiteral("$.id"),
            duplicate ? QStringLiteral("Requested theme ID is duplicated.")
                      : QStringLiteral("Requested theme was not found.")));
        return result;
    }

    QList<const DiscoveredPackage*> chain;
    QSet<QString> visiting;
    QString currentId = themeId;
    while (!findBuiltinTheme(currentId))
    {
        if (visiting.contains(currentId))
        {
            result.diagnostics.append(diagnostic(
                ThemePackageErrorCode::InheritanceCycle,
                packages.value(currentId).displayRoot
                    + QStringLiteral("/manifest.json"),
                QStringLiteral("$.baseThemeId"),
                QStringLiteral("Theme inheritance contains a cycle.")));
            return result;
        }
        if (chain.size() >= options.limits.maximumInheritanceDepth)
        {
            result.diagnostics.append(diagnostic(
                ThemePackageErrorCode::InheritanceDepthExceeded,
                packages.value(currentId).displayRoot
                    + QStringLiteral("/manifest.json"),
                QStringLiteral("$.baseThemeId"),
                QStringLiteral("Theme inheritance depth exceeds the limit.")));
            return result;
        }
        const auto packageIt = packages.constFind(currentId);
        if (packageIt == packages.constEnd())
        {
            const DiscoveredPackage* child = chain.isEmpty() ? nullptr : chain.back();
            result.diagnostics.append(diagnostic(
                ThemePackageErrorCode::MissingBaseTheme,
                child ? child->displayRoot + QStringLiteral("/manifest.json")
                      : themesRoot,
                QStringLiteral("$.baseThemeId"),
                QStringLiteral("Base theme was not found.")));
            return result;
        }
        visiting.insert(currentId);
        chain.append(&packageIt.value());
        currentId = packageIt->manifest.baseThemeId.value_or(
            QString::fromLatin1(kBuiltinThemeId));
    }
    std::reverse(chain.begin(), chain.end());

    qsizetype totalBytes = 0;
    for (const DiscoveredPackage* package : chain)
    {
        if (totalBytes > options.limits.maximumTotalBytes - package->manifestBytes)
        {
            result.diagnostics.append(diagnostic(
                ThemePackageErrorCode::ResourceLimit,
                package->displayRoot + QStringLiteral("/manifest.json"),
                QStringLiteral("$"),
                QStringLiteral("Theme JSON read budget exceeded.")));
            return result;
        }
        totalBytes += package->manifestBytes;
    }
    SemanticDocuments merged;
    ThemeSnapshot snapshot = findBuiltinTheme(currentId)->create();
    for (const DiscoveredPackage* package : chain)
    {
        if (package->manifest.colorScheme)
        {
            snapshot.colorScheme = *package->manifest.colorScheme == QStringLiteral("light")
                ? ThemeColorScheme::Light
                : ThemeColorScheme::Dark;
        }
        if (!compatible(*package, options, result.diagnostics))
            return result;
        const auto documents = loadDocuments(*package, options, totalBytes,
                                             result.diagnostics);
        if (!documents)
            return result;
        SemanticDocuments effective = merged;
        overlay(effective, *documents);
        if (!applySemantics(*documents, *package, effective.assets, snapshot,
                            result.diagnostics))
            return result;
        merged = std::move(effective);
    }

    resolveThemeColors(snapshot);

    ThemePackageCandidate candidate;
    if (packages.value(themeId).manifest.previewAssetId
        && !merged.assets.assets.contains(
            *packages.value(themeId).manifest.previewAssetId))
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::InvalidValue,
            packages.value(themeId).displayRoot
                + QStringLiteral("/manifest.json"),
            QStringLiteral("$.previewAsset"),
            QStringLiteral("Preview asset ID is not present in the merged asset map.")));
        return result;
    }
    candidate.manifest = packages.value(themeId).manifest;
    candidate.snapshot = std::make_shared<const ThemeSnapshot>(std::move(snapshot));
    candidate.assets = std::move(merged.assets);
    for (const DiscoveredPackage* package : chain)
        candidate.assetRoots.insert(package->manifest.id, package->root);
    candidate.inheritanceOrder.append(currentId);
    for (const DiscoveredPackage* package : chain)
        candidate.inheritanceOrder.append(package->manifest.id);
    result.candidate = std::move(candidate);
    return result;
}

}
