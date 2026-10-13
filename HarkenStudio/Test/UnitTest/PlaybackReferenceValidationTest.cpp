#include "ActionManager.h"
#include "CanonicalRuntimeProjectAdapter.h"
#include "CanonicalRuntimeProjectAdapterPrivate.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "PlayablePropertyAuthoringV1.h"
#include "PlaybackReferenceValidation.h"
#include "ProjectManager.h"
#include "ProjectPackExporter.h"
#include "ProjectSerializer.h"
#include "PropertyEditSession.h"
#include "Timeline/TimelineEditing.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <functional>
#include <tuple>

using namespace VNX;

namespace {

QString guidText(const QUuid& guid)
{
    return guid.toString(QUuid::WithoutBraces);
}

QMap<QString, QByteArray> projectFiles(const QString& path)
{
    QMap<QString, QByteArray> files;
    const QDir root(QFileInfo(path).absolutePath());
    QDirIterator it(root.path(), {QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile file(it.next());
        if (!file.open(QIODevice::ReadOnly)) return {};
        files.insert(root.relativeFilePath(file.fileName()), file.readAll());
    }
    return files;
}

bool editNode(QJsonObject& node, const QUuid& guid,
              const std::function<void(QJsonObject&)>& edit)
{
    if (QUuid(node.value(QStringLiteral("guid")).toString()) == guid) {
        edit(node);
        return true;
    }
    QJsonArray children = node.value(QStringLiteral("children")).toArray();
    for (qsizetype i = 0; i < children.size(); ++i) {
        QJsonObject child = children[i].toObject();
        if (!editNode(child, guid, edit)) continue;
        children[i] = child;
        node[QStringLiteral("children")] = children;
        return true;
    }
    return false;
}

enum class ReferenceKind { Selection, Discrete, Continuous, Source, Target, Clip };
enum class InvalidTarget { External, Grandchild, Missing, OnlyInCurrent, Variable, Bus, Ancestor };

QString referenceName(ReferenceKind kind)
{
    switch (kind) {
    case ReferenceKind::Selection:
    case ReferenceKind::Discrete:
    case ReferenceKind::Continuous: return QStringLiteral("Selection");
    case ReferenceKind::Source: return QStringLiteral("Transition");
    case ReferenceKind::Target: return QStringLiteral("Transition");
    case ReferenceKind::Clip: return QStringLiteral("Clip");
    }
    return {};
}

}

class PlaybackReferenceValidationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char name[] = "harken_editor_tests";
            static char* argv[] = {name, nullptr};
            static QApplication app(argc, argv);
        }
        auto& pm = ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &pm, nullptr, {});
        registerContainerActions(ActionManager::instance(), &pm);
        ASSERT_TRUE(directory_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("PlaybackReferences");
        config.location = directory_.path();
        ASSERT_TRUE(pm.createProject(config));
        layer_ = pm.createItem(nullptr, QStringLiteral("Layer_R"), ProjectItem::Type::Layer);
        ASSERT_NE(layer_, nullptr);
        owner_ = pm.createItem(layer_, QStringLiteral("Container_A"), ProjectItem::Type::Container);
        ASSERT_NE(owner_, nullptr);
        sound_ = pm.createItem(owner_, QStringLiteral("Sound_A"), ProjectItem::Type::Sound);
        other_ = pm.createItem(owner_, QStringLiteral("Sound_B"), ProjectItem::Type::Sound);
        nested_ = pm.createItem(owner_, QStringLiteral("Container_N"), ProjectItem::Type::Container);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(other_, nullptr);
        ASSERT_NE(nested_, nullptr);
        grandchild_ = pm.createItem(nested_, QStringLiteral("Sound_N"), ProjectItem::Type::Sound);
        track_ = pm.createItem(nested_, QStringLiteral("Track_A"), ProjectItem::Type::Track);
        ASSERT_NE(grandchild_, nullptr);
        ASSERT_NE(track_, nullptr);
        trackSound_ = pm.createItem(track_, QStringLiteral("Sound_T"), ProjectItem::Type::Sound);
        auto* trackContainer = pm.createItem(track_, QStringLiteral("Container_T"), ProjectItem::Type::Container);
        ASSERT_NE(trackSound_, nullptr);
        ASSERT_NE(trackContainer, nullptr);
        trackGrandchild_ = pm.createItem(trackContainer, QStringLiteral("Sound_TN"), ProjectItem::Type::Sound);
        external_ = pm.createItem(layer_, QStringLiteral("Sound_X"), ProjectItem::Type::Sound);
        variable_ = pm.createItem(nullptr, QStringLiteral("Variable"), ProjectItem::Type::Variable);
        ASSERT_NE(trackGrandchild_, nullptr);
        ASSERT_NE(external_, nullptr);
        ASSERT_NE(variable_, nullptr);
        auto* selection = owner_->comp<VNX::VSelection>();
        TransitionRuleDef rule;
        rule.source = sound_->guid;
        rule.target = other_->guid;
        rule.bridge = external_->guid;
        ASSERT_TRUE(selection->transitions.addRule(rule));
        Clip clip{QUuid::createUuid(), trackSound_->guid, 10, 200};
        ASSERT_TRUE(TimelineEditing::addClip(&pm, track_, clip).success);
        ASSERT_TRUE(pm.saveProject());
        projectPath_ = pm.currentProject()->projectPath;
        UndoRedoManager::instance().clear();
        connection_ = QObject::connect(&pm, &ProjectManager::errorOccurred,
            [this](const QString& error) { errors_.push_back(error); });
    }

    void TearDown() override
    {
        QObject::disconnect(connection_);
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    ActionResult setOccurrences(const QJsonObject& value, ActionSource source = ActionSource::GUI)
    {
        return ActionManager::instance().execute(QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(owner_->objectID)},
             {QStringLiteral("property"), QStringLiteral("selections")},
             {QStringLiteral("value"), value}}, source);
    }

    void configureSelection(SelectionMode mode)
    {
        auto* selection = owner_->comp<VSelection>();
        selection->mode = mode;
        selection->matchResponse = VariableResponse::AtStart;
        selection->blendResponse = VariableResponse::Live;
        ASSERT_TRUE(selection->selections.shared.setMatchGrid({}));
        ASSERT_TRUE(selection->selections.shared.setMixing({}));
        variable_->comp<VVariable>()->kind = mode == SelectionMode::Match
            ? VariableKind::Step : VariableKind::Smooth;
        if (mode == SelectionMode::Match) {
            DiscreteGridDef grid;
            grid.axes = {variable_->guid};
            ASSERT_TRUE(grid.addRow(sound_->guid));
            ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
        } else if (mode == SelectionMode::Blend) {
            MixingDef mixing;
            mixing.rows.push_back({variable_->guid,
                {MixingChildDef::unity(sound_->guid, 0.0f, 100.0f)}});
            ASSERT_TRUE(selection->selections.shared.setMixing(mixing));
        }
        owner_->markDirty();
        variable_->markDirty();
    }

    ActionResult setProperty(ProjectItem* item, const QString& property, const QJsonValue& value)
    {
        return ActionManager::instance().execute(QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
             {QStringLiteral("property"), property}, {QStringLiteral("value"), value}}, ActionSource::IPC);
    }

    void inject(ReferenceKind kind, const QUuid& target)
    {
        auto* selection = owner_->comp<VNX::VSelection>();
        switch (kind) {
        case ReferenceKind::Selection:
        case ReferenceKind::Discrete:
        case ReferenceKind::Continuous: selection->selections.shared.member(sound_->guid)->childGuid = target; break;
        case ReferenceKind::Source: selection->transitions.rules.back().source = target; break;
        case ReferenceKind::Target: selection->transitions.rules.back().target = target; break;
        case ReferenceKind::Clip: track_->comp<VTrack>()->clips.items.front().targetGuid = target; break;
        }
    }

    bool rewriteFile(const QString& path, const QUuid& ownerGuid,
                     const std::function<void(QJsonObject&)>& edit)
    {
        const QDir root(QFileInfo(path).absolutePath());
        for (const auto& relative : projectFiles(path).keys()) {
            QFile file(root.filePath(relative));
            if (!file.open(QIODevice::ReadOnly)) return false;
            QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            QJsonObject content = document.value(QStringLiteral("content")).toObject();
            if (!editNode(content, ownerGuid, edit)) continue;
            document[QStringLiteral("content")] = content;
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            const QByteArray bytes = QJsonDocument(document).toJson();
            return file.write(bytes) == bytes.size();
        }
        return false;
    }

    QString copyProject()
    {
        const QString root = directory_.path() + QStringLiteral("/CandidateQ");
        const auto files = projectFiles(projectPath_);
        for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
            const QString destination = QDir(root).filePath(it.key());
            if (!QDir().mkpath(QFileInfo(destination).absolutePath())) return {};
            QFile file(destination);
            if (!file.open(QIODevice::WriteOnly) || file.write(it.value()) != it.value().size()) return {};
        }
        return QDir(root).filePath(QFileInfo(projectPath_).fileName());
    }

    QTemporaryDir directory_;
    ProjectItem* layer_ = nullptr;
    ProjectItem* owner_ = nullptr;
    ProjectItem* sound_ = nullptr;
    ProjectItem* other_ = nullptr;
    ProjectItem* nested_ = nullptr;
    ProjectItem* grandchild_ = nullptr;
    ProjectItem* track_ = nullptr;
    ProjectItem* trackSound_ = nullptr;
    ProjectItem* trackGrandchild_ = nullptr;
    ProjectItem* external_ = nullptr;
    ProjectItem* variable_ = nullptr;
    QString projectPath_;
    QStringList errors_;
    QMetaObject::Connection connection_;
};

