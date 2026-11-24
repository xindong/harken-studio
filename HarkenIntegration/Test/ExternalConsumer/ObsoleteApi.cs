using Harken;

internal static class ObsoleteApi
{
    private static void Main()
    {
        HarkenEngine.StartRemoteCommunication("game", projectName: "project");
        HarkenEngine.QueryRemoteCommunication(out VnRemoteStatus _);
        HarkenEngine.StopRemoteCommunication();
        Actions.Resolve("Play", out ActionResolved resolved);
        ulong generation = resolved.PublicationGeneration;
        VnActionTargetKind kind = VnActionTargetKind.VN_ACTION_TARGET_PIECE;
        PackByteProvider.InspectInitPack(generation, null, kind, out InitPackInspection _);
    }
}
