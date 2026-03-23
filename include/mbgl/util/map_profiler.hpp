#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifndef MLN_MAP_PROFILER_ENABLE
#define MLN_MAP_PROFILER_ENABLE 0
#endif

#if defined(__APPLE__) && __has_include(<os/signpost.h>)
#include <os/signpost.h>
#define MLN_MAP_PROFILER_SIGNPOST 1
#else
#define MLN_MAP_PROFILER_SIGNPOST 0
#endif

#ifdef MLN_TRACY_ENABLE
#include <tracy/TracyC.h>
#define MLN_MAP_PROFILER_TRACY 1
#else
#define MLN_MAP_PROFILER_TRACY 0
#endif

namespace mbgl::util::map_profiler {

enum class Stage : uint8_t {
    StyleLayerParse,
    StylePropertyParse,
    ExpressionParse,
    ExpressionEvaluate,
    RenderLayerEvaluate,
    TileLayerParse,
    TileFilterEvaluate,
};

struct SnapshotEntry {
    Stage stage = Stage::ExpressionEvaluate;
    std::string layer;
    std::string property;
    std::string detail;
    uint64_t count = 0;
    uint64_t totalDurationNs = 0;
    uint64_t maxDurationNs = 0;
};

struct Snapshot {
    uint64_t windowStartNs = 0;
    uint64_t windowEndNs = 0;
    uint64_t droppedSamples = 0;
    std::vector<SnapshotEntry> entries;
};

void retainSink() noexcept;
void releaseSink() noexcept;
bool isEnabled() noexcept;

void setEvalSampleRate(uint32_t sampleRate) noexcept;
uint32_t getEvalSampleRate() noexcept;

const char* stageToString(Stage) noexcept;

class ScopedContext {
public:
    ScopedContext(const char* layer = nullptr, const char* property = nullptr, const char* detail = nullptr) noexcept;
    ~ScopedContext();

    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

private:
    const char* previousLayer = nullptr;
    const char* previousProperty = nullptr;
    const char* previousDetail = nullptr;
    uint64_t previousLayerHash = 0;
    uint64_t previousPropertyHash = 0;
    uint64_t previousDetailHash = 0;
};

class ScopedEvent {
public:
    ScopedEvent(Stage stage, const char* detail = nullptr) noexcept;
    ~ScopedEvent();

    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;

private:
    Stage stage;
    bool active = false;
    const char* layer = nullptr;
    const char* property = nullptr;
    const char* detail = nullptr;
    uint64_t layerHash = 0;
    uint64_t propertyHash = 0;
    uint64_t detailHash = 0;
    uint64_t startNs = 0;

#if MLN_MAP_PROFILER_SIGNPOST
    os_signpost_id_t signpostID = OS_SIGNPOST_ID_INVALID;
    bool signpostActive = false;
#endif

#if MLN_MAP_PROFILER_TRACY
    TracyCZoneCtx tracyCtx;
#endif
};

Snapshot consumeSnapshot(std::size_t topK);

} // namespace mbgl::util::map_profiler

#if MLN_MAP_PROFILER_ENABLE
#define MLN_MAP_PROFILER_CONCAT_INNER(x, y) x##y
#define MLN_MAP_PROFILER_CONCAT(x, y) MLN_MAP_PROFILER_CONCAT_INNER(x, y)
#define MLN_MAP_PROFILE_SCOPE(stage, detail)                                                               \
    ::mbgl::util::map_profiler::ScopedEvent MLN_MAP_PROFILER_CONCAT(_mlnMapProfileScope_, __LINE__)((stage), (detail))
#define MLN_MAP_PROFILE_CONTEXT(layer, property)                                                               \
    ::mbgl::util::map_profiler::ScopedContext MLN_MAP_PROFILER_CONCAT(_mlnMapProfileContext_, __LINE__)((layer), (property))
#define MLN_MAP_PROFILE_CONTEXT_DETAIL(layer, property, detail)                                                 \
    ::mbgl::util::map_profiler::ScopedContext MLN_MAP_PROFILER_CONCAT(_mlnMapProfileContext_, __LINE__)(       \
        (layer),                                                                                                 \
        (property),                                                                                              \
        (detail))
#else
#define MLN_MAP_PROFILE_SCOPE(stage, detail) ((void)0)
#define MLN_MAP_PROFILE_CONTEXT(layer, property) ((void)0)
#define MLN_MAP_PROFILE_CONTEXT_DETAIL(layer, property, detail) ((void)0)
#endif
