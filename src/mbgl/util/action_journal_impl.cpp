#include <mbgl/util/action_journal_impl.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/monotonic_timer.hpp>
#include <mbgl/util/map_profiler.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/map/map.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <regex>
#include <utility>

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR

#define USE_GHC_FILESYSTEM

#endif
#endif

#ifdef USE_GHC_FILESYSTEM

#include <ghc/filesystem.hpp>
namespace mbgl {
namespace filesystem = ghc::filesystem;
}

#else

#include <filesystem>
namespace mbgl {
namespace filesystem = std::filesystem;
}

#endif

namespace mbgl {
namespace util {

constexpr auto ACTION_JOURNAL_DIRECTORY_NAME = "action_journal";
constexpr auto ACTION_JOURNAL_FILE_NAME = "action_journal";
constexpr auto ACTION_JOURNAL_FILE_EXTENSION = "log";
constexpr std::size_t ACTION_JOURNAL_PROFILER_TOP_K = 200;

class MapEnvironmentSnapshot {
public:
    MapEnvironmentSnapshot(const ActionJournal::Impl& journal)
        : time(std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now())),
          clientName(journal.getMap().getClientOptions().name()),
          clientVersion(journal.getMap().getClientOptions().version()),
          styleName(journal.getMap().getStyle().getName()),
          styleURL(journal.getMap().getStyle().getURL()) {}

    const auto& getTime() const { return time; }
    const auto& getClientName() const { return clientName; }
    const auto& getClientVersion() const { return clientVersion; }
    const auto& getStyleName() const { return styleName; }
    const auto& getStyleURL() const { return styleURL; }

protected:
    const std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds> time;
    const std::string clientName;
    const std::string clientVersion;
    const std::string styleName;
    const std::string styleURL;
};

class ActionJournalEvent {
public:
    ActionJournalEvent(const rapidjson::GenericStringRef<char>& name, const MapEnvironmentSnapshot& env)
        : json(rapidjson::kObjectType),
          eventJson(rapidjson::kObjectType) {
        json.AddMember("name", name, json.GetAllocator());
        json.AddMember("time", iso8601(env.getTime()), json.GetAllocator());

        if (!env.getClientName().empty()) {
            json.AddMember("clientName", env.getClientName(), json.GetAllocator());
        }

        if (!env.getClientVersion().empty()) {
            json.AddMember("clientVersion", env.getClientVersion(), json.GetAllocator());
        }

        if (!env.getStyleName().empty()) {
            json.AddMember("styleName", env.getStyleName(), json.GetAllocator());
        }

        if (!env.getStyleURL().empty()) {
            json.AddMember("styleURL", env.getStyleURL(), json.GetAllocator());
        }
    }

    ~ActionJournalEvent() = default;

    template <typename T>
    ActionJournalEvent& addEvent(const rapidjson::GenericStringRef<char>& key, const T& value) {
        eventJson.AddMember(key, value, json.GetAllocator());
        return *this;
    }

    ActionJournalEvent& addEventStringArray(const rapidjson::GenericStringRef<char>& key,
                                            const std::vector<std::string>& value) {
        rapidjson::Value arrayJson(rapidjson::kArrayType);

        for (const auto& elem : value) {
            rapidjson::Value elemJson(rapidjson::kStringType);
            elemJson.SetString(elem.c_str(), static_cast<rapidjson::SizeType>(elem.size()));
            arrayJson.PushBack(elemJson, json.GetAllocator());
        }

        eventJson.AddMember(key, arrayJson, json.GetAllocator());
        return *this;
    }

    template <typename T>
    ActionJournalEvent& addEventEnum(const rapidjson::GenericStringRef<char>& key, const T& value) {
        eventJson.AddMember(key, rapidjson::StringRef(Enum<T>::toString(value)), json.GetAllocator());
        return *this;
    }

    std::string toString() {
        if (!eventJson.ObjectEmpty()) {
            json.AddMember("event", eventJson, json.GetAllocator());
        }

        rapidjson::StringBuffer jsonBuffer;
        rapidjson::Writer<rapidjson::StringBuffer> jsonWriter(jsonBuffer);

        json.Accept(jsonWriter);

        return jsonBuffer.GetString();
    }

