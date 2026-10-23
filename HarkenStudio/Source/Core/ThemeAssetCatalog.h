#pragma once

#include "ThemePackageDto.h"

#include <QString>
#include <QVector>

namespace VNX {

namespace ThemeAssetIds {
inline constexpr char AppIcon[] = "app.icon";
inline constexpr char AppLogo[] = "app.logo";
inline constexpr char WindowMinimize[] = "window.minimize";
inline constexpr char WindowMaximize[] = "window.maximize";
inline constexpr char WindowRestore[] = "window.restore";
inline constexpr char WindowClose[] = "window.close";
inline constexpr char SidebarLeftOn[] = "shell.sidebar.left.on";
inline constexpr char SidebarLeftOff[] = "shell.sidebar.left.off";
inline constexpr char SidebarBottomOn[] = "shell.sidebar.bottom.on";
inline constexpr char SidebarBottomOff[] = "shell.sidebar.bottom.off";
inline constexpr char SidebarRightOn[] = "shell.sidebar.right.on";
inline constexpr char SidebarRightOff[] = "shell.sidebar.right.off";
inline constexpr char RemoteConnect[] = "shell.remote.connect";
inline constexpr char MonitorCapture[] = "shell.monitor.capture";
inline constexpr char PlatformEditAll[] = "shell.platform.editAll";
inline constexpr char Settings[] = "navigation.settings";
inline constexpr char DockClose[] = "dock.close";
inline constexpr char DockPin[] = "dock.pin";
inline constexpr char DockPinFilled[] = "dock.pin.filled";
inline constexpr char AuditionPlay[] = "audition.play";
inline constexpr char AuditionPause[] = "audition.pause";
inline constexpr char AuditionStop[] = "audition.stop";
inline constexpr char AuditionReset[] = "audition.reset";
inline constexpr char AuditionLoop[] = "audition.loop";
inline constexpr char BlueprintSelect[] = "blueprint.select";
inline constexpr char BlueprintPan[] = "blueprint.pan";
inline constexpr char BlueprintGridSnap[] = "blueprint.gridSnap";
inline constexpr char BlueprintChainFilter[] = "blueprint.chainFilter";
inline constexpr char BlueprintDisplayVisibility[] = "blueprint.displayVisibility";
inline constexpr char BlueprintPropertiesFilter[] = "blueprint.propertiesFilter";
inline constexpr char NavigationChevronDown[] = "navigation.chevron.down";
inline constexpr char NavigationChevronRight[] = "navigation.chevron.right";
inline constexpr char CheckboxIndeterminate[] = "checkbox.indeterminate";
inline constexpr char TreeFolderOpen[] = "tree.folder.open";
inline constexpr char TreeFolderClosed[] = "tree.folder.closed";
inline constexpr char AudioSource[] = "source.audio";
inline constexpr char Search[] = "search.search";
inline constexpr char SearchClear[] = "search.clear";
inline constexpr char TableAdd[] = "table.add";
inline constexpr char TableRemove[] = "table.remove";
inline constexpr char InspectorDetach[] = "inspector.detach";
inline constexpr char SelectorTarget[] = "selector.target";
inline constexpr char AssistantEmpty[] = "assistant.empty";
inline constexpr char AssistantSend[] = "assistant.send";
inline constexpr char AssistantStop[] = "assistant.stop";
inline constexpr char AssistantNewConversation[] = "assistant.newConversation";
inline constexpr char LogScrollToBottom[] = "log.scrollToBottom";
inline constexpr char LogCopy[] = "log.copy";
inline constexpr char LogExport[] = "log.export";
inline constexpr char LogClear[] = "log.clear";
inline constexpr char ActionStepPlay[] = "actionStep.play";
inline constexpr char ActionStepStop[] = "actionStep.stop";
inline constexpr char ActionStepSetValue[] = "actionStep.setValue";
inline constexpr char ActionStepPostAction[] = "actionStep.postAction";
inline constexpr char ActionStepReset[] = "actionStep.reset";
inline constexpr char ProjectItemFolder[] = "projectItem.folder";
inline constexpr char ProjectItemSound[] = "projectItem.sound";
inline constexpr char ProjectItemContainer[] = "projectItem.container";
inline constexpr char ProjectItemContainerMatch[] = "projectItem.container.match";
inline constexpr char ProjectItemContainerBlend[] = "projectItem.container.blend";
inline constexpr char ProjectItemContainerRandom[] = "projectItem.container.random";
inline constexpr char ProjectItemContainerRandomQueue[] = "projectItem.container.randomQueue";
inline constexpr char ProjectItemContainerSequence[] = "projectItem.container.sequence";
inline constexpr char ProjectItemContainerSequenceQueue[] = "projectItem.container.sequenceQueue";
inline constexpr char ProjectItemTrack[] = "projectItem.track";
inline constexpr char ProjectItemPiece[] = "projectItem.piece";
inline constexpr char ProjectItemLayer[] = "projectItem.layer";
inline constexpr char ProjectItemBus[] = "projectItem.bus";
inline constexpr char ProjectItemAction[] = "projectItem.action";
inline constexpr char ProjectItemVariable[] = "projectItem.variable";
inline constexpr char ProjectItemVariableSmooth[] = "projectItem.variable.smooth";
inline constexpr char ProjectItemVariableStep[] = "projectItem.variable.step";
inline constexpr char ProjectItemAttenuation[] = "projectItem.attenuation";
inline constexpr char MeterPeakCap[] = "meter.peak.cap";
}

enum class ThemeBuiltinAssetFallback
{
    File,
    TransparentPixel
};

struct ThemeAssetCatalogEntry
{
    QString logicalId;
    ThemeAssetKind kind = ThemeAssetKind::Image;
    ThemeBuiltinAssetFallback fallback = ThemeBuiltinAssetFallback::File;
    QString builtinPath;
    bool themeOverridable = true;
};

const QVector<ThemeAssetCatalogEntry>& themeAssetCatalog();
const ThemeAssetCatalogEntry* findThemeAssetCatalogEntry(const QString& logicalId);

}
