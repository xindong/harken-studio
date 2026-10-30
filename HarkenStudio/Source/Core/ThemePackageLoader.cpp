#include "ThemePackageLoader.h"

#include "ThemeColorResolver.h"
#include "ComponentRecipeRegistry.h"
#include "ParameterPresenterRegistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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

constexpr auto kBuiltinThemeId = "harken.studio.default";

struct BuiltinThemeDefinition
{
    const char* id;
    const char* displayName;
};

constexpr BuiltinThemeDefinition kBuiltinThemes[]{
    {kBuiltinThemeId, "Studio Default"},
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
    std::optional<ThemeColorDefinitionDto> colors;
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
    std::optional<QByteArray> colors;
    if (package.manifest.entries.colors)
        colors = load(*package.manifest.entries.colors, QStringLiteral("$.entries.colors"));
    if (!tokens || !curve || !meter || !waveform || !domainVisualsLoaded
        || !assets
        || (package.manifest.entries.presenters && !presenters)
        || (package.manifest.entries.recipes && !recipes)
        || (package.manifest.entries.colors && !colors))
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

    if (colors)
    {
        const auto parsed = parseThemeColorDefinition(
            *colors, prefix + *package.manifest.entries.colors);
        if (!parsed.ok())
        {
            diagnostics.append(parsed.diagnostics);
            return std::nullopt;
        }
        documents.colors = *parsed.value;
    }

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

template<typename Snapshot>
auto tokenColorSlot(const QString& key, Snapshot& theme)
    -> decltype(&theme.ui.colors.accent)
{
#define COLOR_SLOT(name, field) \
    if (key == QStringLiteral(name)) return &theme.field
    COLOR_SLOT("background.primary", ui.colors.bgPrimary);
    COLOR_SLOT("background.secondary", ui.colors.bgSecondary);
    COLOR_SLOT("background.tertiary", ui.colors.bgTertiary);
    COLOR_SLOT("text.secondary", ui.colors.textSecondary);
    COLOR_SLOT("text.muted", ui.colors.textMuted);
    COLOR_SLOT("drop.highlight", ui.colors.dropHighlight);
    COLOR_SLOT("drop.accent", ui.colors.dropAccent);
    COLOR_SLOT("qtPalette.window", qtPalette.window);
    COLOR_SLOT("qtPalette.windowText", qtPalette.windowText);
    COLOR_SLOT("qtPalette.base", qtPalette.base);
    COLOR_SLOT("qtPalette.alternateBase", qtPalette.alternateBase);
    COLOR_SLOT("qtPalette.toolTipBase", qtPalette.toolTipBase);
    COLOR_SLOT("qtPalette.toolTipText", qtPalette.toolTipText);
    COLOR_SLOT("qtPalette.text", qtPalette.text);
    COLOR_SLOT("qtPalette.button", qtPalette.button);
    COLOR_SLOT("qtPalette.buttonText", qtPalette.buttonText);
    COLOR_SLOT("qtPalette.brightText", qtPalette.brightText);
    COLOR_SLOT("qtPalette.link", qtPalette.link);
    COLOR_SLOT("qtPalette.highlight", qtPalette.highlight);
    COLOR_SLOT("qtPalette.highlightedText", qtPalette.highlightedText);
    COLOR_SLOT("qtPalette.light", qtPalette.light);
    COLOR_SLOT("qtPalette.dark", qtPalette.dark);
    COLOR_SLOT("qtPalette.mid", qtPalette.mid);
    COLOR_SLOT("qtPalette.disabledText", qtPalette.disabledText);
    COLOR_SLOT("qtPalette.disabledButtonText", qtPalette.disabledButtonText);
    COLOR_SLOT("qtPalette.disabledHighlight", qtPalette.disabledHighlight);
    COLOR_SLOT("qtAds.chromeBackground", thirdParty.qtAds.chromeBackground);
    COLOR_SLOT("qtAds.inactiveTabText", thirdParty.qtAds.inactiveTabText);
    COLOR_SLOT("qtAds.closeButtonHover", thirdParty.qtAds.closeButtonHover);
    COLOR_SLOT("qtAds.closeButtonPressed", thirdParty.qtAds.closeButtonPressed);
    COLOR_SLOT("qtAds.lightHoverOverlay", thirdParty.qtAds.lightHoverOverlay);
    COLOR_SLOT("accent.primary", ui.colors.accent);
    COLOR_SLOT("accent.hover", ui.colors.accentHover);
    COLOR_SLOT("accent.text", ui.colors.accentText);
    COLOR_SLOT("text.primary", ui.colors.textPrimary);
    COLOR_SLOT("text.disabled", ui.colors.textDisabled);
    COLOR_SLOT("palette.categorical0", ui.colors.categoricalPalette[0]);
    COLOR_SLOT("palette.categorical1", ui.colors.categoricalPalette[1]);
    COLOR_SLOT("palette.categorical2", ui.colors.categoricalPalette[2]);
    COLOR_SLOT("palette.categorical3", ui.colors.categoricalPalette[3]);
    COLOR_SLOT("palette.categorical4", ui.colors.categoricalPalette[4]);
    COLOR_SLOT("palette.categorical5", ui.colors.categoricalPalette[5]);
    COLOR_SLOT("palette.categorical6", ui.colors.categoricalPalette[6]);
    COLOR_SLOT("background.hover", ui.colors.bgHover);
    COLOR_SLOT("background.selected", ui.colors.bgSelected);
    COLOR_SLOT("border.primary", ui.colors.borderPrimary);
    COLOR_SLOT("border.secondary", ui.colors.borderSecondary);
    COLOR_SLOT("border.view", ui.colors.viewBorder);
    COLOR_SLOT("status.success", ui.colors.success);
    COLOR_SLOT("status.warning", ui.colors.warning);
    COLOR_SLOT("status.error", ui.colors.error);
    COLOR_SLOT("status.info", ui.colors.info);
    COLOR_SLOT("track.background", ui.colors.trackBackground);
    COLOR_SLOT("track.backgroundAlternate", ui.colors.trackBackgroundAlt);
    COLOR_SLOT("bus.background", ui.colors.busBackground);
    COLOR_SLOT("bus.backgroundActive", ui.colors.busBackgroundActive);
    COLOR_SLOT("slider.track", ui.colors.sliderTrack);
    COLOR_SLOT("knob.indicator", ui.colors.knobIndicator);
    COLOR_SLOT("panel.background", ui.colors.panelBackground);
    COLOR_SLOT("section.separator", ui.colors.sectionSeparator);
    COLOR_SLOT("header.background", ui.colors.headerBackground);
    COLOR_SLOT("audition.background", ui.colors.auditionBg);
    COLOR_SLOT("audition.playActive", ui.colors.auditionPlayActive);
    COLOR_SLOT("audition.stopActive", ui.colors.auditionStopActive);
    COLOR_SLOT("shell.menuText", ui.colors.shellMenuText);
    COLOR_SLOT("shell.menuHoverText", ui.colors.shellMenuHoverText);
    COLOR_SLOT("shell.buttonText", ui.colors.shellButtonText);
    COLOR_SLOT("shell.buttonHoverText", ui.colors.shellButtonHoverText);
    COLOR_SLOT("shell.closeHover", ui.colors.shellCloseHover);
    COLOR_SLOT("shell.closePressed", ui.colors.shellClosePressed);
#undef COLOR_SLOT
    return nullptr;
}

bool applyTokenColor(const QString& key, const QColor& value, ThemeSnapshot& theme)
{
    if (auto* color = tokenColorSlot(key, theme))
    {
        *color = value;
        return true;
    }
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

template<typename Snapshot>
auto curveColorSlot(const QString& key, Snapshot& theme)
    -> decltype(&theme.ui.colors.accent)
{
#define COLOR_SLOT(name, field) \
    if (key == QStringLiteral(name)) return &theme.field
    COLOR_SLOT("curve.canvasBackground", domains.curve.colors.canvasBackground);
    COLOR_SLOT("curve.canvasBorder", domains.curve.colors.canvasBorder);
    COLOR_SLOT("curve.plotBackground", domains.curve.colors.plotBackground);
    COLOR_SLOT("curve.grid", domains.curve.colors.grid);
    COLOR_SLOT("curve.plotBorder", domains.curve.colors.plotBorder);
    COLOR_SLOT("curve.axisText", domains.curve.colors.axisText);
    COLOR_SLOT("curve.handleLine", domains.curve.colors.handleLine);
    COLOR_SLOT("curve.handleBorder", domains.curve.colors.handleBorder);
    COLOR_SLOT("curve.handleActiveBorder", domains.curve.colors.handleActiveBorder);
    COLOR_SLOT("curve.handleFill", domains.curve.colors.handleFill);
    COLOR_SLOT("curve.handleActiveFill", domains.curve.colors.handleActiveFill);
    COLOR_SLOT("curve.pointBorder", domains.curve.colors.pointBorder);
    COLOR_SLOT("curve.pointSelectedBorder", domains.curve.colors.pointSelectedBorder);
    COLOR_SLOT("curve.pointSelectedFill", domains.curve.colors.pointSelectedFill);
    COLOR_SLOT("curve.crosshairContrast", domains.curve.colors.crosshairContrast);
    COLOR_SLOT("curve.readoutBackground", domains.curve.colors.readoutBackground);
    COLOR_SLOT("curve.readoutText", domains.curve.colors.readoutText);
    COLOR_SLOT("curve.line", domains.curve.colors.attenuationVolume);
    COLOR_SLOT("curve.attenuationVolume", domains.curve.colors.attenuationVolume);
    COLOR_SLOT("curve.attenuationPitch", domains.curve.colors.attenuationPitch);
    COLOR_SLOT("curve.parameterBinding", domains.curve.colors.parameterBinding);
    COLOR_SLOT("curve.gridFade", domains.curve.colors.gridFade);
    COLOR_SLOT("curve.ghost", domains.curve.colors.ghost);
    COLOR_SLOT("curve.lassoBorder", domains.curve.colors.lassoBorder);
    COLOR_SLOT("curve.lassoFill", domains.curve.colors.lassoFill);
    COLOR_SLOT("curve.crosshair", domains.curve.colors.crosshair);
    COLOR_SLOT("curve.playhead", domains.curve.colors.playhead);
#undef COLOR_SLOT
    return nullptr;
}

bool applyCurveColor(const QString& key, const QColor& value, ThemeSnapshot& theme)
{
    if (auto* color = curveColorSlot(key, theme))
    {
        *color = value;
        return true;
    }
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

template<typename Snapshot>
auto waveformColorSlot(const QString& key, Snapshot& theme)
    -> decltype(&theme.ui.colors.accent)
{
#define COLOR_SLOT(name, field) \
    if (key == QStringLiteral(name)) return &theme.field
    COLOR_SLOT("waveform.dbScale.grid", domains.waveform.dbScale.grid);
    COLOR_SLOT("waveform.dbScale.label", domains.waveform.dbScale.label);
    COLOR_SLOT("waveform.line", domains.waveform.line);
    COLOR_SLOT("waveform.fill", domains.waveform.fill);
    COLOR_SLOT("waveform.selectionHighlight", domains.waveform.selectionHighlight);
    COLOR_SLOT("waveform.cursor", domains.waveform.cursor);
    COLOR_SLOT("waveform.regionMarker", domains.waveform.regionMarker);
    COLOR_SLOT("waveform.trimHandle", domains.waveform.trimHandle);
    COLOR_SLOT("waveform.loopHandle", domains.waveform.loopHandle);
    COLOR_SLOT("waveform.static.quiet", domains.waveform.staticMode.quiet);
    COLOR_SLOT("waveform.static.peak", domains.waveform.staticMode.peak);
    COLOR_SLOT("waveform.splitStereo.divider", domains.waveform.splitStereo.divider);
    COLOR_SLOT("waveform.splitStereo.label", domains.waveform.splitStereo.label);
    COLOR_SLOT("waveform.rawPeaks.background", domains.waveform.rawPeaks.background);
    COLOR_SLOT("waveform.rawPeaks.centerLine", domains.waveform.rawPeaks.centerLine);
    COLOR_SLOT("waveform.multiBand.color0", domains.waveform.multiBandMode.colors[0]);
    COLOR_SLOT("waveform.multiBand.color1", domains.waveform.multiBandMode.colors[1]);
    COLOR_SLOT("waveform.multiBand.color2", domains.waveform.multiBandMode.colors[2]);
#undef COLOR_SLOT
    return nullptr;
}

bool applyWaveformColor(const QString& key, const QColor& value, ThemeSnapshot& theme)
{
    if (auto* color = waveformColorSlot(key, theme))
    {
        *color = value;
        return true;
    }
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

template<typename Snapshot>
auto meterColorSlot(const QString& key, Snapshot& theme)
    -> decltype(&theme.ui.colors.accent)
{
#define COLOR_SLOT(name, field) \
    if (key == QStringLiteral(name)) return &theme.field
    COLOR_SLOT("meter.oscilloscopeGrid", domains.meter.oscilloscopeGrid);
    COLOR_SLOT("meter.oscilloscopeGlow", domains.meter.oscilloscopeGlow);
    COLOR_SLOT("meter.waveformFill", domains.meter.waveformFill);
    COLOR_SLOT("meter.waveformGrid", domains.meter.waveformGrid);
    COLOR_SLOT("meter.stereo.trailOuter", domains.meter.stereo.trailOuter);
    COLOR_SLOT("meter.stereo.trailMiddle", domains.meter.stereo.trailMiddle);
    COLOR_SLOT("meter.stereo.trailCore", domains.meter.stereo.trailCore);
    COLOR_SLOT("meter.stereo.gridPrimary", domains.meter.stereo.gridPrimary);
    COLOR_SLOT("meter.stereo.gridSecondary", domains.meter.stereo.gridSecondary);
    COLOR_SLOT("meter.stereo.liveGlowWide", domains.meter.stereo.liveGlowWide);
    COLOR_SLOT("meter.stereo.liveGlowMedium", domains.meter.stereo.liveGlowMedium);
    COLOR_SLOT("meter.stereo.liveHighlight", domains.meter.stereo.liveHighlight);
    COLOR_SLOT("meter.stereo.correlationPositive", domains.meter.stereo.correlationPositive);
    COLOR_SLOT("meter.stereo.correlationNegative", domains.meter.stereo.correlationNegative);
    COLOR_SLOT("meter.layout.dividerHover", domains.meter.layout.dividerHover);
    COLOR_SLOT("meter.layout.dividerDragging", domains.meter.layout.dividerDragging);
    COLOR_SLOT("meter.layout.dividerSnapped", domains.meter.layout.dividerSnapped);
    COLOR_SLOT("meter.layout.dropBorder", domains.meter.layout.dropBorder);
    COLOR_SLOT("meter.layout.dropFill", domains.meter.layout.dropFill);
    COLOR_SLOT("meter.interaction.crosshair", domains.meter.interaction.colors.crosshair);
    COLOR_SLOT("meter.interaction.readoutBackground", domains.meter.interaction.colors.readoutBackground);
    COLOR_SLOT("meter.interaction.readoutText", domains.meter.interaction.colors.readoutText);
    COLOR_SLOT("meter.peakMeter.canvasBackground", domains.meter.peakMeter.colors.canvasBackground);
    COLOR_SLOT("meter.peakMeter.trackBackground", domains.meter.peakMeter.colors.trackBackground);
    COLOR_SLOT("meter.peakMeter.grid", domains.meter.peakMeter.colors.grid);
    COLOR_SLOT("meter.peakMeter.tickText", domains.meter.peakMeter.colors.tickText);
    COLOR_SLOT("meter.peakMeter.label", domains.meter.peakMeter.colors.label);
    COLOR_SLOT("meter.loudness.canvasBackground", domains.meter.loudnessMeter.colors.canvasBackground);
    COLOR_SLOT("meter.loudness.trackBackground", domains.meter.loudnessMeter.colors.trackBackground);
    COLOR_SLOT("meter.loudness.targetText", domains.meter.loudnessMeter.colors.targetText);
    COLOR_SLOT("meter.loudness.labelText", domains.meter.loudnessMeter.colors.labelText);
    COLOR_SLOT("meter.loudness.valueText", domains.meter.loudnessMeter.colors.valueText);
    COLOR_SLOT("meter.vu.canvasBackground", domains.meter.vuMeter.colors.canvasBackground);
    COLOR_SLOT("meter.vu.titleText", domains.meter.vuMeter.colors.titleText);
    COLOR_SLOT("meter.vu.trackBackground", domains.meter.vuMeter.colors.trackBackground);
    COLOR_SLOT("meter.vu.analogArc", domains.meter.vuMeter.colors.analogArc);
    COLOR_SLOT("meter.vu.analogNegativeTick", domains.meter.vuMeter.colors.analogNegativeTick);
    COLOR_SLOT("meter.vu.analogValueText", domains.meter.vuMeter.colors.analogValueText);
    COLOR_SLOT("meter.vu.analogLabelText", domains.meter.vuMeter.colors.analogLabelText);
    COLOR_SLOT("meter.vu.barGrid", domains.meter.vuMeter.colors.barGrid);
    COLOR_SLOT("meter.vu.barLabelText", domains.meter.vuMeter.colors.barLabelText);
    COLOR_SLOT("meter.vu.barValueText", domains.meter.vuMeter.colors.barValueText);
    COLOR_SLOT("meter.spectrum.canvasBackground", domains.meter.spectrumMeter.colors.canvasBackground);
    COLOR_SLOT("meter.spectrum.horizontalGrid", domains.meter.spectrumMeter.colors.horizontalGrid);
    COLOR_SLOT("meter.spectrum.verticalGrid", domains.meter.spectrumMeter.colors.verticalGrid);
    COLOR_SLOT("meter.spectrum.axisText", domains.meter.spectrumMeter.colors.axisText);
    COLOR_SLOT("meter.spectrogram.canvasBackground", domains.meter.spectrogramMeter.colors.canvasBackground);
    COLOR_SLOT("meter.spectrogram.grid", domains.meter.spectrogramMeter.colors.grid);
    COLOR_SLOT("meter.spectrogram.axisText", domains.meter.spectrogramMeter.colors.axisText);
    COLOR_SLOT("meter.spectrogram.timeText", domains.meter.spectrogramMeter.colors.timeText);
    COLOR_SLOT("meter.green", domains.meter.green);
    COLOR_SLOT("meter.yellow", domains.meter.yellow);
    COLOR_SLOT("meter.red", domains.meter.red);
    COLOR_SLOT("meter.peak", domains.meter.peak);
    COLOR_SLOT("meter.rms", domains.meter.rms);
    COLOR_SLOT("meter.clipOn", domains.meter.clipOn);
    COLOR_SLOT("meter.clipOff", domains.meter.clipOff);
    COLOR_SLOT("meter.background", domains.meter.background);
    COLOR_SLOT("meter.lufsMomentary", domains.meter.lufsMomentary);
    COLOR_SLOT("meter.lufsShortTerm", domains.meter.lufsShortTerm);
    COLOR_SLOT("meter.lufsIntegrated", domains.meter.lufsIntegrated);
    COLOR_SLOT("meter.interaction.accentGuide", domains.meter.interaction.colors.accentGuide);
    COLOR_SLOT("meter.peakMeter.gradientError", domains.meter.peakMeter.colors.gradientError);
    COLOR_SLOT("meter.peakMeter.gradientWarning", domains.meter.peakMeter.colors.gradientWarning);
    COLOR_SLOT("meter.peakMeter.gradientYellow", domains.meter.peakMeter.colors.gradientYellow);
    COLOR_SLOT("meter.peakMeter.gradientAccent", domains.meter.peakMeter.colors.gradientAccent);
    COLOR_SLOT("meter.peakMeter.gradientGreen", domains.meter.peakMeter.colors.gradientAccent);
    COLOR_SLOT("meter.peakMeter.rmsOverlay", domains.meter.peakMeter.colors.rmsOverlay);
    COLOR_SLOT("meter.peakMeter.holdHalo", domains.meter.peakMeter.colors.holdHalo);
    COLOR_SLOT("meter.peakMeter.holdLine", domains.meter.peakMeter.colors.holdLine);
    COLOR_SLOT("meter.peakMeter.clipBackground", domains.meter.peakMeter.colors.clipBackground);
    COLOR_SLOT("meter.peakMeter.clipText", domains.meter.peakMeter.colors.clipText);
    COLOR_SLOT("meter.loudness.targetLine", domains.meter.loudnessMeter.colors.targetLine);
    COLOR_SLOT("meter.vu.analogPositiveTick", domains.meter.vuMeter.colors.analogPositiveTick);
    COLOR_SLOT("meter.vu.analogOverloadArc", domains.meter.vuMeter.colors.analogOverloadArc);
    COLOR_SLOT("meter.vu.analogNeedleGlow", domains.meter.vuMeter.colors.analogNeedleGlow);
    COLOR_SLOT("meter.vu.analogNeedle", domains.meter.vuMeter.colors.analogNeedle);
    COLOR_SLOT("meter.vu.analogPivot", domains.meter.vuMeter.colors.analogPivot);
    COLOR_SLOT("meter.vu.barWarning", domains.meter.vuMeter.colors.barWarning);
    COLOR_SLOT("meter.spectrum.fillTop", domains.meter.spectrumMeter.colors.fillTop);
    COLOR_SLOT("meter.spectrum.fillUpperMiddle", domains.meter.spectrumMeter.colors.fillUpperMiddle);
    COLOR_SLOT("meter.spectrum.fillLowerMiddle", domains.meter.spectrumMeter.colors.fillLowerMiddle);
    COLOR_SLOT("meter.spectrum.fillBottom", domains.meter.spectrumMeter.colors.fillBottom);
    COLOR_SLOT("meter.spectrum.lineGlowWide", domains.meter.spectrumMeter.colors.lineGlowWide);
    COLOR_SLOT("meter.spectrum.lineGlowMedium", domains.meter.spectrumMeter.colors.lineGlowMedium);
    COLOR_SLOT("meter.spectrum.lineGlowNarrow", domains.meter.spectrumMeter.colors.lineGlowNarrow);
    COLOR_SLOT("meter.spectrum.lineHighlight", domains.meter.spectrumMeter.colors.lineHighlight);
    COLOR_SLOT("meter.spectrum.peakLine", domains.meter.spectrumMeter.colors.peakLine);
    COLOR_SLOT("meter.spectrogram.emptyHistory", domains.meter.spectrogramMeter.colors.emptyHistory);
#undef COLOR_SLOT
    return nullptr;
}

bool applyMeterColor(const QString& key, const QColor& value, ThemeSnapshot& theme)
{
    if (auto* color = meterColorSlot(key, theme))
    {
        *color = value;
        return true;
    }
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

template<typename Snapshot>
auto domainColorSlot(const QString& key, Snapshot& theme)
    -> decltype(&theme.ui.colors.accent)
{
#define COLOR_SLOT(name, field) \
    if (key == QStringLiteral(name)) return &theme.field
    COLOR_SLOT("nodeGraph.node.bodyNormal", domains.nodeGraphNode.bodyNormal);
    COLOR_SLOT("nodeGraph.node.bodySelected", domains.nodeGraphNode.bodySelected);
    COLOR_SLOT("nodeGraph.node.titleText", domains.nodeGraphNode.titleText);
    COLOR_SLOT("nodeGraph.node.contentLabel", domains.nodeGraphNode.contentLabel);
    COLOR_SLOT("nodeGraph.node.contentValue", domains.nodeGraphNode.contentValue);
    COLOR_SLOT("blueprint.edgeLabelText", domains.blueprint.edgeLabelText);
    COLOR_SLOT("feedback.success", domains.feedback.success);
    COLOR_SLOT("feedback.warning", domains.feedback.warning);
    COLOR_SLOT("feedback.error", domains.feedback.error);
    COLOR_SLOT("feedback.info", domains.feedback.info);
    COLOR_SLOT("feedback.neutral", domains.feedback.neutral);
    COLOR_SLOT("feedback.overlayHighlight", domains.feedback.overlayHighlight);
    COLOR_SLOT("startup.logo.base", domains.startup.logoPalette[0]);
    COLOR_SLOT("startup.logo.mid", domains.startup.logoPalette[1]);
    COLOR_SLOT("startup.logo.highlight", domains.startup.logoPalette[2]);
    COLOR_SLOT("startup.wave.shadow", domains.startup.wavePalette[0]);
    COLOR_SLOT("startup.wave.low", domains.startup.wavePalette[1]);
    COLOR_SLOT("startup.wave.high", domains.startup.wavePalette[2]);
    COLOR_SLOT("startup.wave.highlight", domains.startup.wavePalette[3]);
    COLOR_SLOT("feedback.overlayBackground", domains.feedback.overlayBackground);
    COLOR_SLOT("feedback.overlayErrorBackground", domains.feedback.overlayErrorBackground);
    COLOR_SLOT("nodeGraph.pin.audio", domains.pinDomain.audio);
    COLOR_SLOT("nodeGraph.pin.control", domains.pinDomain.control);
    COLOR_SLOT("nodeGraph.pin.value", domains.pinDomain.value);
    COLOR_SLOT("nodeGraph.pin.ring", domains.pinDomain.ring);
    COLOR_SLOT("nodeGraph.pin.targetOk", domains.pinDomain.targetOk);
    COLOR_SLOT("nodeGraph.pin.targetBad", domains.pinDomain.targetBad);
    COLOR_SLOT("nodeGraph.node.borderNormal", domains.nodeGraphNode.borderNormal);
    COLOR_SLOT("nodeGraph.node.borderHoverMix", domains.nodeGraphNode.borderHoverMix);
    COLOR_SLOT("nodeGraph.node.dropTarget", domains.nodeGraphNode.dropTarget);
    COLOR_SLOT("nodeGraph.node.shadow", domains.nodeGraphNode.shadow);
    COLOR_SLOT("nodeGraph.node.sandboxBadgeBg", domains.nodeGraphNode.sandboxBadgeBg);
    COLOR_SLOT("nodeGraph.node.sandboxBadgeText", domains.nodeGraphNode.sandboxBadgeText);
    COLOR_SLOT("nodeGraph.node.badgeOverride", domains.nodeGraphNode.badgeOverride);
    COLOR_SLOT("nodeGraph.node.badgeChild", domains.nodeGraphNode.badgeChild);
    COLOR_SLOT("nodeGraph.node.badgeReceive", domains.nodeGraphNode.badgeReceive);
    COLOR_SLOT("nodeGraph.node.waveformBg", domains.nodeGraphNode.waveformBg);
    COLOR_SLOT("nodeGraph.node.waveformCenter", domains.nodeGraphNode.waveformCenter);
    COLOR_SLOT("nodeGraph.node.blueprintWaveform", domains.nodeGraphNode.blueprintWaveform);
    COLOR_SLOT("blueprint.canvasBackground", domains.blueprint.canvasBackground);
    COLOR_SLOT("blueprint.gridDot", domains.blueprint.gridDot);
    COLOR_SLOT("blueprint.edgeLabelBg", domains.blueprint.edgeLabelBg);
    COLOR_SLOT("blueprint.placementChain", domains.blueprint.placementChain);
    COLOR_SLOT("blueprint.organizationChain", domains.blueprint.organizationChain);
    COLOR_SLOT("blueprint.containment", domains.blueprint.containment);
    COLOR_SLOT("blueprint.busRoute", domains.blueprint.busRoute);
    COLOR_SLOT("blueprint.send", domains.blueprint.send);
    COLOR_SLOT("blueprint.overrideRoute", domains.blueprint.overrideRoute);
    COLOR_SLOT("timeline.entry", domains.timeline.entry);
    COLOR_SLOT("timeline.exit", domains.timeline.exit);
    COLOR_SLOT("timeline.transition", domains.timeline.transition);
    COLOR_SLOT("timeline.clip", domains.timeline.clip);
    COLOR_SLOT("timeline.clipSelected", domains.timeline.clipSelected);
    COLOR_SLOT("timeline.clipBorder", domains.timeline.clipBorder);
    COLOR_SLOT("timeline.clipSelectedBorder", domains.timeline.clipSelectedBorder);
    COLOR_SLOT("timeline.clipText", domains.timeline.clipText);
    COLOR_SLOT("timeline.badge", domains.timeline.badge);
    COLOR_SLOT("timeline.emergenceMarker", domains.timeline.emergenceMarker);
    COLOR_SLOT("timeline.unavailableOverlay", domains.timeline.unavailableOverlay);
    COLOR_SLOT("timeline.unavailableText", domains.timeline.unavailableText);
    COLOR_SLOT("timeline.outOfSpanVeil", domains.timeline.outOfSpanVeil);
    COLOR_SLOT("music.beatGridMajor", domains.music.beatGridMajor);
    COLOR_SLOT("music.beatGridMinor", domains.music.beatGridMinor);
    COLOR_SLOT("music.cursor", domains.music.cursor);
    COLOR_SLOT("music.loopRegion", domains.music.loopRegion);
    COLOR_SLOT("log.debug", domains.log.debug);
    COLOR_SLOT("log.info", domains.log.info);
    COLOR_SLOT("log.warning", domains.log.warning);
    COLOR_SLOT("log.error", domains.log.error);
    COLOR_SLOT("log.critical", domains.log.critical);
    COLOR_SLOT("monitor.stateStolen", domains.monitor.stateStolen);
    COLOR_SLOT("monitor.statePlaying", domains.monitor.statePlaying);
    COLOR_SLOT("monitor.stateStarting", domains.monitor.stateStarting);
    COLOR_SLOT("monitor.stateVirtual", domains.monitor.stateVirtual);
    COLOR_SLOT("monitor.stateReleasing", domains.monitor.stateReleasing);
    COLOR_SLOT("monitor.stateStopping", domains.monitor.stateStopping);
    COLOR_SLOT("monitor.statePaused", domains.monitor.statePaused);
    COLOR_SLOT("monitor.stateDefault", domains.monitor.stateDefault);
    COLOR_SLOT("monitor.virtualDim", domains.monitor.virtualDim);
    COLOR_SLOT("monitor.mutedText", domains.monitor.mutedText);
    COLOR_SLOT("monitor.lowVolume", domains.monitor.lowVolume);
    COLOR_SLOT("monitor.busIdText", domains.monitor.busIdText);
    COLOR_SLOT("monitor.peakClip", domains.monitor.peakClip);
    COLOR_SLOT("monitor.peakCaution", domains.monitor.peakCaution);
    COLOR_SLOT("monitor.peakSafe", domains.monitor.peakSafe);
    COLOR_SLOT("monitor.pitchHigh", domains.monitor.pitchHigh);
    COLOR_SLOT("monitor.pitchLow", domains.monitor.pitchLow);
    COLOR_SLOT("monitor.cpuCritical", domains.monitor.cpuCritical);
    COLOR_SLOT("monitor.cpuWarning", domains.monitor.cpuWarning);
    COLOR_SLOT("monitor.cpuSafe", domains.monitor.cpuSafe);
    COLOR_SLOT("monitor.bufferWarning", domains.monitor.bufferWarning);
    COLOR_SLOT("monitor.bufferNormal", domains.monitor.bufferNormal);
    COLOR_SLOT("monitor.underrunFlash", domains.monitor.underrunFlash);
    COLOR_SLOT("monitor.underrunIdle", domains.monitor.underrunIdle);
    COLOR_SLOT("monitor.underrunNone", domains.monitor.underrunNone);
    COLOR_SLOT("monitor.spatialGainHigh", domains.monitor.spatialGainHigh);
    COLOR_SLOT("monitor.spatialGainLow", domains.monitor.spatialGainLow);
    COLOR_SLOT("monitor.spatialLpf", domains.monitor.spatialLpf);
    COLOR_SLOT("monitor.spatialWidth", domains.monitor.spatialWidth);
    COLOR_SLOT("monitor.spatialDistance", domains.monitor.spatialDistance);
    COLOR_SLOT("monitor.spatialReverb", domains.monitor.spatialReverb);
    COLOR_SLOT("monitor.spatialDopplerHi", domains.monitor.spatialDopplerHi);
    COLOR_SLOT("monitor.spatialDopplerLo", domains.monitor.spatialDopplerLo);
    COLOR_SLOT("monitor.spatialDot", domains.monitor.spatialDot);
    COLOR_SLOT("ai.brandAccent", domains.ai.brandAccent);
    COLOR_SLOT("ai.userAccent", domains.ai.userAccent);
    COLOR_SLOT("scene3d.listener", domains.scene3d.listener);
    COLOR_SLOT("scene3d.listenerCone", domains.scene3d.listenerCone);
    COLOR_SLOT("scene3d.room", domains.scene3d.room);
    COLOR_SLOT("scene3d.emitterMin", domains.scene3d.emitterMin);
    COLOR_SLOT("scene3d.emitterMax", domains.scene3d.emitterMax);
    COLOR_SLOT("scene3d.hudBackground", domains.scene3d.hudBackground);
    COLOR_SLOT("scene3d.hudTitle", domains.scene3d.hudTitle);
    COLOR_SLOT("scene3d.hudLabel", domains.scene3d.hudLabel);
    COLOR_SLOT("scene3d.hudGain", domains.scene3d.hudGain);
    COLOR_SLOT("scene3d.hudGainLow", domains.scene3d.hudGainLow);
    COLOR_SLOT("scene3d.hudLpf", domains.scene3d.hudLpf);
    COLOR_SLOT("scene3d.hudWidth", domains.scene3d.hudWidth);
    COLOR_SLOT("scene3d.dopplerIdle", domains.scene3d.dopplerIdle);
    COLOR_SLOT("spatial.background", domains.spatial.background);
    COLOR_SLOT("spatial.grid", domains.spatial.grid);
    COLOR_SLOT("spatial.axis", domains.spatial.axis);
    COLOR_SLOT("spatial.axisLabel", domains.spatial.axisLabel);
    COLOR_SLOT("spatial.tangentLine", domains.spatial.tangentLine);
    COLOR_SLOT("spatial.tangentHover", domains.spatial.tangentHover);
    COLOR_SLOT("spatial.tangentActive", domains.spatial.tangentActive);
    COLOR_SLOT("spatial.pointSelected", domains.spatial.pointSelected);
    COLOR_SLOT("spatial.previewMarker", domains.spatial.previewMarker);
    COLOR_SLOT("spatial.infoText", domains.spatial.infoText);
    COLOR_SLOT("spatial.legendBg", domains.spatial.legendBg);
    COLOR_SLOT("spatial.layerVolume", domains.spatial.layerVolume);
    COLOR_SLOT("spatial.layerLpf", domains.spatial.layerLpf);
    COLOR_SLOT("spatial.layerWidth", domains.spatial.layerWidth);
    COLOR_SLOT("spatial.layerReverb", domains.spatial.layerReverb);
#undef COLOR_SLOT
    for (int index = 0; index < DomainColors::Music::kTrackColorCount; ++index)
        if (key == QStringLiteral("music.trackColor%1").arg(index))
            return &theme.domains.music.trackColors[static_cast<size_t>(index)];
    return nullptr;
}

bool applyDomainColor(const QString& key, const QColor& value, ThemeSnapshot& theme)
{
    if (auto* color = domainColorSlot(key, theme))
    {
        *color = value;
        return true;
    }
    return false;
}

template<typename Snapshot>
auto themeColorSlot(const QString& key, Snapshot& theme)
    -> decltype(&theme.ui.colors.accent)
{
    if (auto* color = tokenColorSlot(key, theme)) return color;
    if (auto* color = curveColorSlot(key, theme)) return color;
    if (auto* color = waveformColorSlot(key, theme)) return color;
    if (auto* color = meterColorSlot(key, theme)) return color;
    return domainColorSlot(key, theme);
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

bool validateColorBindings(const QVector<ThemeColorRole>& roles,
                           const ThemeSnapshot& snapshot,
                           const QString& file,
                           QList<ThemePackageDiagnostic>& diagnostics)
{
    QSet<const QColor*> targets;
    for (qsizetype roleIndex = 0; roleIndex < roles.size(); ++roleIndex)
    {
        const auto& role = roles.at(roleIndex);
        for (qsizetype bindingIndex = 0; bindingIndex < role.bindings.size(); ++bindingIndex)
        {
            const auto& binding = role.bindings.at(bindingIndex);
            const QString path = QStringLiteral("$.roles[%1].bindings[%2].target")
                .arg(roleIndex).arg(bindingIndex);
            const auto* target = themeColorSlot(binding.target, snapshot);
            if (!target)
            {
                diagnostics.append(diagnostic(ThemePackageErrorCode::UnknownSemanticKey,
                    file, path, QStringLiteral("Unknown render color target: ") + binding.target));
                return false;
            }
            if (targets.contains(target))
            {
                diagnostics.append(diagnostic(ThemePackageErrorCode::InvalidValue,
                    file, path, QStringLiteral("A render color target can be bound only once.")));
                return false;
            }
            targets.insert(target);
        }
    }
    return true;
}

bool collectRoleOverrides(const SemanticDocuments& documents,
                          const DiscoveredPackage& package,
                          const QVector<ThemeColorRole>& roles,
                          const ThemeSnapshot& snapshot,
                          ThemeColorOverrides& overrides,
                          QList<ThemePackageDiagnostic>& diagnostics)
{
    struct BoundColor {
        const ThemeColorRole* role;
        bool fixedAlpha;
    };
    QHash<const QColor*, BoundColor> bindings;
    for (const auto& role : roles)
        for (const auto& binding : role.bindings)
            bindings.insert(themeColorSlot(binding.target, snapshot), {&role, binding.alpha.has_value()});
    const QString prefix = package.displayRoot + QLatin1Char('/');
    const auto collect = [&](const QMap<QString, QString>& colors, const QString& relative) {
        for (auto color = colors.constBegin(); color != colors.constEnd(); ++color)
        {
            const auto* target = themeColorSlot(color.key(), snapshot);
            if (!target || !bindings.contains(target))
                continue;
            const auto binding = bindings.value(target);
            const QString& roleKey = binding.role->key;
            QString conflict;
            if (documents.colors)
                conflict = QStringLiteral("Color role '%1' is defined by this package's palettes; remove the direct render color assignment.").arg(roleKey);
            else if (binding.fixedAlpha)
                conflict = QStringLiteral("Color target '%1' has a fixed alpha binding; override role '%2' in a color definition instead.")
                    .arg(color.key(), roleKey);
            const QColor value(color.value());
            if (conflict.isEmpty() && overrides.contains(roleKey) && overrides.value(roleKey) != value)
                conflict = QStringLiteral("Multiple render color assignments give conflicting values to role '%1'.").arg(roleKey);
            if (!conflict.isEmpty())
            {
                diagnostics.append(diagnostic(ThemePackageErrorCode::InvalidValue,
                    prefix + relative, QStringLiteral("$.colors.") + color.key(), conflict));
                return false;
            }
            overrides.insert(roleKey, value);
        }
        return true;
    };
    if (!collect(documents.tokens.colors, package.manifest.entries.tokens)
        || !collect(documents.curve.colors, package.manifest.entries.visuals.curve)
        || !collect(documents.meter.colors, package.manifest.entries.visuals.meter)
        || !collect(documents.waveform.colors, package.manifest.entries.visuals.waveform))
        return false;
#define COLLECT_VISUAL(name) \
    if (package.manifest.entries.visuals.name \
        && !collect(documents.name.colors, *package.manifest.entries.visuals.name)) return false
    COLLECT_VISUAL(nodeGraph);
    COLLECT_VISUAL(blueprint);
    COLLECT_VISUAL(timeline);
    COLLECT_VISUAL(music);
    COLLECT_VISUAL(log);
    COLLECT_VISUAL(monitor);
    COLLECT_VISUAL(ai);
    COLLECT_VISUAL(scene3d);
    COLLECT_VISUAL(spatial);
    COLLECT_VISUAL(feedback);
#undef COLLECT_VISUAL
    return true;
}

}

QString builtinThemeId()
{
    return QString::fromLatin1(kBuiltinThemeId);
}

bool applyThemeColor(ThemeSnapshot& theme, const QString& key, const QColor& color)
{
    auto* target = themeColorSlot(key, theme);
    if (!target || !color.isValid())
        return false;
    *target = color;
    return true;
}

QColor readThemeColor(const ThemeSnapshot& theme, const QString& key)
{
    const auto* color = themeColorSlot(key, theme);
    return color ? *color : QColor{};
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
        candidate.manifest.schemaVersion = ThemePackageContract::maximumSchemaVersion;
        candidate.colorRoles = studioDefaultColorRoles();
        candidate.palettes = createStudioDefaultPalettes();
        candidate.defaultPaletteId = QStringLiteral("dark");
        candidate.snapshot = candidate.palettes.front().snapshot;
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
    QVector<ThemeColorRole> colorRoles = studioDefaultColorRoles();
    QList<ColorPalette> palettes = createStudioDefaultPalettes();
    QString defaultPaletteId = QStringLiteral("dark");
    ThemeSnapshot snapshot = *palettes.front().snapshot;
    for (const DiscoveredPackage* package : chain)
    {
        if (!compatible(*package, options, result.diagnostics))
            return result;
        const auto documents = loadDocuments(*package, options, totalBytes,
                                             result.diagnostics);
        if (!documents)
            return result;
        SemanticDocuments effective = merged;
        overlay(effective, *documents);
        if (documents->colors)
        {
            colorRoles = documents->colors->roles;
            if (!validateColorBindings(colorRoles, snapshot,
                    package->displayRoot + QLatin1Char('/') + *package->manifest.entries.colors,
                    result.diagnostics))
                return result;
        }
        ThemeColorOverrides roleOverrides;
        if (!collectRoleOverrides(*documents, *package, colorRoles, snapshot,
                                  roleOverrides, result.diagnostics))
            return result;
        if (documents->colors)
        {
            if (!applySemantics(*documents, *package, effective.assets, snapshot,
                                result.diagnostics))
                return result;
            palettes.clear();
            defaultPaletteId = documents->colors->defaultPalette;
            for (const auto& definition : documents->colors->palettes)
            {
                ThemeSnapshot paletteSnapshot = snapshot;
                paletteSnapshot.colorScheme = definition.colorScheme;
                if (!resolveThemeColors(paletteSnapshot, colorRoles, definition.colors))
                {
                    result.diagnostics.append(diagnostic(ThemePackageErrorCode::InvalidColor,
                        package->displayRoot + QLatin1Char('/') + *package->manifest.entries.colors,
                        QStringLiteral("$.palettes"), QStringLiteral("Palette colors could not be resolved.")));
                    return result;
                }
                palettes.append({definition.id, definition.displayName, definition.colors,
                    std::make_shared<const ThemeSnapshot>(std::move(paletteSnapshot))});
            }
        }
        else
        {
            for (auto& palette : palettes)
            {
                ThemeSnapshot paletteSnapshot = *palette.snapshot;
                if (!applySemantics(*documents, *package, effective.assets,
                                    paletteSnapshot, result.diagnostics))
                    return result;
                overlayMap(palette.colors, roleOverrides);
                if (!resolveThemeColors(paletteSnapshot, colorRoles, palette.colors))
                {
                    result.diagnostics.append(diagnostic(ThemePackageErrorCode::InvalidColor,
                        package->displayRoot + QStringLiteral("/manifest.json"),
                        QStringLiteral("$.entries"), QStringLiteral("Inherited palette colors could not be resolved.")));
                    return result;
                }
                palette.snapshot = std::make_shared<const ThemeSnapshot>(std::move(paletteSnapshot));
            }
        }
        const auto selected = std::find_if(palettes.cbegin(), palettes.cend(),
            [&defaultPaletteId](const ColorPalette& palette) { return palette.id == defaultPaletteId; });
        snapshot = *selected->snapshot;
        merged = std::move(effective);
    }

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
    candidate.colorRoles = std::move(colorRoles);
    candidate.palettes = std::move(palettes);
    candidate.defaultPaletteId = defaultPaletteId;
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
