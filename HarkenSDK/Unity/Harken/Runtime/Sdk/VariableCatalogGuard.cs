using System;
using Harken.Generated;

namespace Harken
{
    internal static class VariableCatalogGuard
    {
        public static VnResult Evaluate(in VnVariableCatalogInfoV1 live)
        {
            if (live.structSize != 24u || live.reserved0 != 0)
            {
                return VnResult.VN_INVALID_ARG;
            }
            if (live.executionCatalogFingerprint !=
                HarkenVariables.ExecutionCatalogFingerprint)
            {
                return VnResult.VN_VARIABLE_CATALOG_MISMATCH;
            }
            if (live.sdkCatalogFingerprint != 0 &&
                live.sdkCatalogFingerprint != HarkenVariables.CatalogFingerprint)
            {
                DisplayMismatchCount++;
                DisplayMismatchHandler?.Invoke(
                    live.sdkCatalogFingerprint, HarkenVariables.CatalogFingerprint);
#if UNITY_5_3_OR_NEWER
                UnityEngine.Debug.LogWarning(
                    "Harken Variable SDK catalog is stale; write still proceeds.");
#endif
            }
            return VnResult.VN_OK;
        }

        public static VnResult QueryAndEvaluate()
        {
            if (!VariableGameplay.IsGameThread)
                return VnResult.VN_INVALID_ARG;
            ulong session = VariableGameplay.SessionVersion;
            VnVariableCatalogInfoV1 info = default;
            info.structSize = 24;
            VnResult queried = Query != null
                ? Query(ref info)
                : HarkenNative.Vn_QueryVariableCatalog(ref info);
            if (queried != VnResult.VN_OK)
            {
                return queried;
            }
            VnResult result = Evaluate(in info);
            return session == VariableGameplay.SessionVersion
                ? result : VnResult.VN_NOT_INITIALIZED;
        }

        public static int DisplayMismatchCount { get; private set; }

        public static Action<ulong, ulong> DisplayMismatchHandler;

        public static QueryCatalog Query;

        public static void ResetTestState()
        {
            Query = null;
            DisplayMismatchCount = 0;
            DisplayMismatchHandler = null;
        }

        internal delegate VnResult QueryCatalog(ref VnVariableCatalogInfoV1 info);
    }
}