    rapidjson::Document json;
    rapidjson::Value eventJson;
};

ActionJournal::Impl::Impl(const Map& map_, const ActionJournalOptions& options_)
    : map(map_),
      options(options_),
      scheduler(Scheduler::GetSequenced()),
      renderingInfoReportTime(options.renderingStatsReportInterval()) {
    assert(!options.path().empty());
    assert(options.logFileSize() > 0);
    assert(options.logFileCount() > 1);

    previousFrameTime = util::MonotonicTimer::now().count();

    options.withPath((mbgl::filesystem::canonical(options.path()) / ACTION_JOURNAL_DIRECTORY_NAME).generic_string());

    if (!openFile(detectFiles(), false)) {
        Log::Error(Event::General, "Failed to open Action Journal file");
    }

    map_profiler::retainSink();
}

ActionJournal::Impl::~Impl() {
    flush();
    map_profiler::releaseSink();
}

std::string ActionJournal::Impl::getLogDirectory() const {
    return options.path();
}

std::vector<std::string> ActionJournal::Impl::getLogFiles() const {
    std::set<std::string> files;
    for (const auto& entry : mbgl::filesystem::directory_iterator(options.path())) {
        if (!entry.is_regular_file()) {
            continue;
        }

        files.emplace(mbgl::filesystem::canonical(entry.path()).generic_string());
    }

    return std::vector<std::string>(files.begin(), files.end());
}

std::vector<std::string> ActionJournal::Impl::getLog() {
    std::scoped_lock lock(fileMutex);
    std::vector<std::string> logEvents;

    const auto readFile = [&](std::fstream& file) {
        if (!file) {
            return;
        }

        for (std::string line; std::getline(file, line);) {
            logEvents.emplace_back(line);
        }

        file.clear();
    };

    // read current file
    if (currentFile && currentFileIndex == 0) {
        currentFile.seekg(std::ios::beg);
        readFile(currentFile);
        return logEvents;
    }

    const uint32_t maxFileIndex = currentFile ? currentFileIndex - 1 : detectFiles();

    // read previous files
    for (uint32_t fileIndex = 0; fileIndex <= maxFileIndex; ++fileIndex) {
        const auto& filepath = getFilepath(fileIndex);
        std::fstream file(filepath, std::fstream::in);
        readFile(file);
    }

    if (currentFile) {
        currentFile.seekg(std::ios::beg);
        readFile(currentFile);
    }

    return logEvents;
}

void ActionJournal::Impl::clearLog() {
    std::scoped_lock lock(fileMutex);

    // close current file
    currentFile.close();

    currentFileIndex = 0;
    currentFileSize = 0;

    if (!mbgl::filesystem::remove_all(options.path())) {
        Log::Error(Event::General, "Failed to clear ActionJournal");
    }

    if (!openFile(0, false)) {
        Log::Error(Event::General, "Failed to open Action Journal file");
    }
}

void ActionJournal::Impl::flush() {
    scheduler->waitForEmpty();
}

std::string ActionJournal::Impl::getDirectoryName() {
    return ACTION_JOURNAL_DIRECTORY_NAME;
}

void ActionJournal::Impl::onCameraWillChange(CameraChangeMode mode) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        log(ActionJournalEvent("onCameraWillChange", env).addEvent("cameraMode", static_cast<int>(mode)));
    });
}

void ActionJournal::Impl::onCameraDidChange(CameraChangeMode mode) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        log(ActionJournalEvent("onCameraDidChange", env).addEvent("cameraMode", static_cast<int>(mode)));
    });
}

void ActionJournal::Impl::onWillStartLoadingMap() {
    scheduler->schedule(
        [=, this, env = MapEnvironmentSnapshot(*this)]() { log(ActionJournalEvent("onWillStartLoadingMap", env)); });
}

void ActionJournal::Impl::onDidFinishLoadingMap() {
    scheduler->schedule(
        [=, this, env = MapEnvironmentSnapshot(*this)]() { log(ActionJournalEvent("onDidFinishLoadingMap", env)); });
}

