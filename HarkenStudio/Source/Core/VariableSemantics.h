
#pragma once

#include <QString>
#include <QStringList>
#include <QSet>
#include <QUuid>

#include <cstdint>
#include <optional>
#include <vector>

namespace VNX {

struct ProjectData;
struct ProjectItem;
class VVariable;

enum class VariableKind  : uint8_t { Step = 0, Smooth = 1 };
enum class VariableScope : uint8_t { Global = 0, Emitter = 1 };

struct VariableLabel
{
    int anchor = 0;
    QString name;
    QUuid id = QUuid::createUuid();
};

namespace VariableSemantics {

// kind governs the editing form of every declared number: a Step declaration
// is authored in whole steps, a Smooth one in fractions.
struct EditForm
{
    int   decimals;
    float quantum;
};

EditForm editFormForKind(VariableKind kind);

int discreteRead(float value);
bool discreteCellReachable(int cellValue, float rangeMin, float rangeMax);

inline constexpr int kStepComboMaxInclusiveCount = 64;
inline constexpr int kStepExactIntBound = 16777216;

bool stepComboFits(float rangeMin, float rangeMax);
int clampStepExactInt(int value);

struct StepExactSpinRange
{
    int begin = 0;
    int end = 0;
    bool valid = false;
};

StepExactSpinRange stepExactSpinRange(float rangeMin, float rangeMax);

std::optional<QString> labelTextForValue(float value, const std::vector<VariableLabel>& labels);
const VariableLabel* labelAtIndex(const std::vector<VariableLabel>& labels, int index);
const VariableLabel* labelById(const std::vector<VariableLabel>& labels, const QUuid& id);
bool validateLabels(const std::vector<VariableLabel>& labels, QString* error = nullptr);
QString labelNameError(const QString& name, const QSet<QString>& otherNames);
std::vector<VariableLabel> effectiveLabels(const std::vector<VariableLabel>& labels,
                                           float rangeMin, float rangeMax);
std::vector<VariableLabel> effectiveLabels(const VVariable& variable);
void initializeNewVariable(VVariable& variable, VariableKind kind);

float clampToRange(float value, float rangeMin, float rangeMax);

float resolveDefaultValue(float defaultValue, float rangeMin, float rangeMax);

// The one current-value read every consumer shares: rehearsal override when
// present, declared default otherwise, clamped into range at read time (a
// Step declaration also rounds), so declaration edits re-clamp stored
// overrides without touching them.
float currentValue(std::optional<float> rehearsal, VariableKind kind,
                   float rangeMin, float rangeMax, float defaultValue);

struct NormalizedDeclaration
{
    float rangeMin;
    float rangeMax;
    float defaultValue;
};

NormalizedDeclaration normalizeDeclarationForKind(VariableKind kind,
                                                  float rangeMin,
                                                  float rangeMax,
                                                  float defaultValue);

std::vector<ProjectItem*> fileBoundVariables(ProjectData* project);
QStringList restoreLoadedLabelReferences(ProjectData& project);

}
}
