using System;

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
            if (GeneratedSymbols == null)
            {
                return VnResult.VN_OK;
            }
            if (live.executionCatalogFingerprint != GeneratedExecutionFingerprint)
            {
                return VnResult.VN_VARIABLE_CATALOG_MISMATCH;
            }
            if (live.sdkCatalogFingerprint != 0 &&
                live.sdkCatalogFingerprint != GeneratedCatalogFingerprint)
            {
                DisplayMismatchCount++;
                DisplayMismatchHandler?.Invoke(
                    live.sdkCatalogFingerprint, GeneratedCatalogFingerprint);
#if UNITY_5_3_OR_NEWER
                UnityEngine.Debug.LogWarning(
                    "Harken Variable API catalog is stale; write still proceeds.");
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

        public static void Register(ulong executionFingerprint, ulong catalogFingerprint, Type symbols)
        {
            GeneratedExecutionFingerprint = executionFingerprint;
            GeneratedCatalogFingerprint = catalogFingerprint;
            GeneratedSymbols = symbols;
        }

        public static Type GeneratedSymbols { get; private set; }

        public static ulong GeneratedExecutionFingerprint { get; private set; }

        public static ulong GeneratedCatalogFingerprint { get; private set; }

        public static int DisplayMismatchCount { get; private set; }

        public static Action<ulong, ulong> DisplayMismatchHandler;

        public static QueryCatalog Query;

        public static void ResetTestState()
        {
            Query = null;
            GeneratedSymbols = null;
            GeneratedExecutionFingerprint = 0;
            GeneratedCatalogFingerprint = 0;
            DisplayMismatchCount = 0;
            DisplayMismatchHandler = null;
        }

        internal delegate VnResult QueryCatalog(ref VnVariableCatalogInfoV1 info);
    }
}