void ActionJournal::Impl::onDidFailLoadingMap(MapLoadError error, const std::string& errorStr) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        log(ActionJournalEvent("onDidFailLoadingMap", env)
                .addEvent("error", errorStr)
                .addEvent("code", static_cast<int>(error)));
    });
}

void ActionJournal::Impl::onDidFinishRenderingFrame(const RenderFrameStatus& frame) {
    // update report time
    double currentFrameTime = util::MonotonicTimer::now().count();
    double elapsedTime = currentFrameTime - previousFrameTime;
    previousFrameTime = currentFrameTime;

    // update rendering stats
    renderingStats.encodingMin = std::min(renderingStats.encodingMin, frame.renderingStats.encodingTime);

    renderingStats.encodingMax = std::max(renderingStats.encodingMax, frame.renderingStats.encodingTime);

    renderingStats.renderingMin = std::min(renderingStats.renderingMin, frame.renderingStats.renderingTime);

    renderingStats.renderingMax = std::max(renderingStats.renderingMax, frame.renderingStats.renderingTime);

    renderingStats.encodingTotal += frame.renderingStats.encodingTime;
    renderingStats.renderingTotal += frame.renderingStats.renderingTime;
    ++renderingStats.frameCount;

    renderingInfoReportTime -= elapsedTime;
    if (renderingInfoReportTime > 1e-9) {
        return;
    }

    renderingInfoReportTime = options.renderingStatsReportInterval();

    // skip if the full interval had an idle map
    if (renderingStats.frameCount == 0) {
        return;
    }

    auto profilerSnapshot = map_profiler::consumeSnapshot(0);
    const auto profilerSampleRate = map_profiler::getEvalSampleRate();

    scheduler->schedule([=,
                         this,
                         env = MapEnvironmentSnapshot(*this),
                         stats = std::move(renderingStats),
                         profilerSnapshot = std::move(profilerSnapshot)]() mutable {
        log(ActionJournalEvent("renderingStats", env)
                .addEvent("encodingMin", stats.encodingMin)
                .addEvent("encodingMax", stats.encodingMax)
                .addEvent("encodingAvg", stats.encodingTotal / stats.frameCount)
                .addEvent("renderingMin", stats.renderingMin)
                .addEvent("renderingMax", stats.renderingMax)
                .addEvent("renderingAvg", stats.renderingTotal / stats.frameCount));

        if (profilerSnapshot.entries.empty() && profilerSnapshot.droppedSamples == 0) {
            return;
        }

        ActionJournalEvent profilerEvent("profilingStats", env);
        profilerEvent.addEvent("windowStartNs", profilerSnapshot.windowStartNs)
            .addEvent("windowEndNs", profilerSnapshot.windowEndNs)
            .addEvent("sampleRate", profilerSampleRate)
            .addEvent("droppedSamples", profilerSnapshot.droppedSamples)
            .addEvent("entryCount", static_cast<uint64_t>(profilerSnapshot.entries.size()));

        auto& allocator = profilerEvent.json.GetAllocator();
        rapidjson::Value entriesJson(rapidjson::kArrayType);
        entriesJson.Reserve(
            static_cast<rapidjson::SizeType>(std::min<std::size_t>(ACTION_JOURNAL_PROFILER_TOP_K,
                                                                    profilerSnapshot.entries.size())),
            allocator);

        struct AggregateStats {
            uint64_t count = 0;
            uint64_t totalDurationNs = 0;
            uint64_t maxDurationNs = 0;
        };

        std::map<std::string, AggregateStats> byLayerMap;
        std::map<std::string, AggregateStats> byExpressionMap;
        std::map<std::string, std::array<std::string, 4>> byExpressionLabels;

        const auto merge = [](AggregateStats& dst, const map_profiler::SnapshotEntry& src) {
            dst.count += src.count;
            dst.totalDurationNs += src.totalDurationNs;
            dst.maxDurationNs = std::max(dst.maxDurationNs, src.maxDurationNs);
        };

        for (const auto& entry : profilerSnapshot.entries) {
            if (!entry.layer.empty()) {
                merge(byLayerMap[entry.layer], entry);
            }

            if ((entry.stage == map_profiler::Stage::ExpressionEvaluate ||
                 entry.stage == map_profiler::Stage::ExpressionParse) &&
                !entry.detail.empty()) {
                const std::string stage = map_profiler::stageToString(entry.stage);
                std::string key;
                key.reserve(stage.size() + entry.layer.size() + entry.property.size() + entry.detail.size() + 4);
                key.append(stage);
                key.push_back('\x1f');
                key.append(entry.layer);
                key.push_back('\x1f');
                key.append(entry.property);
                key.push_back('\x1f');
                key.append(entry.detail);
                merge(byExpressionMap[key], entry);
                byExpressionLabels.try_emplace(
                    key, std::array<std::string, 4>{stage, entry.layer, entry.property, entry.detail});
            }
        }

        for (std::size_t i = 0;
             i < profilerSnapshot.entries.size() && i < static_cast<std::size_t>(ACTION_JOURNAL_PROFILER_TOP_K);
             ++i) {
            const auto& entry = profilerSnapshot.entries[i];
            rapidjson::Value entryJson(rapidjson::kObjectType);
            entryJson.AddMember("stage", rapidjson::StringRef(map_profiler::stageToString(entry.stage)), allocator);

            if (!entry.layer.empty()) {
                rapidjson::Value value(rapidjson::kStringType);
                value.SetString(entry.layer.c_str(), static_cast<rapidjson::SizeType>(entry.layer.size()), allocator);
                entryJson.AddMember("layer", value, allocator);
            }

            if (!entry.property.empty()) {
                rapidjson::Value value(rapidjson::kStringType);
                value.SetString(
                    entry.property.c_str(), static_cast<rapidjson::SizeType>(entry.property.size()), allocator);
                entryJson.AddMember("property", value, allocator);
            }

            if (!entry.detail.empty()) {
                rapidjson::Value value(rapidjson::kStringType);
                value.SetString(entry.detail.c_str(), static_cast<rapidjson::SizeType>(entry.detail.size()), allocator);
                entryJson.AddMember("detail", value, allocator);
            }

            entryJson.AddMember("count", entry.count, allocator);
            entryJson.AddMember("totalDurationNs", entry.totalDurationNs, allocator);
            entryJson.AddMember("maxDurationNs", entry.maxDurationNs, allocator);

            entriesJson.PushBack(entryJson, allocator);
        }

        profilerEvent.addEvent("truncatedEntries", profilerSnapshot.entries.size() > ACTION_JOURNAL_PROFILER_TOP_K);
        profilerEvent.eventJson.AddMember("entries", entriesJson, allocator);

        rapidjson::Value byLayerJson(rapidjson::kArrayType);
        std::vector<std::pair<std::string, AggregateStats>> byLayer(byLayerMap.begin(), byLayerMap.end());
        std::sort(byLayer.begin(), byLayer.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.second.totalDurationNs > rhs.second.totalDurationNs;
        });
        byLayerJson.Reserve(
            static_cast<rapidjson::SizeType>(std::min<std::size_t>(ACTION_JOURNAL_PROFILER_TOP_K, byLayer.size())),
            allocator);
        for (std::size_t i = 0; i < byLayer.size() && i < ACTION_JOURNAL_PROFILER_TOP_K; ++i) {
            const auto& [layer, layerStats] = byLayer[i];
            rapidjson::Value layerJson(rapidjson::kObjectType);
            rapidjson::Value layerName(rapidjson::kStringType);
            layerName.SetString(layer.c_str(), static_cast<rapidjson::SizeType>(layer.size()), allocator);
            layerJson.AddMember("layer", layerName, allocator);
            layerJson.AddMember("count", layerStats.count, allocator);
            layerJson.AddMember("totalDurationNs", layerStats.totalDurationNs, allocator);
            layerJson.AddMember("maxDurationNs", layerStats.maxDurationNs, allocator);
            byLayerJson.PushBack(layerJson, allocator);
        }
        profilerEvent.eventJson.AddMember("byLayer", byLayerJson, allocator);

        rapidjson::Value byExpressionJson(rapidjson::kArrayType);
        std::vector<std::pair<std::string, AggregateStats>> byExpression(byExpressionMap.begin(), byExpressionMap.end());
        std::sort(byExpression.begin(), byExpression.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.second.totalDurationNs > rhs.second.totalDurationNs;
        });
        byExpressionJson.Reserve(static_cast<rapidjson::SizeType>(
            std::min<std::size_t>(ACTION_JOURNAL_PROFILER_TOP_K, byExpression.size())), allocator);
        for (std::size_t i = 0; i < byExpression.size() && i < ACTION_JOURNAL_PROFILER_TOP_K; ++i) {
            const auto& [key, exprStats] = byExpression[i];
            const auto labelsIt = byExpressionLabels.find(key);
            if (labelsIt == byExpressionLabels.end()) {
                continue;
            }

            const auto& labels = labelsIt->second;
            rapidjson::Value expressionJson(rapidjson::kObjectType);

            rapidjson::Value stageName(rapidjson::kStringType);
            stageName.SetString(labels[0].c_str(), static_cast<rapidjson::SizeType>(labels[0].size()), allocator);
            expressionJson.AddMember("stage", stageName, allocator);

            if (!labels[1].empty()) {
                rapidjson::Value layerName(rapidjson::kStringType);
                layerName.SetString(labels[1].c_str(), static_cast<rapidjson::SizeType>(labels[1].size()), allocator);
                expressionJson.AddMember("layer", layerName, allocator);
            }

            if (!labels[2].empty()) {
                rapidjson::Value propertyName(rapidjson::kStringType);
                propertyName.SetString(
                    labels[2].c_str(), static_cast<rapidjson::SizeType>(labels[2].size()), allocator);
                expressionJson.AddMember("property", propertyName, allocator);
            }

            rapidjson::Value expressionName(rapidjson::kStringType);
            expressionName.SetString(labels[3].c_str(), static_cast<rapidjson::SizeType>(labels[3].size()), allocator);
            expressionJson.AddMember("expression", expressionName, allocator);

            expressionJson.AddMember("count", exprStats.count, allocator);
            expressionJson.AddMember("totalDurationNs", exprStats.totalDurationNs, allocator);
            expressionJson.AddMember("maxDurationNs", exprStats.maxDurationNs, allocator);

            byExpressionJson.PushBack(expressionJson, allocator);
        }
        profilerEvent.eventJson.AddMember("byExpression", byExpressionJson, allocator);

        log(std::move(profilerEvent));
    });

    renderingStats = {};
}

