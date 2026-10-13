#include "Common/ObjectIdTestSupport.h"
#include "harken_current_world_compiler/CurrentWorldCompiler.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/param/ParamTargetTable.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    for (const bool inlineBinding : {false, true}) {
        harkenprojectschema::CanonicalProject project{};
        harkenprojectschema::ContentObject sound{};
        sound.kind = harkenprojectschema::ContentKind::Sound;
        sound.sound.chassis.id = 40;
        if (inlineBinding) {
            sound.sound.chassis.presence.Set(
                harkenplayableproperties::PlayablePropertyIdV1::BindingAttachmentRef);
            sound.sound.chassis.bindingAttachmentRef = 9001;
        }
        project.objects.push_back(sound);
        project.roots.push_back(40);

        harken::VariableDefinitionV1 variable{};
        variable.variableId = harkentest::ObjectId(70);
        variable.kind = harken::VariableKindV1::Smooth;
        variable.rangeMax = 1.0f;

        harkencurrentworldcompiler::PropertyBindingMaterializationV1 binding{};
        binding.ownerKind = harkenplayableproperties::ContentPropertyKindV1::Sound;
        binding.ownerStableId = 40;
        binding.bindingAttachmentRef = 9001;
        harkencurrentworldcompiler::BindingCurveMaterializationV1 curve{};
        curve.curveLocalId = 3;
        curve.points = {{0.0f, 0.0f}, {1.0f, 1.0f}};
        curve.handles = {{1.0f / 3.0f, 1.0f / 3.0f,
                          2.0f / 3.0f, 2.0f / 3.0f}};
        binding.curves.push_back(curve);
        binding.bindings.push_back({7, 70, harken::kTargetScopeVoice,
                                    harken::kTargetVoiceVolume, 10, 3});
        const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
            project, {variable},
            {{harkenplayableproperties::ContentPropertyKindV1::Sound,
              40, 400, 0, 0}}, {}, {},
            inlineBinding
                ? std::vector{binding}
                : std::vector<harkencurrentworldcompiler::PropertyBindingMaterializationV1>{});
        if (!compiled.ok() || harken::ValidateCurrentWorldV1(compiled.world->View()) !=
                                  harken::WorldInstallStatus::Ok) {
            return 2;
        }
        const auto& payload = compiled.world->View().playableProperties[0].payload;
        if (payload.bindingCount != (inlineBinding ? 1u : 0u) ||
            payload.curveCount != (inlineBinding ? 1u : 0u)) {
            return 3;
        }

        const std::array encoders{
            harken::EncodeCurrentWorldBodyV5,
            harken::EncodeCurrentWorldBodyV6,
            harken::EncodeCurrentWorldBodyV7};
        for (std::size_t i = 0; i < encoders.size(); ++i) {
            const auto vintage = static_cast<std::uint32_t>(5 + i);
            std::vector<std::uint8_t> bytes;
            if (!encoders[i](compiled.world->View(), bytes)) {
                return 4;
            }
            harken::CurrentWorldBodyImage decoded;
            if (!decoded.Decode(bytes.data(), bytes.size(), vintage,
                                harken::CurrentWorldDecodeMode::ReplayRawAttempt) ||
                harken::ValidateCurrentWorldV1(decoded.View()) !=
                    harken::WorldInstallStatus::Ok ||
                decoded.View().playableProperties[0].payload.bindingCount !=
                    payload.bindingCount ||
                decoded.View().playableProperties[0].payload.curveCount !=
                    payload.curveCount) {
                return 5;
            }
            const auto name = "v" + std::to_string(vintage) +
                (inlineBinding ? "-inline-binding.bin" : "-without-inline-binding.bin");
            std::ofstream file(output / name, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (!file) {
                return 6;
            }
            std::cout << name << ": " << bytes.size()
                      << " bytes; old encode, decode and validation passed\n";
        }
    }
}