TEST_F(PlaybackReferenceValidationTest, LateChildrenAndRepeatedOccurrencesRoundTrip)
{
    auto& pm = ProjectManager::instance();
    ASSERT_TRUE(setProperty(sound_, QStringLiteral("loopCount"), 3).success);
    SelectionEntry extra;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*owner_, sound_->guid, {}, &extra));
    auto& rows = owner_->comp<VNX::VSelection>()->selections.shared.member(sound_->guid)->occurrences;
    rows.front().weight = 30;
    rows.back().weight = 70;
    const auto expected = owner_->comp<VNX::VSelection>()->selections.shared;
    const auto ownerGuid = owner_->guid;
    const auto soundGuid = sound_->guid;
    owner_->markDirty();
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(pm.loadProject(projectPath_));
    owner_ = pm.findItemByGuid(ownerGuid);
    ASSERT_NE(owner_, nullptr);
    EXPECT_EQ(owner_->comp<VNX::VSelection>()->selections.shared, expected);
    const auto* loadedSound = pm.findItemByGuid(soundGuid);
    ASSERT_NE(loadedSound, nullptr);
    EXPECT_EQ(loadedSound->parent, owner_);
    ASSERT_NE(loadedSound->comp<VAudio>(), nullptr);
    EXPECT_EQ(loadedSound->comp<VAudio>()->loopCount, 3u);
    EXPECT_TRUE(validatePlaybackReferences(*pm.currentProject()));
    auto& loaded = owner_->comp<VNX::VSelection>()->selections.shared;
    auto invalid = loaded;
    invalid.member(soundGuid)->occurrences.back().stableOccurrenceOrdinal =
        invalid.member(soundGuid)->occurrences.front().stableOccurrenceOrdinal;
    SelectionData candidate;
    candidate.shared = invalid;
    EXPECT_FALSE(setOccurrences(candidate.toJson()).success);
    EXPECT_EQ(loaded, expected);
    EXPECT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*owner_, soundGuid));
}