void ActionJournal::Impl::onWillStartRenderingMap() {
    scheduler->schedule(
        [=, this, env = MapEnvironmentSnapshot(*this)]() { log(ActionJournalEvent("onWillStartRenderingMap", env)); });
}

void ActionJournal::Impl::onDidFinishRenderingMap(RenderMode) {
    scheduler->schedule(
        [=, this, env = MapEnvironmentSnapshot(*this)]() { log(ActionJournalEvent("onDidFinishRenderingMap", env)); });
}

void ActionJournal::Impl::onDidFinishLoadingStyle() {
    scheduler->schedule(
        [=, this, env = MapEnvironmentSnapshot(*this)]() { log(ActionJournalEvent("onDidFinishLoadingStyle", env)); });
}

void ActionJournal::Impl::onSourceChanged(style::Source& source) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this), type = source.getType(), id = source.getID()]() {
        log(ActionJournalEvent("onSourceChanged", env).addEventEnum("type", type).addEvent("id", id));
    });
}

void ActionJournal::Impl::onDidBecomeIdle() {
    scheduler->schedule(
        [=, this, env = MapEnvironmentSnapshot(*this)]() { log(ActionJournalEvent("onDidBecomeIdle", env)); });
}

void ActionJournal::Impl::onStyleImageMissing(const std::string& id) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        log(ActionJournalEvent("onStyleImageMissing", env).addEvent("id", id));
    });
}

