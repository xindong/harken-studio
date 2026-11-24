#pragma warning disable 618
using Harken;
using Harken.Unity;
using Harken.Unity.Editor;
using UnityEngine;

internal static class ObsoleteApi
{
    private static void Runtime(HarkenAudioSystem system, HarkenPackSource asset, TextAsset init)
    {
        system.ConfigurePlatformBeforeStartup(null, "Windows");
        asset.CaptureCompanion(init, out PackSource source, out string _);
        system.ConfigureCompanionSourceBeforeStartup(source);
        system.ConfigureCompanionSourceBeforeStartup(HarkenStreamingPackSource.Create("Windows", "en"));
        system.RetryRemoteCommunication();
        system.QueryRemoteCommunication(out VnRemoteStatus _);
        HarkenProjectSettings settings = HarkenProjectSettings.Load();
        uint capacity = settings.MaxPacks + settings.MaxActions + settings.MaxActionFirings
            + settings.MaxEmitters + settings.EmitterVariableSlots
            + HarkenProjectSettings.DefaultMaxPacks + HarkenProjectSettings.DefaultMaxActions
            + HarkenProjectSettings.DefaultMaxActionFirings + HarkenProjectSettings.UnlimitedActionFirings;
        Debug.Log(settings.PlatformId + capacity);
    }

    private static void EditorTools(string[] names)
    {
        var catalog = new HarkenEditorCatalog();
        catalog.RefreshPlatform("GeneratedPacks", "", "Windows", "Windows");
        HarkenPreparedPacks packs = catalog.PreparedPacks;
        GameplayPublication publication = catalog.Publication ?? packs.Publication;
        foreach (GameplayAction action in publication.Actions) Debug.Log(action.Name);
        publication.TryFindPack(publication.InitPackStableId, out PackManifestEntry _);
        publication.GetActionPreparationOrder(names, publication.ProjectId, out ulong[] ids);
        catalog.GetActionPreview(names[0], out HarkenActionPreview preview, out string _);
        GameplayAction previewed = preview.Action;
        Debug.Log(previewed.ActionStableId == preview.Action.ActionStableId);
        var editorPreview = new HarkenEditorPreview();
        editorPreview.Prepare(packs, ids);
        editorPreview.CreateScope().Prepare(packs, names);
    }
}