TEST_F(PlaybackReferenceValidationTest, PartialAndEmptySelectionsRemainExplicit)
{
    auto& pm = ProjectManager::instance();
    const auto ownerGuid = owner_->guid;
    for (size_t count : {size_t{1}, size_t{0}}) {
        SCOPED_TRACE(count);
        auto candidate = owner_->comp<VNX::VSelection>()->selections;
        auto rows = candidate.shared.occurrenceRows();
        rows.resize(count);
        ASSERT_TRUE(candidate.shared.setOccurrenceRows(rows));
        ASSERT_TRUE(setOccurrences(candidate.toJson()).success);
        ASSERT_TRUE(pm.saveProject());
        ASSERT_TRUE(pm.loadProject(projectPath_));
        owner_ = pm.findItemByGuid(ownerGuid);
        ASSERT_NE(owner_, nullptr);
        EXPECT_EQ(owner_->children.size(), 3u);
        EXPECT_EQ(owner_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), count);
        EXPECT_TRUE(validatePlaybackReferences(*pm.currentProject()));
    }
}

TEST_F(PlaybackReferenceValidationTest, InvalidSelectionTargetsAreRejectedAtomically)
{
    auto* stack = UndoRedoManager::instance().undoStack();
    const int history = stack->count();
    for (const auto kind : {ReferenceKind::Discrete, ReferenceKind::Continuous}) {
        configureSelection(kind == ReferenceKind::Discrete ? SelectionMode::Match : SelectionMode::Blend);
        const auto before = owner_->comp<VSelection>()->toJson();
        for (const QJsonValue target : {QJsonValue(QJsonValue::Null), QJsonValue(QStringLiteral("invalid"))}) {
            auto value = before.value(QStringLiteral("selections")).toObject();
            auto table = value.value(QStringLiteral("shared")).toObject();
            auto entries = table.value(QStringLiteral("entries")).toArray();
            auto entry = entries[0].toObject();
            entry[QStringLiteral("childGuid")] = target;
            entries[0] = entry;
            table[QStringLiteral("entries")] = entries;
            value[QStringLiteral("shared")] = table;
            EXPECT_FALSE(setOccurrences(value).success);
            EXPECT_EQ(owner_->comp<VSelection>()->toJson(), before);
            EXPECT_EQ(stack->count(), history);
        }
    }
}

