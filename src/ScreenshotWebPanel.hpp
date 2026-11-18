// SPDX-License-Identifier: MIT
#pragma once

#include "../include/GakuWebPanel.h"
#include "GkJsonLite.hpp"
#include "ScreenshotConfig.hpp"
#include "ScreenshotEngine.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ScreenshotManager::WebPanel
{
    struct AppearanceInfo
    {
        int theme{0};
        float uiScale{1.0f};
    };

    struct BrowseFolderResult
    {
        bool started{false};
        bool pending{false};
        bool cancelled{false};
        std::string pathUtf8;
        std::string error;
    };

    struct HostHooks
    {
        std::function<std::wstring()> assetsDir;
        std::function<bool(const std::string& dirUtf8, std::string& error)> openFolder;
        std::function<BrowseFolderResult(const std::string& initialDirUtf8)> browseFolder;
        std::function<bool(BrowseFolderResult& outResult)> pollBrowseFolder;
        std::function<void()> cancelBrowseFolder;
        std::function<AppearanceInfo()> appearance;
        std::string pluginVersion{"0.1.0"};
    };

    struct CmdResult
    {
        bool ok{true};
        std::string body{"{}"};
        std::string code;
        std::string message;
        std::string details;

        static CmdResult Ok(std::string result = "{}")
        {
            CmdResult r;
            r.body = std::move(result);
            return r;
        }
        static CmdResult Err(std::string code_, std::string message_, std::string details_ = "")
        {
            CmdResult r;
            r.ok = false;
            r.code = std::move(code_);
            r.message = std::move(message_);
            r.details = std::move(details_);
            return r;
        }
    };

    inline constexpr const char* kSupportedMethods[] = {
        "bridge.hello",
        "bridge.ping",
        "state.get",
        "capture.take",
        "config.set",
        "folder.open",
        "folder.browse",
        "appearance.get",
        "shell.setTextInputActive",
    };

    class Bridge
    {
    public:
        Bridge(ScreenshotEngine& engine, ConfigStore& configStore, HostHooks hooks)
            : engine_(engine), configStore_(configStore), hooks_(std::move(hooks))
        {
            sessionId_ = NextSessionId();
        }

        uint64_t SessionId() const noexcept { return sessionId_; }
        bool Closed() const noexcept { return closed_.load(std::memory_order_acquire); }
        bool TextInputActive() const noexcept { return textInputActive_.load(std::memory_order_acquire); }

        GakuStatus GetAssets(GakuWebPanelAssetsV1* output)
        {
            if (!output) return GAKU_E_INVALID_ARGUMENT;
            if (closed_.load(std::memory_order_acquire)) return GAKU_E_SHUTTING_DOWN;
            if (!hooks_.assetsDir) return GAKU_E_UNAVAILABLE;
            const std::wstring dir = hooks_.assetsDir();
            if (dir.empty()) return GAKU_E_UNAVAILABLE;
            assetsDirCache_ = dir;
            output->abiVersion = GAKU_WEB_PANEL_VERSION;
            output->structSize = sizeof(GakuWebPanelAssetsV1);
            output->assetsDir = assetsDirCache_.c_str();
            output->entryPath = L"index.html";
            output->virtualHost = L"screenshotmanager.invalid";
            return GAKU_OK;
        }

        void OnShow()
        {
            visible_ = true;
            baselinePending_ = true;
        }

        void OnHide()
        {
            visible_ = false;
        }

        void SetEventSink(GakuWebPanelEventSink sink, void* userData)
        {
            std::lock_guard<std::mutex> lock(sinkMutex_);
            sink_ = sink;
            sinkUserData_ = userData;
            if (sink) baselinePending_ = true;
        }

        GakuStatus HandleMessage(const char* requestJson, uint32_t responseCapacity,
            char* responseJson, uint32_t* requiredBytes)
        {
            if (!requestJson) return GAKU_E_INVALID_ARGUMENT;
            std::string response;
            try { response = DispatchRequest(requestJson); }
            catch (...) { response = MakeError(0, "internal", "Unhandled bridge exception"); }
            const uint64_t needed = static_cast<uint64_t>(response.size()) + 1ull;
            if (needed > 0xFFFFFFFFull) return GAKU_E_LIMIT;
            if (requiredBytes) *requiredBytes = static_cast<uint32_t>(needed);
            if (!responseJson || responseCapacity < needed) return GAKU_E_SIZE;
            std::memcpy(responseJson, response.c_str(), static_cast<size_t>(needed));
            return GAKU_OK;
        }

        void OnUiTick()
        {
            if (closed_.load(std::memory_order_acquire)) return;
            try { Tick(); }
            catch (...) {}
        }

        void OnShutdown()
        {
            if (closed_.load(std::memory_order_acquire)) return;
            Emit("bridge.closed", "{\"reason\":\"service-shutdown\"}");
            if (hooks_.cancelBrowseFolder) hooks_.cancelBrowseFolder();
            visible_ = false;
            textInputActive_.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(sinkMutex_);
                sink_ = nullptr;
                sinkUserData_ = nullptr;
                sessionId_ = NextSessionId();
                seq_ = 0;
            }
            baselinePending_ = true;
            lastStateJson_.clear();
            lastAppearanceJson_.clear();
        }

        void Invalidate()
        {
            closed_.store(true, std::memory_order_release);
            textInputActive_.store(false, std::memory_order_release);
            if (hooks_.cancelBrowseFolder) hooks_.cancelBrowseFolder();
            std::lock_guard<std::mutex> lock(sinkMutex_);
            sink_ = nullptr;
            sinkUserData_ = nullptr;
        }

        std::string HandleMessageForTest(const std::string& request)
        {
            return DispatchRequest(request.c_str());
        }

    private:
        static uint64_t NextSessionId()
        {
            static std::atomic<uint64_t> counter{0};
            return ++counter;
        }

        static std::string MakeOk(uint64_t id, const std::string& resultJson)
        {
            return "{\"v\":1,\"type\":\"response\",\"id\":" + JsonNumber(static_cast<double>(id)) +
                ",\"ok\":true,\"result\":" + resultJson + "}";
        }

        static std::string MakeError(uint64_t id, const std::string& code, const std::string& message,
            const std::string& details = "")
        {
            std::string err = "{\"code\":" + JsonString(code) + ",\"message\":" + JsonString(message);
            if (!details.empty()) err += ",\"details\":" + details;
            err += "}";
            return "{\"v\":1,\"type\":\"response\",\"id\":" + JsonNumber(static_cast<double>(id)) +
                ",\"ok\":false,\"error\":" + err + "}";
        }

        void Emit(const std::string& name, const std::string& payloadJson)
        {
            std::lock_guard<std::mutex> lock(sinkMutex_);
            if (!sink_ || closed_.load(std::memory_order_acquire)) return;
            const std::string json = "{\"v\":1,\"type\":\"event\",\"name\":" + JsonString(name) +
                ",\"payload\":" + payloadJson +
                ",\"seq\":" + JsonNumber(static_cast<double>(++seq_)) +
                ",\"sid\":" + JsonNumber(static_cast<double>(sessionId_)) + "}";
            sink_(sinkUserData_, json.c_str());
        }

        std::string DispatchRequest(const char* requestJson)
        {
            GakuMotion::jsonlite::Value root;
            std::string parseError;
            if (!GakuMotion::jsonlite::ParseText(requestJson, root, parseError) || !root.IsObject())
                return MakeError(0, "invalid_params", "Malformed request JSON");

            const auto* version = root.Find("v");
            const auto* type = root.Find("type");
            const auto* idVal = root.Find("id");
            const auto* methodVal = root.Find("method");

            uint64_t id = 0;
            if (idVal && idVal->IsNumber() && idVal->numberValue >= 0)
                id = static_cast<uint64_t>(idVal->numberValue);

            if (!version || !version->IsNumber() || version->numberValue != 1.0 ||
                !type || !type->IsString() || type->stringValue != "request" ||
                !idVal || !idVal->IsNumber() ||
                !methodVal || !methodVal->IsString() || methodVal->stringValue.empty())
            {
                return MakeError(id, "invalid_params", "Request must carry v=1, type=request, numeric id, and method");
            }

            if (closed_.load(std::memory_order_acquire))
                return MakeError(id, "not_allowed", "Bridge is closed");

            const auto* params = root.Find("params");
            if (params && !params->IsObject() && !params->IsNull())
                return MakeError(id, "invalid_params", "params must be an object");

            static const GakuMotion::jsonlite::Value emptyObj = [] {
                GakuMotion::jsonlite::Value v;
                v.type = GakuMotion::jsonlite::Type::Object;
                return v;
            }();

            const GakuMotion::jsonlite::Value& paramObj = (params && params->IsObject()) ? *params : emptyObj;
            const std::string& method = methodVal->stringValue;

            CmdResult res;
            try { res = Dispatch(method, paramObj); }
            catch (...) { res = CmdResult::Err("internal", "Unhandled method exception"); }
            return res.ok ? MakeOk(id, res.body) : MakeError(id, res.code, res.message, res.details);
        }

        CmdResult Dispatch(const std::string& method, const GakuMotion::jsonlite::Value& params)
        {
            if (method == "bridge.hello")
            {
                std::string caps = "[";
                for (size_t i = 0; i < std::size(kSupportedMethods); ++i)
                {
                    if (i) caps += ",";
                    caps += JsonString(kSupportedMethods[i]);
                }
                caps += "]";
                return CmdResult::Ok("{\"protocol\":1,\"sessionId\":" +
                    JsonNumber(static_cast<double>(sessionId_)) +
                    ",\"pluginVersion\":" + JsonString(hooks_.pluginVersion) +
                    ",\"capabilities\":" + caps + "}");
            }
            if (method == "bridge.ping")
            {
                return CmdResult::Ok("{\"pong\":true}");
            }
            if (method == "appearance.get")
            {
                return CmdResult::Ok(BuildAppearanceJson());
            }
            if (method == "shell.setTextInputActive")
            {
                const auto* active = params.Find("active");
                if (!active || !active->IsBool())
                    return CmdResult::Err("invalid_params", "active boolean is required");
                textInputActive_.store(active->boolValue, std::memory_order_release);
                return CmdResult::Ok();
            }
            if (method == "state.get")
            {
                return CmdResult::Ok(BuildSnapshotJson(nullptr));
            }
            if (method == "config.set")
            {
                return HandleConfigSet(params);
            }
            if (method == "capture.take")
            {
                return HandleCaptureTake(params);
            }
            if (method == "folder.open")
            {
                return HandleFolderOpen();
            }
            if (method == "folder.browse")
            {
                return HandleFolderBrowse();
            }
            return CmdResult::Err("unknown_method", "Unknown method: " + method);
        }

        static bool ParseConfigFields(const GakuMotion::jsonlite::Value& src, ScreenshotConfig& next,
            const std::string& defaultDir, std::string& error)
        {
            if (const auto* wVal = src.Find("width"))
            {
                if (!wVal->IsNumber() || wVal->numberValue < 0 ||
                    wVal->numberValue != std::floor(wVal->numberValue) ||
                    wVal->numberValue > static_cast<double>(kMaxCaptureDimension))
                {
                    error = "width must be 0 or an integer in 1..7680";
                    return false;
                }
                next.width = static_cast<uint32_t>(wVal->numberValue);
            }
            if (const auto* hVal = src.Find("height"))
            {
                if (!hVal->IsNumber() || hVal->numberValue < 0 ||
                    hVal->numberValue != std::floor(hVal->numberValue) ||
                    hVal->numberValue > static_cast<double>(kMaxCaptureDimension))
                {
                    error = "height must be 0 or an integer in 1..7680";
                    return false;
                }
                next.height = static_cast<uint32_t>(hVal->numberValue);
            }
            if (!IsValidResolution(next.width, next.height, kMaxCaptureDimension))
            {
                error = "width and height must both be 0 (native) or both in 1..7680";
                return false;
            }

            if (const auto* fmtVal = src.Find("imageFormat"))
            {
                if (!ParseImageFormatValue(fmtVal, next.imageFormat))
                {
                    error = "imageFormat must be 'png' or 'jpeg'";
                    return false;
                }
            }
            if (const auto* qVal = src.Find("jpegQuality"))
            {
                if (!qVal->IsNumber() || qVal->numberValue < 1.0 || qVal->numberValue > 100.0 ||
                    qVal->numberValue != std::floor(qVal->numberValue))
                {
                    error = "jpegQuality must be an integer in 1..100";
                    return false;
                }
                next.jpegQuality = static_cast<uint32_t>(qVal->numberValue);
            }
            if (const auto* aVal = src.Find("alphaMode"))
            {
                if (!ParseAlphaModeValue(aVal, next.alphaMode))
                {
                    error = "alphaMode must be 'auto_greenscreen', 'force_transparent', or 'opaque'";
                    return false;
                }
            }
            if (const auto* dirVal = src.Find("outputDir"))
            {
                if (!dirVal->IsString())
                {
                    error = "outputDir must be a string";
                    return false;
                }
                if (dirVal->stringValue.empty())
                {
                    next.outputDirUtf8 = defaultDir;
                }
                else
                {
                    std::wstring checkWide;
                    if (!Utf8ToWide(dirVal->stringValue, checkWide))
                    {
                        error = "outputDir must be valid UTF-8 without null bytes";
                        return false;
                    }
                    next.outputDirUtf8 = dirVal->stringValue;
                }
            }
            return true;
        }

        CmdResult HandleConfigSet(const GakuMotion::jsonlite::Value& params)
        {
            const auto* nested = params.Find("config");
            if (nested && !nested->IsObject())
                return CmdResult::Err("invalid_params", "config must be an object");
            const GakuMotion::jsonlite::Value& src = nested ? *nested : params;

            ScreenshotConfig next = configStore_.Get();
            std::string error;
            if (!ParseConfigFields(src, next, configStore_.DefaultOutputDirUtf8(), error))
                return CmdResult::Err("invalid_params", error);

            if (!configStore_.Update(next, true))
                return CmdResult::Err("io_error", "Failed to save ScreenshotManager config.json");

            return CmdResult::Ok("{\"config\":" + SerializeConfigJson(configStore_.Get()) + "}");
        }

        CmdResult HandleCaptureTake(const GakuMotion::jsonlite::Value& params)
        {
            ScreenshotConfig cfg = configStore_.Get();
            std::string error;
            if (!ParseConfigFields(params, cfg, configStore_.DefaultOutputDirUtf8(), error))
                return CmdResult::Err("invalid_params", error);

            const std::filesystem::path outDir = PathFromUtf8(cfg.outputDirUtf8);
            if (outDir.empty())
                return CmdResult::Err("invalid_params", "Output directory is invalid");

            std::error_code ec;
            std::filesystem::create_directories(outDir, ec);
            if (ec)
                return CmdResult::Err("io_error", "Cannot create output directory: " + ec.message());

            const std::filesystem::path targetPath = GenerateScreenshotFilePath(outDir, cfg.imageFormat);
            const std::string targetUtf8 = PathToUtf8(targetPath);

            GakuScreenshotFileRequestV1 req{};
            req.abiVersion = GAKU_SCREENSHOT_VERSION;
            req.structSize = sizeof(GakuScreenshotFileRequestV1);
            req.width = cfg.width;
            req.height = cfg.height;
            req.imageFormat = cfg.imageFormat;
            req.jpegQuality = cfg.jpegQuality;
            req.alphaMode = cfg.alphaMode;
            if (targetUtf8.size() >= sizeof(req.outputPathUtf8))
                return CmdResult::Err("invalid_params", "Output file path exceeds buffer limit");
            std::memcpy(req.outputPathUtf8, targetUtf8.c_str(), targetUtf8.size() + 1u);

            uint64_t ticket = 0;
            const GakuStatus st = engine_.RequestFileCapture(&req, &ticket, outDir);
            if (st == GAKU_E_UNAVAILABLE)
                return CmdResult::Err("not_ready", "Game runtime or main camera is not ready");
            if (st != GAKU_OK)
                return CmdResult::Err("failed", "Capture request failed (status=" + std::to_string(st) + ")");

            return CmdResult::Ok("{\"ticket\":" + std::to_string(ticket) +
                ",\"outputPath\":" + JsonString(targetUtf8) + "}");
        }

        CmdResult HandleFolderOpen()
        {
            const ScreenshotConfig cfg = configStore_.Get();
            const std::filesystem::path dir = PathFromUtf8(cfg.outputDirUtf8);
            if (dir.empty())
                return CmdResult::Err("invalid_params", "Output directory is invalid");

            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec)
                return CmdResult::Err("io_error", "Cannot create output directory: " + ec.message());

            if (!hooks_.openFolder)
                return CmdResult::Err("not_implemented", "Host folder opener is unavailable");
            std::string err;
            if (!hooks_.openFolder(cfg.outputDirUtf8, err))
                return CmdResult::Err("failed", err.empty() ? "Failed to open output folder" : err);

            return CmdResult::Ok("{\"path\":" + JsonString(cfg.outputDirUtf8) + "}");
        }

        CmdResult HandleFolderBrowse()
        {
            if (!hooks_.browseFolder)
                return CmdResult::Err("not_implemented", "Host folder picker is unavailable");

            ScreenshotConfig cfg = configStore_.Get();
            const BrowseFolderResult res = hooks_.browseFolder(cfg.outputDirUtf8);
            if (!res.started)
            {
                const std::string code = (res.error == "busy") ? "busy" : "failed";
                return CmdResult::Err(code, res.error.empty() ? "Cannot start folder picker" : res.error);
            }

            if (res.pending)
            {
                return CmdResult::Ok("{\"pending\":true,\"cancelled\":false,\"path\":\"\"}");
            }

            if (!res.cancelled && !res.pathUtf8.empty())
            {
                cfg.outputDirUtf8 = res.pathUtf8;
                configStore_.Update(cfg, true);
            }
            return CmdResult::Ok("{\"pending\":false,\"cancelled\":" + JsonBool(res.cancelled) +
                ",\"path\":" + JsonString(res.cancelled ? "" : res.pathUtf8) + "}");
        }

        std::string BuildAppearanceJson() const
        {
            AppearanceInfo info;
            if (hooks_.appearance) info = hooks_.appearance();
            return "{\"theme\":" + std::to_string(info.theme) +
                ",\"uiScale\":" + JsonNumber(info.uiScale) + "}";
        }

        std::string BuildSnapshotJson(std::vector<CaptureCompletionNotice>* outCompletions)
        {
            GakuScreenshotCapabilitiesV1 caps{};
            engine_.GetCapabilities(&caps);

            bool inFlight = false;
            uint32_t pendingCount = 0;
            std::string lastError;
            std::vector<RecentCaptureEntry> recent;
            std::vector<CaptureCompletionNotice> completions;
            engine_.CopyStatusSnapshot(inFlight, pendingCount, lastError, recent, completions);
            if (outCompletions) *outCompletions = std::move(completions);

            const ScreenshotConfig cfg = configStore_.Get();
            std::string recentJson = "[";
            for (size_t i = 0; i < recent.size(); ++i)
            {
                if (i) recentJson += ",";
                const RecentCaptureEntry& r = recent[i];
                recentJson += "{\"id\":" + std::to_string(r.id) +
                    ",\"path\":" + JsonString(r.pathUtf8) +
                    ",\"fileName\":" + JsonString(r.fileNameUtf8) +
                    ",\"width\":" + std::to_string(r.width) +
                    ",\"height\":" + std::to_string(r.height) +
                    ",\"imageFormat\":" + JsonString(ImageFormatToString(r.imageFormat)) +
                    ",\"hasAlpha\":" + JsonBool(r.hasAlpha) +
                    ",\"sizeBytes\":" + std::to_string(r.sizeBytes) +
                    ",\"timestamp\":" + JsonString(r.timestamp) + "}";
            }
            recentJson += "]";

            return "{\"config\":" + SerializeConfigJson(cfg) +
                ",\"capabilities\":{\"runtimeReady\":" + JsonBool(caps.runtimeReady != 0) +
                ",\"cameraAvailable\":" + JsonBool(caps.cameraAvailable != 0) +
                ",\"greenScreenDetected\":" + JsonBool(caps.greenScreenDetected != 0) +
                ",\"maxDimension\":" + std::to_string(caps.maxDimension) +
                ",\"windowWidth\":" + std::to_string(caps.windowWidth) +
                ",\"windowHeight\":" + std::to_string(caps.windowHeight) + "}" +
                ",\"captureStatus\":{\"inFlight\":" + JsonBool(inFlight) +
                ",\"pendingCount\":" + std::to_string(pendingCount) +
                ",\"lastError\":" + JsonString(lastError) + "}" +
                ",\"recentCaptures\":" + recentJson +
                ",\"appearance\":" + BuildAppearanceJson() + "}";
        }

        void Tick()
        {
            if (hooks_.pollBrowseFolder)
            {
                BrowseFolderResult browse;
                if (hooks_.pollBrowseFolder(browse))
                {
                    if (!browse.cancelled && !browse.pathUtf8.empty())
                    {
                        ScreenshotConfig cfg = configStore_.Get();
                        cfg.outputDirUtf8 = browse.pathUtf8;
                        configStore_.Update(cfg, true);
                    }
                    Emit("folder.browsed", "{\"cancelled\":" + JsonBool(browse.cancelled) +
                        ",\"path\":" + JsonString(browse.cancelled ? "" : browse.pathUtf8) +
                        ",\"error\":" + JsonString(browse.error) + "}");
                }
            }

            if (!visible_ && !baselinePending_)
            {
                // Drain completion notices so stale toasts do not replay when panel reopens;
                // recentCaptures still updates via state.changed on OnShow.
                bool dummyInFlight = false;
                uint32_t dummyPending = 0;
                std::string dummyErr;
                std::vector<RecentCaptureEntry> dummyRecent;
                std::vector<CaptureCompletionNotice> dropped;
                engine_.CopyStatusSnapshot(dummyInFlight, dummyPending, dummyErr, dummyRecent, dropped);
                return;
            }

            std::vector<CaptureCompletionNotice> completions;
            const std::string stateJson = BuildSnapshotJson(&completions);
            const std::string appearanceJson = BuildAppearanceJson();

            for (const auto& c : completions)
            {
                Emit("capture.completed", "{\"ticket\":" + std::to_string(c.ticket) +
                    ",\"ok\":" + JsonBool(c.ok) +
                    ",\"path\":" + JsonString(c.pathUtf8) +
                    ",\"fileName\":" + JsonString(c.fileNameUtf8) +
                    ",\"width\":" + std::to_string(c.width) +
                    ",\"height\":" + std::to_string(c.height) +
                    ",\"imageFormat\":" + JsonString(ImageFormatToString(c.imageFormat)) +
                    ",\"hasAlpha\":" + JsonBool(c.hasAlpha) +
                    ",\"sizeBytes\":" + std::to_string(c.sizeBytes) +
                    ",\"timestamp\":" + JsonString(c.timestamp) +
                    ",\"error\":" + JsonString(c.errorMessage) + "}");
            }

            const bool baseline = baselinePending_;
            baselinePending_ = false;
            if (baseline || appearanceJson != lastAppearanceJson_)
            {
                lastAppearanceJson_ = appearanceJson;
                Emit("appearance.changed", appearanceJson);
            }
            if (baseline || stateJson != lastStateJson_)
            {
                lastStateJson_ = stateJson;
                Emit("state.changed", stateJson);
            }
        }

        ScreenshotEngine& engine_;
        ConfigStore& configStore_;
        HostHooks hooks_;
        uint64_t sessionId_{0};
        uint64_t seq_{0};
        std::atomic<bool> closed_{false};
        std::atomic<bool> textInputActive_{false};
        bool visible_{false};
        bool baselinePending_{true};

        mutable std::mutex sinkMutex_;
        GakuWebPanelEventSink sink_{nullptr};
        void* sinkUserData_{nullptr};
        std::wstring assetsDirCache_;

        std::string lastStateJson_;
        std::string lastAppearanceJson_;
    };
} // namespace ScreenshotManager::WebPanel
