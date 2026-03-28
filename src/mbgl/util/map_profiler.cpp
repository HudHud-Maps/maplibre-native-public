#include <mbgl/util/map_profiler.hpp>

#if MLN_MAP_PROFILER_ENABLE

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mbgl::util::map_profiler {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t shardCount = 64;
constexpr std::size_t maxEntriesPerShard = 512;
constexpr uint64_t fnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t fnvPrime = 1099511628211ull;

struct Context {
    const char* layer = nullptr;
    const char* property = nullptr;
    const char* detail = nullptr;
    uint64_t layerHash = 0;
    uint64_t propertyHash = 0;
    uint64_t detailHash = 0;
};

struct AggregationKey {
    Stage stage;
    uint64_t layerHash;
    uint64_t propertyHash;
    uint64_t detailHash;

    bool operator==(const AggregationKey& rhs) const noexcept {
        return stage == rhs.stage && layerHash == rhs.layerHash && propertyHash == rhs.propertyHash &&
               detailHash == rhs.detailHash;
    }
};

struct AggregatedEntry {
    std::string layer;
    std::string property;
    std::string detail;
    uint64_t count = 0;
    uint64_t totalDurationNs = 0;
    uint64_t maxDurationNs = 0;
};

struct AggregationKeyHash {
    std::size_t operator()(const AggregationKey& key) const noexcept {
        auto hash = static_cast<uint64_t>(key.stage);
        hash ^= key.layerHash + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        hash ^= key.propertyHash + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        hash ^= key.detailHash + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        return static_cast<std::size_t>(hash);
    }
};

struct AggregationShard {
    std::mutex mutex;
    std::unordered_map<AggregationKey, AggregatedEntry, AggregationKeyHash> entries;
};

struct State {
    std::atomic<uint32_t> sinkRefs{0};
    std::atomic<uint32_t> evalSampleRate{16};
    std::atomic<uint64_t> sampleCounter{0};
    std::atomic<uint64_t> signpostSampleCounter{0};
    std::atomic<uint64_t> droppedSamples{0};
    std::atomic<uint64_t> windowStartNs{0};
    std::array<AggregationShard, shardCount> shards;

#if MLN_MAP_PROFILER_SIGNPOST
    os_log_t signpostLog = os_log_create("org.maplibre.native", "map-profiler");
#endif

    State() {
        for (auto& shard : shards) {
            shard.entries.reserve(maxEntriesPerShard / 2);
        }
    }
};

thread_local Context currentContext;

State& state() {
    static State profilerState;
    return profilerState;
}

uint64_t nowNs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

uint64_t hashCString(const char* str) noexcept {
    if (!str || !*str) {
        return 0;
    }

    uint64_t hash = fnvOffsetBasis;
    const auto* data = reinterpret_cast<const unsigned char*>(str);
    while (*data != '\0') {
        hash ^= static_cast<uint64_t>(*data++);
        hash *= fnvPrime;
    }
    return hash;
}

const char* normalize(const char* value) noexcept {
    return (value && *value) ? value : nullptr;
}

bool isHighVolumeStage(const Stage stage) noexcept {
    switch (stage) {
        case Stage::ExpressionEvaluate:
        case Stage::RenderLayerEvaluate:
        case Stage::TileFilterEvaluate:
            return true;
        default:
            return false;
    }
}

bool shouldSample(const Stage stage) noexcept {
    if (!isHighVolumeStage(stage)) {
        return true;
    }

    const uint32_t sampleRate = state().evalSampleRate.load(std::memory_order_relaxed);
    if (sampleRate <= 1) {
        return true;
    }

    const uint64_t ticket = state().sampleCounter.fetch_add(1, std::memory_order_relaxed);
    return (ticket % sampleRate) == 0;
}

bool shouldSampleSignpost() noexcept {
    const uint32_t sampleRate = state().evalSampleRate.load(std::memory_order_relaxed);
    if (sampleRate <= 1) {
        return true;
    }

    const uint64_t ticket = state().signpostSampleCounter.fetch_add(1, std::memory_order_relaxed);
    return (ticket % sampleRate) == 0;
}

void record(const Stage stage,
            const char* layer,
            const char* property,
            const char* detail,
            const uint64_t layerHash,
            const uint64_t propertyHash,
            const uint64_t detailHash,
            const uint64_t durationNs) {
    auto& profilerState = state();
    const AggregationKey key{
        stage,
        layerHash,
        propertyHash,
        detailHash,
    };

    const auto hash = AggregationKeyHash{}(key);
    auto& shard = profilerState.shards[hash & (shardCount - 1)];

    std::scoped_lock lock(shard.mutex);

    auto entryIt = shard.entries.find(key);
    if (entryIt == shard.entries.end()) {
        if (shard.entries.size() >= maxEntriesPerShard) {
            profilerState.droppedSamples.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        AggregatedEntry newEntry;
        if (layer) {
            newEntry.layer = layer;
        }

        if (property) {
            newEntry.property = property;
        }

        if (detail) {
            newEntry.detail = detail;
        }

        entryIt = shard.entries.emplace(key, std::move(newEntry)).first;
    }

    auto& entry = entryIt->second;
    ++entry.count;
    entry.totalDurationNs += durationNs;
    entry.maxDurationNs = std::max(entry.maxDurationNs, durationNs);
}

} // namespace

const char* stageToString(const Stage stage) noexcept {
    switch (stage) {
        case Stage::StyleLayerParse:
            return "style_layer_parse";
        case Stage::StylePropertyParse:
            return "style_property_parse";
        case Stage::ExpressionParse:
            return "expression_parse";
        case Stage::ExpressionEvaluate:
            return "expression_evaluate";
        case Stage::RenderLayerEvaluate:
            return "render_layer_evaluate";
        case Stage::TileLayerParse:
            return "tile_layer_parse";
        case Stage::TileFilterEvaluate:
            return "tile_filter_evaluate";
    }

    return "unknown";
}

void retainSink() noexcept {
    auto& profilerState = state();
    const auto previous = profilerState.sinkRefs.fetch_add(1, std::memory_order_relaxed);
    if (previous != 0) {
        return;
    }

    profilerState.windowStartNs.store(nowNs(), std::memory_order_relaxed);
    for (auto& shard : profilerState.shards) {
        std::scoped_lock lock(shard.mutex);
        shard.entries.clear();
    }
    profilerState.droppedSamples.store(0, std::memory_order_relaxed);
}

void releaseSink() noexcept {
    auto& profilerState = state();
    auto refs = profilerState.sinkRefs.load(std::memory_order_relaxed);
    while (refs > 0 && !profilerState.sinkRefs.compare_exchange_weak(refs, refs - 1, std::memory_order_relaxed)) {
    }
}

bool isEnabled() noexcept {
    return state().sinkRefs.load(std::memory_order_relaxed) > 0;
}

void setEvalSampleRate(uint32_t sampleRate) noexcept {
    state().evalSampleRate.store(std::max<uint32_t>(sampleRate, 1u), std::memory_order_relaxed);
}

uint32_t getEvalSampleRate() noexcept {
    return state().evalSampleRate.load(std::memory_order_relaxed);
}

ScopedContext::ScopedContext(const char* layer, const char* property, const char* detail) noexcept
    : previousLayer(currentContext.layer),
      previousProperty(currentContext.property),
      previousDetail(currentContext.detail),
      previousLayerHash(currentContext.layerHash),
      previousPropertyHash(currentContext.propertyHash),
      previousDetailHash(currentContext.detailHash) {
    if (const auto* normalizedLayer = normalize(layer)) {
        currentContext.layer = normalizedLayer;
        currentContext.layerHash = hashCString(normalizedLayer);
    }

    if (const auto* normalizedProperty = normalize(property)) {
        currentContext.property = normalizedProperty;
        currentContext.propertyHash = hashCString(normalizedProperty);
    }

    if (const auto* normalizedDetail = normalize(detail)) {
        currentContext.detail = normalizedDetail;
        currentContext.detailHash = hashCString(normalizedDetail);
    }
}

ScopedContext::~ScopedContext() {
    currentContext.layer = previousLayer;
    currentContext.property = previousProperty;
    currentContext.detail = previousDetail;
    currentContext.layerHash = previousLayerHash;
    currentContext.propertyHash = previousPropertyHash;
    currentContext.detailHash = previousDetailHash;
}

ScopedEvent::ScopedEvent(const Stage stage_, const char* detail_) noexcept
    : stage(stage_)
#if MLN_MAP_PROFILER_TRACY
      ,
      tracyCtx({})
#endif
{
    const bool sinkEnabled = isEnabled();

#if MLN_MAP_PROFILER_SIGNPOST
    auto& profilerState = state();
    const bool signpostEnabled = os_signpost_enabled(profilerState.signpostLog);
#else
    const bool signpostEnabled = false;
#endif

    const bool sinkSampled = sinkEnabled ? shouldSample(stage_) : false;
    const bool signpostSampled = signpostEnabled ? shouldSampleSignpost() : false;

    if (!sinkSampled && !signpostSampled) {
        return;
    }

    layer = currentContext.layer;
    property = currentContext.property;
    layerHash = currentContext.layerHash;
    propertyHash = currentContext.propertyHash;
    if (const auto* normalizedDetail = normalize(detail_)) {
        detail = normalizedDetail;
        detailHash = hashCString(normalizedDetail);
    } else {
        detail = currentContext.detail;
        detailHash = currentContext.detailHash;
    }

#if MLN_MAP_PROFILER_SIGNPOST
    if (signpostSampled) {
        signpostID = os_signpost_id_generate(profilerState.signpostLog);
#define MLN_SIGNPOST_BEGIN(nameLiteral)                                                  \
    os_signpost_interval_begin(profilerState.signpostLog,                                \
                               signpostID,                                               \
                               nameLiteral,                                              \
                               "layer=%{public}s property=%{public}s detail=%{public}s", \
                               layer ? layer : "-",                                      \
                               property ? property : "-",                                \
                               detail ? detail : "-")
        switch (stage) {
            case Stage::StyleLayerParse:
                MLN_SIGNPOST_BEGIN("style_layer_parse");
                break;
            case Stage::StylePropertyParse:
                MLN_SIGNPOST_BEGIN("style_property_parse");
                break;
            case Stage::ExpressionParse:
                MLN_SIGNPOST_BEGIN("expression_parse");
                break;
            case Stage::ExpressionEvaluate:
                MLN_SIGNPOST_BEGIN("expression_evaluate");
                break;
            case Stage::RenderLayerEvaluate:
                MLN_SIGNPOST_BEGIN("render_layer_evaluate");
                break;
            case Stage::TileLayerParse:
                MLN_SIGNPOST_BEGIN("tile_layer_parse");
                break;
            case Stage::TileFilterEvaluate:
                MLN_SIGNPOST_BEGIN("tile_filter_evaluate");
                break;
        }
#undef MLN_SIGNPOST_BEGIN
        signpostActive = true;
    }
#endif

#if MLN_MAP_PROFILER_TRACY
    tracyCtx = TracyCZoneN(stageToString(stage), 1);
#endif

    if (sinkSampled) {
        active = true;
        startNs = nowNs();
    }
}

ScopedEvent::~ScopedEvent() {
    if (active) {
        record(stage, layer, property, detail, layerHash, propertyHash, detailHash, nowNs() - startNs);
    }

#if MLN_MAP_PROFILER_SIGNPOST
    if (signpostActive) {
        auto& profilerState = state();
#define MLN_SIGNPOST_END(nameLiteral)                                                  \
    os_signpost_interval_end(profilerState.signpostLog,                                \
                             signpostID,                                               \
                             nameLiteral,                                              \
                             "layer=%{public}s property=%{public}s detail=%{public}s", \
                             layer ? layer : "-",                                      \
                             property ? property : "-",                                \
                             detail ? detail : "-")
        switch (stage) {
            case Stage::StyleLayerParse:
                MLN_SIGNPOST_END("style_layer_parse");
                break;
            case Stage::StylePropertyParse:
                MLN_SIGNPOST_END("style_property_parse");
                break;
            case Stage::ExpressionParse:
                MLN_SIGNPOST_END("expression_parse");
                break;
            case Stage::ExpressionEvaluate:
                MLN_SIGNPOST_END("expression_evaluate");
                break;
            case Stage::RenderLayerEvaluate:
                MLN_SIGNPOST_END("render_layer_evaluate");
                break;
            case Stage::TileLayerParse:
                MLN_SIGNPOST_END("tile_layer_parse");
                break;
            case Stage::TileFilterEvaluate:
                MLN_SIGNPOST_END("tile_filter_evaluate");
                break;
        }
#undef MLN_SIGNPOST_END
    }
#endif

#if MLN_MAP_PROFILER_TRACY
    TracyCZoneEnd(tracyCtx);
#endif
}

Snapshot consumeSnapshot(const std::size_t topK) {
    Snapshot snapshot;
    snapshot.windowEndNs = nowNs();
    snapshot.windowStartNs = state().windowStartNs.exchange(snapshot.windowEndNs, std::memory_order_relaxed);
    snapshot.droppedSamples = state().droppedSamples.exchange(0, std::memory_order_relaxed);

    auto& profilerState = state();
    std::vector<SnapshotEntry> entries;
    entries.reserve(shardCount * (maxEntriesPerShard / 4));

    for (auto& shard : profilerState.shards) {
        std::scoped_lock lock(shard.mutex);
        entries.reserve(entries.size() + shard.entries.size());
        for (auto& [key, value] : shard.entries) {
            entries.emplace_back(SnapshotEntry{
                .stage = key.stage,
                .layer = std::move(value.layer),
                .property = std::move(value.property),
                .detail = std::move(value.detail),
                .count = value.count,
                .totalDurationNs = value.totalDurationNs,
                .maxDurationNs = value.maxDurationNs,
            });
        }
        shard.entries.clear();
    }

    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.totalDurationNs > rhs.totalDurationNs;
    });

    if (topK > 0 && entries.size() > topK) {
        entries.resize(topK);
    }

    snapshot.entries = std::move(entries);
    return snapshot;
}

} // namespace mbgl::util::map_profiler

#else

namespace mbgl::util::map_profiler {

void retainSink() noexcept {}
void releaseSink() noexcept {}
bool isEnabled() noexcept {
    return false;
}
void setEvalSampleRate(uint32_t) noexcept {}
uint32_t getEvalSampleRate() noexcept {
    return 1;
}
const char* stageToString(Stage) noexcept {
    return "disabled";
}

ScopedContext::ScopedContext(const char*, const char*, const char*) noexcept {}
ScopedContext::~ScopedContext() = default;

ScopedEvent::ScopedEvent(Stage, const char*) noexcept {}
ScopedEvent::~ScopedEvent() = default;

Snapshot consumeSnapshot(std::size_t) {
    return {};
}

} // namespace mbgl::util::map_profiler

#endif