TEST_F(PlaybackReferenceValidationTest, PreparedCandidateIsRevalidatedBeforeCommit)
{
    auto& pm = ProjectManager::instance();
    auto prepared = ProjectManager::prepareProject(projectPath_);
    ASSERT_NE(prepared.project, nullptr) << prepared.error.toStdString();
    ASSERT_FALSE(prepared.project->layers.empty());
    auto stagedLayer = prepared.project->layers.front().rootItem;
    ASSERT_NE(stagedLayer, nullptr);
    ASSERT_EQ(stagedLayer->guid, layer_->guid);
    ASSERT_FALSE(stagedLayer->children.empty());
    auto stagedOwner = stagedLayer->children.front();
    ASSERT_EQ(stagedOwner->guid, owner_->guid);
    ASSERT_NE(stagedOwner.get(), owner_);
    stagedOwner->comp<VNX::VSelection>()->selections.shared.entries.front().childGuid = external_->guid;
    ASSERT_TRUE(setProperty(owner_, QStringLiteral("mode"), static_cast<int>(SelectionMode::Sequence)).success);
    auto* current = pm.currentProject();
    const auto ledger = owner_->comp<VNX::VSelection>()->selections.shared;
    const auto files = projectFiles(projectPath_);
    auto* stack = UndoRedoManager::instance().undoStack();
    const int history = stack->count();
    const int index = stack->index();
    EXPECT_FALSE(pm.commitPreparedProject(std::move(prepared)));
    EXPECT_EQ(pm.currentProject(), current);
    EXPECT_EQ(owner_->comp<VNX::VSelection>()->selections.shared, ledger);
    EXPECT_TRUE(owner_->dirtyBit);
    EXPECT_EQ(stack->count(), history);
    EXPECT_EQ(stack->index(), index);
    EXPECT_EQ(projectFiles(projectPath_), files);
}

