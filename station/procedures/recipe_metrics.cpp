#include "metric_registry.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <set>

namespace ir::procedures::recipe {
using namespace ir::api;

const char *slotKindName(SlotKind kind) noexcept
{
    switch (kind) {
    case SlotKind::batch: return "batch";
    case SlotKind::double_map: return "double_map";
    case SlotKind::byte_mask: return "byte_mask";
    case SlotKind::drift_context: return "drift_context";
    }
    return "unknown";
}

std::string requireInputSlot(const Json &inputs, const char *key, SlotKind expected,
                             const std::map<std::string, SlotKind> &knownSlots,
                             const std::string &path)
{
    const auto slot = stringField(inputs, key, path + ".inputs");
    const auto found = knownSlots.find(slot);
    require(found != knownSlots.end(), IR_BAD_ARGUMENT,
            path + ".inputs." + key + " references undefined slot: " + slot);
    require(found->second == expected, IR_BAD_ARGUMENT,
            path + ".inputs." + key + " expects slot type " + slotKindName(expected) +
                ", got " + slotKindName(found->second));
    return slot;
}

std::string optionalInputSlot(const Json &inputs, const char *key, SlotKind expected,
                              const std::map<std::string, SlotKind> &knownSlots,
                              const std::string &path)
{
    if (!inputs.contains(key))
        return {};
    return requireInputSlot(inputs, key, expected, knownSlots, path);
}

std::string bindOutputSlot(const Json &outputs, const char *key, SlotKind kind, bool required,
                           std::map<std::string, SlotKind> &knownSlots, const std::string &path)
{
    if (!outputs.contains(key)) {
        require(!required, IR_BAD_ARGUMENT, path + ".outputs." + key + " is required");
        return {};
    }
    const auto slot = stringField(outputs, key, path + ".outputs");
    const auto [it, inserted] = knownSlots.emplace(slot, kind);
    if (!inserted) {
        require(it->second == kind, IR_BAD_ARGUMENT,
                path + ".outputs." + key + " cannot overwrite slot '" + slot + "' of type " +
                    slotKindName(it->second) + " with " + slotKindName(kind));
    }
    return slot;
}

const FrameBatch &getBatch(const ExecutionContext &ctx, const std::string &slot)
{
    const auto it = ctx.batches.find(slot);
    require(it != ctx.batches.end(), IR_BAD_ARGUMENT, "Missing runtime batch slot: " + slot);
    return *it->second;
}

const DoubleMap &getMap(const ExecutionContext &ctx, const std::string &slot)
{
    const auto it = ctx.maps.find(slot);
    require(it != ctx.maps.end(), IR_BAD_ARGUMENT, "Missing runtime map slot: " + slot);
    return it->second;
}

const ByteMask &getMask(const ExecutionContext &ctx, const std::string &slot)
{
    const auto it = ctx.masks.find(slot);
    require(it != ctx.masks.end(), IR_BAD_ARGUMENT, "Missing runtime mask slot: " + slot);
    return it->second;
}

const IRGjbDriftContext &getDriftContext(const ExecutionContext &ctx, const std::string &slot)
{
    const auto it = ctx.driftContexts.find(slot);
    require(it != ctx.driftContexts.end(), IR_BAD_ARGUMENT, "Missing runtime drift_ctx slot: " + slot);
    return it->second;
}

void ensureSameGeometry(IRGeometry a, IRGeometry b, const char *label)
{
    require(a.width == b.width && a.height == b.height, IR_BAD_ARGUMENT,
            std::string("Geometry mismatch in ") + label);
}

void checkMapSize(const DoubleMap &m, int64_t expectedPixels, IRGeometry g, const char *label)
{
    ensureSameGeometry(m.geometry, g, label);
    require(int64_t(m.data.size()) == expectedPixels, IR_BAD_ARGUMENT,
            std::string("Unexpected pixel buffer size in ") + label);
}

void checkMaskSize(const ByteMask &m, int64_t expectedPixels, IRGeometry g, const char *label)
{
    ensureSameGeometry(m.geometry, g, label);
    require(int64_t(m.data.size()) == expectedPixels, IR_BAD_ARGUMENT,
            std::string("Unexpected mask buffer size in ") + label);
}

void recordScalarOutputs(ExecutionContext &ctx, const std::string &name,
                         const std::vector<MetricOutput> &outputs,
                         std::span<const double> values)
{
    require(values.size() == outputs.size(), IR_COMPUTATION_FAILED,
            "Internal metric output count mismatch");
    for (size_t i = 0; i < values.size(); ++i) {
        const auto &out = outputs[i];
        const std::string key = name + out.suffix;
        require(std::isfinite(values[i]), IR_COMPUTATION_FAILED,
                "Metric produced non-finite value for: " + key);
        ctx.scalarValues[key] = values[i];
        Json entry{
            {"value", values[i]},
            {"unit", out.unit},
            {"method", out.method},
            {"version", out.version}
        };
        if (out.plugin.has_value()) {
            entry["plugin_id"] = out.plugin->id;
            entry["plugin_file"] = out.plugin->file;
        }
        ctx.metrics[key] = std::move(entry);
    }
}

IRCalcParameters parseCalcParams(const Json &params, const std::string &path)
{
    IRCalcParameters p{};
    p.t0_c = numberField(params, "t0_c", path);
    p.t1_c = numberField(params, "t1_c", path);
    p.pixel_pitch_um = optionalNumber(params, "pixel_pitch_um", 15.0, path);
    p.emissivity = optionalNumber(params, "emissivity", 1.0, path);
    p.system_gain = optionalNumber(params, "system_gain", 1.0, path);
    p.adc_swing_v = optionalNumber(params, "adc_swing_v", 2.0, path);
    p.f_number = optionalNumber(params, "f_number", 1.0, path);
    p.vmax_per_count = optionalNumber(params, "vmax_per_count", 2.0, path);
    p.integration_time_us = optionalInteger(params, "integration_time_us", 1000, 1, INT32_MAX, path);
    p.invalid_integration_us = optionalInteger(params, "invalid_integration_us", 0, 0, INT32_MAX, path);
    p.digital_full = optionalInteger(params, "digital_full", 65535, 1, INT32_MAX, path);
    p.reserved = 0;
    return p;
}

int32_t parseNoiseSubtractMode(const std::string &text, const std::string &path)
{
    if (text == "none") return IR_NOISE_SUBTRACT_NONE;
    if (text == "quadrature") return IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE;
    throw Error(IR_BAD_ARGUMENT, path + ".noise_subtract_mode must be 'none' or 'quadrature'");
}

int32_t parseWindowKind(const std::string &text, const std::string &path)
{
    if (text == "rectangular") return IR_WINDOW_RECTANGULAR;
    if (text == "hann") return IR_WINDOW_HANN;
    if (text == "hamming") return IR_WINDOW_HAMMING;
    throw Error(IR_BAD_ARGUMENT, path + ".window_kind must be rectangular, hann, or hamming");
}

namespace {

const MetricSpec *findBuiltinMethod(const std::string &method) noexcept
{
    const std::span<const MetricSpec> groups[]{
        basicMetrics(),
        standardMetrics(),
    };
    for (const auto group : groups) {
        for (const auto &spec : group) {
            if (method == spec.name)
                return &spec;
        }
    }
    return nullptr;
}

void validateOutputBindings(const Json &inputs, const Json &outputs, const std::string &path)
{
    const auto outputPath = path + ".outputs";
    std::set<std::string> boundSlots;
    for (const auto &[key, _] : outputs.items()) {
        const auto slot = stringField(outputs, key.c_str(), outputPath);
        const bool aliasesInput = std::any_of(inputs.begin(), inputs.end(), [&](const Json &input) {
            return input.is_string() && input.get_ref<const std::string &>() == slot;
        });
        require(!aliasesInput, IR_BAD_ARGUMENT,
                outputPath + "." + key + " aliases input slot '" + slot + "'");
        require(boundSlots.insert(slot).second, IR_BAD_ARGUMENT,
                outputPath + "." + key + " binds duplicate output slot '" + slot + "'");
    }
}

} // namespace

Json normalizeInputsObject(const Json &fields, const char *defaultInputKey,
                           const std::string &method, const std::string &path)
{
    Json normalizedInputs = Json::object();
    if (fields.contains("inputs"))
        normalizedInputs = objectField(fields, "inputs", path);
    if (fields.contains("input")) {
        const auto shorthand = stringField(fields, "input", path);
        require(defaultInputKey != nullptr && defaultInputKey[0] != '\0', IR_BAD_ARGUMENT,
                path + ".input shorthand is unsupported for method '" + method + "'; use inputs object");
        require(!normalizedInputs.contains(defaultInputKey), IR_BAD_ARGUMENT,
                path + ": cannot provide both input shorthand and inputs." + defaultInputKey);
        normalizedInputs[defaultInputKey] = shorthand;
    }
    return normalizedInputs;
}

AnalysisStep prepareAnalysis(StepRef ref, const Json &fields, const std::string &path,
                             std::map<std::string, SlotKind> &knownSlots,
                             const ExternalMetricResolver *externalMetrics)
{
    allowFields(fields, {"id", "type", "method", "version", "name", "input", "inputs", "params", "outputs"}, path);
    const auto method = stringField(fields, "method", path);
    const std::string stepName = fields.contains("name")
        ? stringField(fields, "name", path)
        : ref.id;
    const int32_t reqVersion = optionalInteger(fields, "version", 1, 1, 1000, path);

    if (const auto *found = findBuiltinMethod(method)) {
        const Json normalizedInputs = normalizeInputsObject(
            fields, found->defaultInputKey, method, path);
        const Json params = fields.contains("params") ? objectField(fields, "params", path) : Json::object();
        const Json outputs = fields.contains("outputs") ? objectField(fields, "outputs", path) : Json::object();

        validateOutputBindings(normalizedInputs, outputs, path);
        auto prepared = found->prepare(stepName, normalizedInputs, params, outputs, knownSlots, path);
        require(reqVersion == prepared.version, IR_BAD_ARGUMENT,
                path + ".version=" + std::to_string(reqVersion) + " does not match method version " +
                    std::to_string(prepared.version));

        return {
            std::move(ref),
            stepName,
            method,
            prepared.version,
            std::move(prepared.outputs),
            std::move(prepared.ops)
        };
    }

    if (method.rfind("plugin:", 0) == 0) {
        require(externalMetrics != nullptr && externalMetrics->handlesMethod && externalMetrics->prepareMetric,
                IR_BAD_ARGUMENT, path + ".method requires station.plugins configuration: " + method);
        require(externalMetrics->handlesMethod(method), IR_BAD_ARGUMENT,
                path + ".method is unsupported: " + method);
        return externalMetrics->prepareMetric(
            std::move(ref), stepName, method, reqVersion, fields, path, knownSlots);
    }

    throw Error(IR_BAD_ARGUMENT, path + ".method is unsupported: " + method);
}

} // namespace ir::procedures::recipe