void ActionJournal::Impl::onRegisterShaders(gfx::ShaderRegistry&) {
    scheduler->schedule(
        [=, this, env = MapEnvironmentSnapshot(*this)]() { log(ActionJournalEvent("onRegisterShaders", env)); });
}

void ActionJournal::Impl::onPreCompileShader(shaders::BuiltIn id, gfx::Backend::Type backend, const std::string&) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        log(ActionJournalEvent("onPreCompileShader", env)
                .addEventEnum("shader", id)
                .addEvent("backend", static_cast<int>(backend)));
    });
}

void ActionJournal::Impl::onPostCompileShader(shaders::BuiltIn id, gfx::Backend::Type backend, const std::string&) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        log(ActionJournalEvent("onPostCompileShader", env)
                .addEventEnum("shader", id)
                .addEvent("backend", static_cast<int>(backend)));
    });
}

void ActionJournal::Impl::onShaderCompileFailed(shaders::BuiltIn id,
                                                gfx::Backend::Type backend,
                                                const std::string& defines) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        log(ActionJournalEvent("onShaderCompileFailed", env)
                .addEventEnum("shader", id)
                .addEvent("backend", static_cast<int>(backend))
                .addEvent("defines", defines));
    });
}

void ActionJournal::Impl::onGlyphsLoaded(const FontStack& fonts, const GlyphRange& range) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        log(ActionJournalEvent("onGlyphsLoaded", env)
                .addEventStringArray("fonts", fonts)
                .addEvent("rangeStart", range.first)
                .addEvent("rangeEnd", range.second));
    });
}