class PlaybackReferenceModeTest : public PlaybackReferenceValidationTest,
    public ::testing::WithParamInterface<SelectionMode> {};

TEST_P(PlaybackReferenceModeTest, AllDirectContentKindsReachCompilationAfterReload)
{
    auto& pm = ProjectManager::instance();
    configureSelection(GetParam());
    auto* directTrack = pm.createItem(owner_, QStringLiteral("DirectTrack"), ProjectItem::Type::Track);
    auto* directPiece = pm.createItem(owner_, QStringLiteral("DirectPiece"), ProjectItem::Type::Piece);
    auto* clipPiece = pm.createItem(track_, QStringLiteral("ClipPiece"), ProjectItem::Type::Piece);
    ASSERT_NE(directTrack, nullptr);
    ASSERT_NE(directPiece, nullptr);
    ASSERT_NE(clipPiece, nullptr);
    auto selection = *owner_->comp<VNX::VSelection>();
    auto grid = selection.matchGrid();
    auto mixing = selection.mixing();
    for (auto* target : {nested_, directTrack, directPiece}) {
        if (GetParam() == SelectionMode::Match) {
            ASSERT_TRUE(grid.addRow(target->guid));
        } else {
            mixing.rows.front().children.push_back(
                MixingChildDef::unity(target->guid, 0.0f, 100.0f));
        }
        TransitionRuleDef rule;
        rule.source = target->guid;
        rule.target = sound_->guid;
        rule.exitTiming = TransitionExitTiming::Immediate;
        ASSERT_TRUE(selection.transitions.addRule(rule));
    }
    if (GetParam() == SelectionMode::Match) {
        ASSERT_TRUE(selection.selections.shared.setMatchGrid(grid));
    } else {
        ASSERT_TRUE(selection.selections.shared.setMixing(mixing));
    }
    ASSERT_TRUE(setOccurrences(selection.selections.toJson()).success);
    if (GetParam() == SelectionMode::Match)
        ASSERT_TRUE(setProperty(owner_, QStringLiteral("transitions"), selection.transitions.toJson()).success);
    for (const auto& child : track_->children) {
        if (child->type == ProjectItem::Type::Sound) continue;
        Clip clip{QUuid::createUuid(), child->guid, 220, 200};
        ASSERT_TRUE(TimelineEditing::addClip(&pm, track_, clip).success);
    }
    const auto expectedSelection = owner_->comp<VNX::VSelection>()->toJson();
    const auto expectedOccurrences = owner_->comp<VNX::VSelection>()->selections.shared;
    const auto ownerGuid = owner_->guid;
    const auto trackGuid = track_->guid;
    ASSERT_TRUE(pm.saveProject());
    const auto expectedClips = track_->comp<VTrack>()->clips;
    ASSERT_TRUE(pm.loadProject(projectPath_));
    owner_ = pm.findItemByGuid(ownerGuid);
    track_ = pm.findItemByGuid(trackGuid);
    ASSERT_NE(owner_, nullptr);
    ASSERT_NE(track_, nullptr);
    EXPECT_EQ(owner_->comp<VNX::VSelection>()->toJson(), expectedSelection);
    EXPECT_EQ(owner_->comp<VNX::VSelection>()->selections.shared, expectedOccurrences);
    EXPECT_EQ(track_->comp<VTrack>()->clips, expectedClips);
    QString error;
    EXPECT_TRUE(validatePlaybackReferences(*pm.currentProject(), &error)) << error.toStdString();
    const auto context = HarkenEditor::Bridge::canonicalRuntimeProjectContext(*pm.currentProject());
    ASSERT_TRUE(context.ok()) << (context.diagnostics.empty() ? "" : context.diagnostics.front().message.toStdString());
}

