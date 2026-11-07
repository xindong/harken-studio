#include <harken/HarkenVariables.h>

int main() {
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    if (Vn_Init(&config) != VN_OK) return 1;
    VnVariableCatalogInfoV1 live{};
    live.structSize = sizeof(live);
    if (Vn_QueryVariableCatalog(&live) != VN_OK) return 2;
    if (harken::GuardTypedVariableCatalog(live.executionCatalogFingerprint, 0) != VN_OK)
        return 3;
    if (harken::GuardTypedVariableCatalog(live.executionCatalogFingerprint ^ 1, 0) !=
        VN_VARIABLE_CATALOG_MISMATCH) return 4;
    VnVariableWriteV1 row{harken::generated::kVariable_VehicleRPM, 1.0f, 0};
    VnVariableBatchDescV1 batch{};
    batch.structSize = sizeof(batch);
    batch.writes = &row;
    batch.writeCount = 1;
    VnVariableTicketV1 ticket{};
    if (harken::variables::SetValues(&batch, &ticket) != VN_VARIABLE_CATALOG_MISMATCH ||
        ticket.sequence != 0) return 5;
    return Vn_Term() == VN_OK ? 0 : 6;
}