void ActionJournal::Impl::onGlyphsError(const FontStack& fonts, const GlyphRange& range, std::exception_ptr error) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        ActionJournalEvent event("onGlyphsError", env);

        event.addEventStringArray("fonts", fonts);
        event.addEvent("rangeStart", range.first);
        event.addEvent("rangeEnd", range.second);

        if (error) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                event.addEvent("error", std::string(e.what()));
            }
        }

        log(event);
    });
}

void ActionJournal::Impl::onGlyphsRequested(const FontStack& fonts, const GlyphRange& range) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        log(ActionJournalEvent("onGlyphsRequested", env)
                .addEventStringArray("fonts", fonts)
                .addEvent("rangeStart", range.first)
                .addEvent("rangeEnd", range.second));
    });
}

void ActionJournal::Impl::onTileAction(TileOperation op, const OverscaledTileID& id, const std::string& sourceID) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        log(ActionJournalEvent("onTileAction", env)
                .addEventEnum("action", op)
                .addEvent("tileX", id.canonical.x)
                .addEvent("tileY", id.canonical.y)
                .addEvent("tileZ", id.canonical.z)
                .addEvent("overscaledZ", id.overscaledZ)
                .addEvent("sourceID", sourceID));
    });
}

void ActionJournal::Impl::onSpriteLoaded(const std::optional<style::Sprite>& sprite) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        ActionJournalEvent event("onSpriteLoaded", *this);

        if (sprite) {
            event.addEvent("id", sprite.value().id);
            event.addEvent("url", sprite.value().spriteURL);
        }

        log(event);
    });
}

void ActionJournal::Impl::onSpriteError(const std::optional<style::Sprite>& sprite, std::exception_ptr error) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        ActionJournalEvent event("onSpriteError", env);

        if (sprite) {
            event.addEvent("id", sprite.value().id);
            event.addEvent("url", sprite.value().spriteURL);
        }

        if (error) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                event.addEvent("error", std::string(e.what()));
            }
        }

        log(event);
    });
}

void ActionJournal::Impl::onSpriteRequested(const std::optional<style::Sprite>& sprite) {
    scheduler->schedule([=, this, env = MapEnvironmentSnapshot(*this)]() {
        ActionJournalEvent event("onSpriteRequested", env);

        if (sprite) {
            event.addEvent("id", sprite.value().id);
            event.addEvent("url", sprite.value().spriteURL);
        }

        log(event);
    });
}

void ActionJournal::Impl::onMapCreate() {
    scheduler->schedule(
        [=, this, env = MapEnvironmentSnapshot(*this)]() { log(ActionJournalEvent("onMapCreate", env)); });
}