INSTANTIATE_TEST_SUITE_P(MatchAndBlend, PlaybackReferenceModeTest,
    ::testing::Values(SelectionMode::Match, SelectionMode::Blend));

TEST_F(PlaybackReferenceValidationTest, FailedSavePreservesActivePropertyEditAndHistory)
{
    auto& pm = ProjectManager::instance();
    PropertyEditSession edit(sound_, QStringLiteral("volume"), sound_->comp<VMixNode>()->volume);
    edit.preview(-4.0f);
    ASSERT_TRUE(edit.isActive());
    ASSERT_FLOAT_EQ(sound_->comp<VMixNode>()->volume, -4.0f);
    const auto ledger = owner_->comp<VNX::VSelection>()->selections.shared;
    inject(ReferenceKind::Selection, external_->guid);
    auto* stack = UndoRedoManager::instance().undoStack();
    const int history = stack->count();
    const int index = stack->index();
    const bool dirty = sound_->dirtyBit;
    const auto files = projectFiles(projectPath_);
    EXPECT_FALSE(pm.saveProject());
    EXPECT_TRUE(edit.isActive());
    EXPECT_FLOAT_EQ(sound_->comp<VMixNode>()->volume, -4.0f);
    EXPECT_EQ(sound_->dirtyBit, dirty);
    EXPECT_EQ(stack->count(), history);
    EXPECT_EQ(stack->index(), index);
    EXPECT_EQ(projectFiles(projectPath_), files);
    owner_->comp<VNX::VSelection>()->selections.shared = ledger;
    ASSERT_TRUE(pm.saveProject());
    EXPECT_FALSE(edit.isActive());
    EXPECT_EQ(stack->count(), history + 1);
    EXPECT_FLOAT_EQ(sound_->comp<VMixNode>()->volume, -4.0f);
}

class PlaybackReferenceBoundaryTest : public PlaybackReferenceValidationTest,
    public ::testing::WithParamInterface<std::tuple<ReferenceKind, InvalidTarget>> {};