void ActionJournal::Impl::onMapDestroy() {
    scheduler->schedule(
        [=, this, env = MapEnvironmentSnapshot(*this)]() { log(ActionJournalEvent("onMapDestroy", env)); });
}

void ActionJournal::Impl::log(ActionJournalEvent& value) {
    logToFile(value.toString());
}

void ActionJournal::Impl::log(ActionJournalEvent&& value) {
    logToFile(value.toString());
}

std::string ActionJournal::Impl::getFilepath(uint32_t fileIndex) const {
    return options.path() + "/" + ACTION_JOURNAL_FILE_NAME + "." + std::to_string(fileIndex) + "." +
           ACTION_JOURNAL_FILE_EXTENSION;
}

uint32_t ActionJournal::Impl::detectFiles() const {
    if (!mbgl::filesystem::exists(options.path())) {
        return 0;
    }

    std::map<uint32_t, mbgl::filesystem::path> existingFiles;

    const std::regex fileRegex(std::string(R"(.*\.([0-9]+)\.)") + ACTION_JOURNAL_FILE_EXTENSION);
    std::smatch fileMatch;

    for (const auto& entry : mbgl::filesystem::directory_iterator(options.path())) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const std::string& path = entry.path().string();
        if (std::regex_match(path, fileMatch, fileRegex)) {
            if (fileMatch.size() == 2) {
                existingFiles.emplace(std::stoi(fileMatch[1].str()), path);
            }
        }
    }

    if (existingFiles.empty()) {
        return 0;
    }

    // removing extra files (old files or due to the file count changing)
    for (auto it = existingFiles.begin(); existingFiles.size() > options.logFileCount();) {
        mbgl::filesystem::remove(it->second);
        it = existingFiles.erase(it);
    }

    uint32_t expectedIndex = 0;

    // validate file index
    for (const auto& file : existingFiles) {
        if (file.first != expectedIndex) {
            mbgl::filesystem::rename(file.second, getFilepath(expectedIndex));
        }

        ++expectedIndex;
    }

    return expectedIndex - 1;
}

uint32_t ActionJournal::Impl::rollFiles() {
    // delete the oldest file
    const auto& oldestFilepath = getFilepath(0);
    if (mbgl::filesystem::exists(oldestFilepath)) {
        mbgl::filesystem::remove(oldestFilepath);
    }

    // rename the rest
    uint32_t expectedIndex = 0;
    for (uint32_t index = 1; index < options.logFileCount(); ++index) {
        const auto& filepath = getFilepath(index);
        if (mbgl::filesystem::exists(filepath)) {
            mbgl::filesystem::rename(filepath, getFilepath(expectedIndex++));
        }
    }

    return expectedIndex;
}

bool ActionJournal::Impl::openFile(uint32_t fileIndex, bool truncate) {
    assert(fileIndex < options.logFileCount());

    if (!mbgl::filesystem::exists(options.path())) {
        mbgl::filesystem::create_directories(options.path());
    }

    const auto& filepath = getFilepath(fileIndex);
    const std::ios::openmode openMode = (truncate ? std::ios::trunc : std::ios::ate | std::ios::app) |
                                        std::fstream::out | std::fstream::in;

    currentFile.open(filepath, openMode);

    if (currentFile.is_open()) {
        currentFileIndex = fileIndex;
        currentFileSize = mbgl::filesystem::file_size(filepath);
        return true;
    }

    return false;
}

bool ActionJournal::Impl::prepareFile(size_t size) {
    if (currentFileSize + size <= options.logFileSize() && currentFile) {
        currentFileSize += size;
        return true;
    }

    // else roll file
    currentFile.close();

    if (currentFileIndex + 1 >= options.logFileCount()) {
        currentFileIndex = rollFiles();
    } else {
        ++currentFileIndex;
    }

    return openFile(currentFileIndex, true);
}

void ActionJournal::Impl::logToFile(const std::string& value) {
    if (value.empty()) {
        return;
    }

    std::scoped_lock lock(fileMutex);

    if (!prepareFile(value.size() + 1)) {
        return;
    }

    currentFile << value << "\n";
    currentFile.flush();
}

} // namespace util
} // namespace mbgl