TEST_P(PlaybackReferenceBoundaryTest, RejectsInvalidReferencesAcrossBoundariesAtomically)
{
    const auto [kind, invalidTarget] = GetParam();
    auto& pm = ProjectManager::instance();
    configureSelection(kind == ReferenceKind::Continuous ? SelectionMode::Blend
        : kind == ReferenceKind::Discrete || kind == ReferenceKind::Source || kind == ReferenceKind::Target
            ? SelectionMode::Match : SelectionMode::Sequence);
    ASSERT_TRUE(pm.saveProject());
    const QString candidatePath = copyProject();
    ASSERT_FALSE(candidatePath.isEmpty());
    auto* borrowed = pm.createItem(layer_, QStringLiteral("OnlyInP"), ProjectItem::Type::Sound);
    ASSERT_NE(borrowed, nullptr);
    QUuid target;
    switch (invalidTarget) {
    case InvalidTarget::External: target = external_->guid; break;
    case InvalidTarget::Grandchild: target = kind == ReferenceKind::Clip ? trackGrandchild_->guid : grandchild_->guid; break;
    case InvalidTarget::Missing: target = QUuid::createUuid(); break;
    case InvalidTarget::OnlyInCurrent: target = borrowed->guid; break;
    case InvalidTarget::Variable: target = variable_->guid; break;
    case InvalidTarget::Bus: target = pm.currentProject()->busses.front().rootItem->guid; break;
    case InvalidTarget::Ancestor: target = kind == ReferenceKind::Clip ? nested_->guid : owner_->guid; break;
    }
    ASSERT_TRUE(setProperty(owner_, QStringLiteral("volume"), -1.0).success);
    const auto beforeLedger = owner_->comp<VNX::VSelection>()->selections.shared;
    const auto beforeSelection = *owner_->comp<VNX::VSelection>();
    const auto beforeClips = track_->comp<VTrack>()->clips;
    const bool wasDirty = owner_->dirtyBit;
    auto* stack = UndoRedoManager::instance().undoStack();
    const int historyCount = stack->count();
    const int historyIndex = stack->index();
    const bool dirtyBeforeReject = owner_->dirtyBit;
    inject(kind, target);
    const auto candidateLedger = owner_->comp<VNX::VSelection>()->selections.shared;
    const auto candidateSelectionJson = owner_->comp<VNX::VSelection>()->toJson();
    const auto candidateClips = track_->comp<VTrack>()->clips;
    owner_->comp<VNX::VSelection>()->selections.shared = beforeLedger;
    *owner_->comp<VNX::VSelection>() = beforeSelection;
    track_->comp<VTrack>()->clips = beforeClips;

    ActionResult result;
    if (kind == ReferenceKind::Selection) {
        auto candidate = owner_->comp<VNX::VSelection>()->selections;
        candidate.shared = candidateLedger;
        result = setOccurrences(candidate.toJson(), ActionSource::IPC);
        EXPECT_FALSE(PlayablePropertyAuthoringV1::validateContainerOccurrenceTargets(*owner_, candidateLedger));
        EXPECT_FALSE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*owner_, target));
        EXPECT_FALSE(PlayablePropertyAuthoringV1::replaceContainerChildOccurrence(
            *owner_, beforeLedger.occurrenceRows().front().stableOccurrenceOrdinal, target));
    } else if (kind == ReferenceKind::Clip) {
        result = setProperty(track_, QStringLiteral("clips"), candidateClips.toJsonObject());
        EXPECT_FALSE(TimelineEditing::replaceClips(&pm, track_, candidateClips).success);
    } else if (kind == ReferenceKind::Discrete || kind == ReferenceKind::Continuous) {
        result = setOccurrences(candidateSelectionJson.value(QStringLiteral("selections")).toObject(), ActionSource::IPC);
    } else {
        result = setProperty(owner_, QStringLiteral("transitions"), candidateSelectionJson.value(QStringLiteral("transitions")));
    }
    EXPECT_FALSE(result.success);
    EXPECT_EQ(owner_->comp<VNX::VSelection>()->selections.shared, beforeLedger);
    EXPECT_EQ(owner_->comp<VNX::VSelection>()->toJson(), beforeSelection.toJson());
    EXPECT_EQ(track_->comp<VTrack>()->clips, beforeClips);
    EXPECT_EQ(owner_->dirtyBit, dirtyBeforeReject);
    EXPECT_EQ(stack->count(), historyCount);
    EXPECT_EQ(stack->index(), historyIndex);

    const QUuid ownerGuid = kind == ReferenceKind::Clip ? track_->guid : owner_->guid;
    const QString ownerName = kind == ReferenceKind::Clip ? track_->name : owner_->name;
    ASSERT_TRUE(rewriteFile(candidatePath, ownerGuid, [&](QJsonObject& node) {
        if (kind == ReferenceKind::Selection) {
            auto components = node.value("components").toObject();
            auto selection = components.value("selection").toObject();
            auto selections = selection.value("selections").toObject();
            QJsonObject ledger = selections.value("shared").toObject();
            QJsonArray rows = ledger.value(QStringLiteral("entries")).toArray();
            QJsonObject row = rows.at(0).toObject();
            row[QStringLiteral("childGuid")] = guidText(target);
            rows[0] = row;
            ledger[QStringLiteral("entries")] = rows;
            selections["shared"] = ledger;
            selection["selections"] = selections;
            components["selection"] = selection;
            node["components"] = components;
        } else {
            QJsonObject components = node.value(QStringLiteral("components")).toObject();
            const QString key = kind == ReferenceKind::Clip ? QStringLiteral("tracktimeline") : QStringLiteral("selection");
            QJsonObject component = components.value(key).toObject();
            if (kind == ReferenceKind::Clip) {
                QJsonArray clips = component.value(QStringLiteral("clips")).toArray();
                QJsonObject clip = clips.at(0).toObject();
                clip[QStringLiteral("targetGuid")] = guidText(target);
                clips[0] = clip;
                component[QStringLiteral("clips")] = clips;
            } else if (kind == ReferenceKind::Discrete || kind == ReferenceKind::Continuous) {
                component[QStringLiteral("selections")] = candidateSelectionJson.value(QStringLiteral("selections"));
            } else {
                component[QStringLiteral("transitions")] = candidateSelectionJson.value(QStringLiteral("transitions"));
            }
            components[key] = component;
            node[QStringLiteral("components")] = components;
        }
    }));
    const auto diskQ = projectFiles(candidatePath);
    const auto diskP = projectFiles(projectPath_);
    auto* current = pm.currentProject();
    EXPECT_FALSE(pm.loadProject(candidatePath));
    ASSERT_EQ(pm.currentProject(), current);
    EXPECT_EQ(pm.findItemByGuid(owner_->guid), owner_);
    EXPECT_EQ(owner_->dirtyBit, dirtyBeforeReject);
    EXPECT_EQ(stack->count(), historyCount);
    EXPECT_EQ(stack->index(), historyIndex);
    EXPECT_EQ(projectFiles(candidatePath), diskQ);
    ASSERT_FALSE(errors_.isEmpty());
    const auto error = errors_.back();
    EXPECT_TRUE(error.contains(ownerName)) << error.toStdString();
    EXPECT_TRUE(error.contains(guidText(ownerGuid))) << error.toStdString();
    EXPECT_TRUE(error.contains(guidText(target))) << error.toStdString();
    EXPECT_TRUE(error.contains(referenceName(kind), Qt::CaseInsensitive)) << error.toStdString();

    inject(kind, target);
    owner_->markDirty();
    track_->markDirty();
    QString validationError;
    EXPECT_FALSE(validatePlaybackReferences(*current, &validationError));
    EXPECT_EQ(pm.duplicateItem(kind == ReferenceKind::Clip ? track_ : owner_), nullptr);
    EXPECT_FALSE(pm.saveProject());
    ProjectSerializer serializer;
    EXPECT_FALSE(serializer.prepareProjectSave(projectPath_, *current));
    EXPECT_EQ(projectFiles(projectPath_), diskP);
    EXPECT_TRUE(owner_->dirtyBit);
    EXPECT_TRUE(track_->dirtyBit);
    EXPECT_EQ(stack->count(), historyCount);
    EXPECT_EQ(stack->index(), historyIndex);
    EXPECT_FALSE(HarkenEditor::Bridge::canonicalRuntimeProjectContext(*current).ok());
    ProjectPackExporter exporter;
    EXPECT_FALSE(exporter.ValidateBeforeBuild(*current).valid);
    owner_->comp<VNX::VSelection>()->selections.shared = beforeLedger;
    *owner_->comp<VNX::VSelection>() = beforeSelection;
    track_->comp<VTrack>()->clips = beforeClips;
    owner_->dirtyBit = wasDirty;
}

INSTANTIATE_TEST_SUITE_P(AllReferences, PlaybackReferenceBoundaryTest,
    ::testing::Combine(
        ::testing::Values(ReferenceKind::Selection, ReferenceKind::Discrete,
                          ReferenceKind::Continuous, ReferenceKind::Source,
                          ReferenceKind::Target, ReferenceKind::Clip),
        ::testing::Values(InvalidTarget::External, InvalidTarget::Grandchild,
                          InvalidTarget::Missing, InvalidTarget::OnlyInCurrent,
                          InvalidTarget::Variable,
                          InvalidTarget::Bus, InvalidTarget::Ancestor)));
