// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "../include/GakuPlugin.h"
#include "../include/GakuPluginMetadata.h"
#include "../include/GakuPaths.hpp"
#include "../include/GakuScreenshot.h"
#include "../include/GakuWebPanel.h"
#include "ScreenshotConfig.hpp"
#include "ScreenshotEngine.hpp"
#include "ScreenshotWebPanel.hpp"

#include <windows.h>
#include <objbase.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace
{
    void ModuleAnchor() {}

    std::wstring GetModuleDirectory()
    {
        HMODULE mod = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ModuleAnchor), &mod) || !mod)
        {
            return {};
        }
        wchar_t buffer[32768]{};
        const DWORD len = GetModuleFileNameW(mod, buffer, static_cast<DWORD>(std::size(buffer)));
        if (!len || len >= std::size(buffer)) return {};
        return std::filesystem::path(buffer, buffer + len).parent_path().wstring();
    }

    struct FolderBrowserState
    {
        std::mutex mutex;
        bool active{false};
        bool finished{false};
        bool cancelled{false};
        std::string pathUtf8;
        std::string error;
        std::atomic<bool> cancelRequested{false};
        std::atomic<HWND> dialogHwnd{nullptr};
        std::thread worker;

        ~FolderBrowserState()
        {
            CancelAndJoin();
        }

        ScreenshotManager::WebPanel::BrowseFolderResult Begin(const std::string& initialDirUtf8)
        {
            ScreenshotManager::WebPanel::BrowseFolderResult res;
            std::thread oldThread;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (active)
                {
                    res.started = false;
                    res.error = "busy";
                    return res;
                }
                if (worker.joinable()) oldThread = std::move(worker);
                active = true;
                finished = false;
                cancelled = false;
                pathUtf8.clear();
                error.clear();
                cancelRequested.store(false, std::memory_order_release);
                dialogHwnd.store(nullptr, std::memory_order_release);
                worker = std::thread([this, initialDirUtf8]() { RunDialog(initialDirUtf8); });
            }
            if (oldThread.joinable()) oldThread.join();
            res.started = true;
            res.pending = true;
            return res;
        }

        bool Poll(ScreenshotManager::WebPanel::BrowseFolderResult& outResult)
        {
            std::thread joinableWorker;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!finished) return false;
                outResult.started = true;
                outResult.pending = false;
                outResult.cancelled = cancelled;
                outResult.pathUtf8 = pathUtf8;
                outResult.error = error;
                finished = false;
                active = false;
                if (worker.joinable()) joinableWorker = std::move(worker);
            }
            if (joinableWorker.joinable()) joinableWorker.join();
            return true;
        }

        void CancelAndJoin()
        {
            cancelRequested.store(true, std::memory_order_release);
            if (HWND hwnd = dialogHwnd.load(std::memory_order_acquire))
            {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            std::thread joinableWorker;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (worker.joinable()) joinableWorker = std::move(worker);
                active = false;
                finished = false;
            }
            if (joinableWorker.joinable()) joinableWorker.join();
        }

    private:
        void RunDialog(const std::string& initialDirUtf8)
        {
            const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            bool picked = false;
            bool wasCancelled = false;
            std::string pickedUtf8;
            std::string errMsg;

            if (SUCCEEDED(comHr) || comHr == RPC_E_CHANGED_MODE)
            {
                Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
                HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&dialog));
                if (SUCCEEDED(hr) && dialog)
                {
                    FILEOPENDIALOGOPTIONS opts = 0;
                    if (SUCCEEDED(dialog->GetOptions(&opts)))
                    {
                        dialog->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                            FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
                    }
                    dialog->SetTitle(L"\u9009\u62e9\u622a\u56fe\u4fdd\u5b58\u76ee\u5f55");

                    std::wstring initialWide;
                    if (!initialDirUtf8.empty() && ScreenshotManager::Utf8ToWide(initialDirUtf8, initialWide))
                    {
                        std::error_code ec;
                        if (std::filesystem::is_directory(initialWide, ec))
                        {
                            Microsoft::WRL::ComPtr<IShellItem> folderItem;
                            if (SUCCEEDED(SHCreateItemFromParsingName(initialWide.c_str(), nullptr,
                                    IID_PPV_ARGS(&folderItem))) && folderItem)
                            {
                                dialog->SetFolder(folderItem.Get());
                            }
                        }
                    }

                    if (!cancelRequested.load(std::memory_order_acquire))
                    {
                        hr = dialog->Show(nullptr);
                        if (SUCCEEDED(hr))
                        {
                            Microsoft::WRL::ComPtr<IShellItem> resultItem;
                            if (SUCCEEDED(dialog->GetResult(&resultItem)) && resultItem)
                            {
                                PWSTR widePath = nullptr;
                                if (SUCCEEDED(resultItem->GetDisplayName(SIGDN_FILESYSPATH, &widePath)) && widePath)
                                {
                                    if (ScreenshotManager::WideToUtf8(widePath, pickedUtf8))
                                        picked = true;
                                    else
                                        errMsg = "Selected path is not valid UTF-8";
                                    CoTaskMemFree(widePath);
                                }
                            }
                        }
                        else if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
                        {
                            wasCancelled = true;
                        }
                        else
                        {
                            char buf[64]{};
                            std::snprintf(buf, sizeof(buf), "Folder dialog failed (0x%08lX)", static_cast<unsigned long>(hr));
                            errMsg = buf;
                        }
                    }
                    else
                    {
                        wasCancelled = true;
                    }
                }
                else
                {
                    errMsg = "Cannot create IFileOpenDialog";
                }
            }
            else
            {
                errMsg = "COM STA initialization failed";
            }

            if (SUCCEEDED(comHr)) CoUninitialize();

            std::lock_guard<std::mutex> lock(mutex);
            if (!cancelRequested.load(std::memory_order_acquire))
            {
                cancelled = !picked && (wasCancelled || errMsg.empty());
                pathUtf8 = picked ? pickedUtf8 : std::string{};
                error = errMsg;
                finished = true;
            }
            dialogHwnd.store(nullptr, std::memory_order_release);
        }
    };

    struct PluginContext
    {
        GakuHostApi host{};
        GakuLogService log{};
        GakuRuntimeService runtime{};
        GakuMainThreadService mainThread{};
        GakuHotkeyService hotkeys{};
        GakuAppearanceService appearance{};
        GakuPathsV1 paths{};
        bool hasPaths{false};

        ScreenshotManager::ConfigStore configStore;
        ScreenshotManager::ScreenshotEngine engine;
        FolderBrowserState folderBrowser;
        std::unique_ptr<ScreenshotManager::WebPanel::Bridge> webBridge;

        GakuScreenshotServiceV1 captureService{};
        GakuWebPanelServiceV1 webPanelService{};
        bool hotkeyRegistered{false};
        std::atomic<bool> runtimeNotified{false};
        std::wstring assetsDir;

        void Log(GakuLogLevel level, const char* msg) const
        {
            if (log.write) log.write(log.context, level, msg);
        }
    };

    // ---- GakuScreenshotServiceV1 C ABI thunks ------------------------------

    GakuStatus ApiGetCapabilities(void* context, GakuScreenshotCapabilitiesV1* output)
    {
        auto* ctx = static_cast<PluginContext*>(context);
        return ctx ? ctx->engine.GetCapabilities(output) : GAKU_E_INVALID_ARGUMENT;
    }

    GakuStatus ApiRequestFileCapture(void* context, const GakuScreenshotFileRequestV1* request, uint64_t* outTicket)
    {
        auto* ctx = static_cast<PluginContext*>(context);
        if (!ctx) return GAKU_E_INVALID_ARGUMENT;
        const auto cfg = ctx->configStore.Get();
        const std::filesystem::path fallbackDir = ScreenshotManager::PathFromUtf8(cfg.outputDirUtf8);
        return ctx->engine.RequestFileCapture(request, outTicket, fallbackDir);
    }

    GakuStatus ApiPollCapture(void* context, uint64_t ticket, GakuScreenshotResultV1* output)
    {
        auto* ctx = static_cast<PluginContext*>(context);
        return ctx ? ctx->engine.PollCapture(ticket, output) : GAKU_E_INVALID_ARGUMENT;
    }

    GakuStatus ApiReleaseTicket(void* context, uint64_t ticket)
    {
        auto* ctx = static_cast<PluginContext*>(context);
        return ctx ? ctx->engine.ReleaseTicket(ticket) : GAKU_E_INVALID_ARGUMENT;
    }

    GakuStatus ApiCaptureFrameImmediate(void* context, const GakuScreenshotFrameRequestV1* request,
        void* outPixels, size_t capacity, GakuScreenshotFrameInfoV1* outInfo,
        char* errorBuffer, uint32_t errorCapacity)
    {
        auto* ctx = static_cast<PluginContext*>(context);
        return ctx ? ctx->engine.CaptureFrameImmediate(request, outPixels, capacity, outInfo, errorBuffer, errorCapacity)
                   : GAKU_E_INVALID_ARGUMENT;
    }

    GakuStatus ApiArmPresentCapture(void* context, uint64_t token, uint32_t width, uint32_t height,
        uint32_t pixelFormat)
    {
        auto* ctx = static_cast<PluginContext*>(context);
        return ctx ? ctx->engine.ArmPresentCapture(token, width, height, pixelFormat) : GAKU_E_INVALID_ARGUMENT;
    }

    GakuStatus ApiConsumePresentCapture(void* context, uint64_t* outToken, void* outPixels, size_t capacity,
        GakuScreenshotFrameInfoV1* outInfo, char* errorBuffer, uint32_t errorCapacity)
    {
        auto* ctx = static_cast<PluginContext*>(context);
        return ctx ? ctx->engine.ConsumePresentCapture(outToken, outPixels, capacity, outInfo, errorBuffer, errorCapacity)
                   : GAKU_E_INVALID_ARGUMENT;
    }

    GakuStatus ApiCancelPresentCapture(void* context)
    {
        auto* ctx = static_cast<PluginContext*>(context);
        return ctx ? ctx->engine.CancelPresentCapture() : GAKU_E_INVALID_ARGUMENT;
    }

    // ---- GakuWebPanelServiceV1 C ABI thunks --------------------------------

    GakuStatus ApiWebGetAssets(void* context, GakuWebPanelAssetsV1* output)
    {
        auto* bridge = static_cast<ScreenshotManager::WebPanel::Bridge*>(context);
        return bridge ? bridge->GetAssets(output) : GAKU_E_UNAVAILABLE;
    }

    void ApiWebOnShow(void* context)
    {
        if (auto* bridge = static_cast<ScreenshotManager::WebPanel::Bridge*>(context)) bridge->OnShow();
    }

    void ApiWebOnHide(void* context)
    {
        if (auto* bridge = static_cast<ScreenshotManager::WebPanel::Bridge*>(context)) bridge->OnHide();
    }

    void ApiWebOnUiTick(void* context)
    {
        if (auto* bridge = static_cast<ScreenshotManager::WebPanel::Bridge*>(context)) bridge->OnUiTick();
    }

    void ApiWebSetEventSink(void* context, GakuWebPanelEventSink sink, void* userData)
    {
        if (auto* bridge = static_cast<ScreenshotManager::WebPanel::Bridge*>(context))
            bridge->SetEventSink(sink, userData);
    }

    GakuStatus ApiWebHandleMessage(void* context, const char* requestJson,
        uint32_t responseCapacity, char* responseJson, uint32_t* requiredBytes)
    {
        auto* bridge = static_cast<ScreenshotManager::WebPanel::Bridge*>(context);
        return bridge ? bridge->HandleMessage(requestJson, responseCapacity, responseJson, requiredBytes)
                      : GAKU_E_UNAVAILABLE;
    }

    void ApiWebOnShutdown(void* context)
    {
        if (auto* bridge = static_cast<ScreenshotManager::WebPanel::Bridge*>(context)) bridge->OnShutdown();
    }

    // ---- Hotkey callback (Unity main thread) -------------------------------

    constexpr const char* kTakeScreenshotActionId = "take-screenshot";

    void TakeScreenshotHotkey(void* userData)
    {
        auto* ctx = static_cast<PluginContext*>(userData);
        if (!ctx) return;

        const ScreenshotManager::ScreenshotConfig cfg = ctx->configStore.Get();
        const std::filesystem::path outDir = ScreenshotManager::PathFromUtf8(cfg.outputDirUtf8);
        if (outDir.empty())
        {
            ctx->Log(GAKU_LOG_WARNING, "[ScreenshotManager] Hotkey F7 skipped: output directory is invalid");
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        if (ec)
        {
            ctx->Log(GAKU_LOG_WARNING, "[ScreenshotManager] Hotkey F7 failed to create output directory");
            return;
        }

        const std::filesystem::path filePath = ScreenshotManager::GenerateScreenshotFilePath(outDir, cfg.imageFormat);
        const std::string pathUtf8 = ScreenshotManager::PathToUtf8(filePath);
        if (pathUtf8.empty() || pathUtf8.size() >= 1024u)
        {
            ctx->Log(GAKU_LOG_WARNING, "[ScreenshotManager] Hotkey F7 skipped: generated path invalid");
            return;
        }

        GakuScreenshotFileRequestV1 req{};
        req.abiVersion = GAKU_SCREENSHOT_VERSION;
        req.structSize = sizeof(GakuScreenshotFileRequestV1);
        req.width = cfg.width;
        req.height = cfg.height;
        req.imageFormat = cfg.imageFormat;
        req.jpegQuality = cfg.jpegQuality;
        req.alphaMode = cfg.alphaMode;
        std::memcpy(req.outputPathUtf8, pathUtf8.c_str(), pathUtf8.size() + 1u);

        uint64_t ticket = 0;
        const GakuStatus st = ctx->engine.RequestFileCapture(&req, &ticket, outDir);
        if (st == GAKU_OK)
        {
            char msg[256]{};
            std::snprintf(msg, sizeof(msg), "[ScreenshotManager] Hotkey capture started (ticket=%llu)",
                static_cast<unsigned long long>(ticket));
            ctx->Log(GAKU_LOG_INFO, msg);
        }
        else
        {
            char msg[160]{};
            std::snprintf(msg, sizeof(msg), "[ScreenshotManager] Hotkey capture unavailable (status=%d)", st);
            ctx->Log(GAKU_LOG_WARNING, msg);
        }
    }

    // ---- Lifecycle callbacks -----------------------------------------------

    GakuStatus Activate(const GakuHostApi* host, void** pluginState)
    {
        if (!host || !pluginState) return GAKU_E_INVALID_ARGUMENT;
        if (host->abiVersion != GAKU_ABI_VERSION) return GAKU_E_VERSION;
        if (host->structSize < sizeof(GakuHostApi)) return GAKU_E_SIZE;
        if (!host->queryService || !host->registerService) return GAKU_E_INVALID_ARGUMENT;

        auto ctx = std::make_unique<PluginContext>();
        ctx->host = *host;

        host->queryService(host->hostContext, GAKU_SERVICE_LOG, GAKU_SERVICE_VERSION,
            sizeof(ctx->log), &ctx->log);
        host->queryService(host->hostContext, GAKU_SERVICE_RUNTIME, GAKU_SERVICE_VERSION,
            sizeof(ctx->runtime), &ctx->runtime);
        host->queryService(host->hostContext, GAKU_SERVICE_MAIN_THREAD, GAKU_SERVICE_VERSION,
            sizeof(ctx->mainThread), &ctx->mainThread);
        host->queryService(host->hostContext, GAKU_SERVICE_HOTKEYS, GAKU_SERVICE_VERSION,
            sizeof(ctx->hotkeys), &ctx->hotkeys);
        host->queryService(host->hostContext, GAKU_SERVICE_APPEARANCE, GAKU_SERVICE_VERSION,
            sizeof(ctx->appearance), &ctx->appearance);

        ctx->hasPaths = (GakuPaths::Query(host, ctx->paths) == GAKU_OK);

        std::filesystem::path configRoot;
        if (ctx->hasPaths)
            GakuPaths::GetRoot(ctx->paths, GAKU_PATH_CONFIGS, configRoot);
        if (configRoot.empty() && ctx->runtime.localRoot && ctx->runtime.localRoot[0])
            configRoot = std::filesystem::path(ctx->runtime.localRoot) / L"Configs" / L"screenshotmanager";

        std::filesystem::path defaultOutputDir;
        if (ctx->hasPaths)
        {
            std::filesystem::path userDataRoot;
            if (GakuPaths::GetRoot(ctx->paths, GAKU_PATH_USER_DATA, userDataRoot) == GAKU_OK && !userDataRoot.empty())
                defaultOutputDir = userDataRoot / L"Screenshots";
        }
        if (defaultOutputDir.empty() && ctx->runtime.localRoot && ctx->runtime.localRoot[0])
            defaultOutputDir = std::filesystem::path(ctx->runtime.localRoot) / L"Screenshots";
        if (defaultOutputDir.empty())
            defaultOutputDir = std::filesystem::current_path() / L"Screenshots";

        std::string defaultOutputUtf8 = ScreenshotManager::PathToUtf8(defaultOutputDir);
        if (defaultOutputUtf8.empty()) defaultOutputUtf8 = "Screenshots";

        PluginContext* rawCtx = ctx.get();
        auto ensureConfigRoot = [rawCtx]() -> bool {
            if (rawCtx->hasPaths && rawCtx->paths.ensureRoot)
                return rawCtx->paths.ensureRoot(rawCtx->paths.context, GAKU_PATH_CONFIGS) == GAKU_OK;
            return true;
        };

        const std::filesystem::path configFile = configRoot.empty()
            ? std::filesystem::path{}
            : (configRoot / L"config.json");
        ctx->configStore.Configure(configFile, defaultOutputUtf8, ensureConfigRoot);

        ctx->engine.SetHostApi(host);
        ctx->engine.InitializeWorker();

        // Register screenshotmanager.capture/v1
        ctx->captureService.abiVersion = GAKU_SCREENSHOT_VERSION;
        ctx->captureService.structSize = sizeof(GakuScreenshotServiceV1);
        ctx->captureService.context = ctx.get();
        ctx->captureService.getCapabilities = &ApiGetCapabilities;
        ctx->captureService.requestFileCapture = &ApiRequestFileCapture;
        ctx->captureService.pollCapture = &ApiPollCapture;
        ctx->captureService.releaseTicket = &ApiReleaseTicket;
        ctx->captureService.captureFrameImmediate = &ApiCaptureFrameImmediate;
        ctx->captureService.armPresentCapture = &ApiArmPresentCapture;
        ctx->captureService.consumePresentCapture = &ApiConsumePresentCapture;
        ctx->captureService.cancelPresentCapture = &ApiCancelPresentCapture;

        const GakuStatus capReg = host->registerService(host->hostContext,
            GAKU_SERVICE_SCREENSHOT_CAPTURE, sizeof(GakuScreenshotServiceV1), &ctx->captureService);
        if (capReg != GAKU_OK) return capReg;

        // Register screenshotmanager.web-panel/v1
        const std::wstring dllDir = GetModuleDirectory();
        if (!dllDir.empty())
            ctx->assetsDir = (std::filesystem::path(dllDir) / L"ScreenshotManager.UI").wstring();
        else
            ctx->assetsDir = (std::filesystem::current_path() / L"ScreenshotManager.UI").wstring();

        ScreenshotManager::WebPanel::HostHooks webHooks;
        webHooks.pluginVersion = "0.1.0";
        webHooks.assetsDir = [rawCtx]() { return rawCtx->assetsDir; };
        webHooks.openFolder = [](const std::string& dirUtf8, std::string& err) -> bool {
            std::wstring wide;
            if (!ScreenshotManager::Utf8ToWide(dirUtf8, wide))
            {
                err = "Invalid folder path";
                return false;
            }
            const auto rc = reinterpret_cast<INT_PTR>(
                ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (rc <= 32)
            {
                err = "ShellExecuteW failed (code " + std::to_string(rc) + ")";
                return false;
            }
            return true;
        };
        webHooks.browseFolder = [rawCtx](const std::string& initialDirUtf8) {
            return rawCtx->folderBrowser.Begin(initialDirUtf8);
        };
        webHooks.pollBrowseFolder = [rawCtx](ScreenshotManager::WebPanel::BrowseFolderResult& outResult) {
            return rawCtx->folderBrowser.Poll(outResult);
        };
        webHooks.cancelBrowseFolder = [rawCtx]() {
            rawCtx->folderBrowser.CancelAndJoin();
        };
        webHooks.appearance = [rawCtx]() {
            ScreenshotManager::WebPanel::AppearanceInfo info;
            if (rawCtx->appearance.getAppearance)
            {
                GakuUiAppearanceV1 data{};
                if (rawCtx->appearance.getAppearance(sizeof(data), &data) == GAKU_OK)
                {
                    info.theme = static_cast<int>(data.theme);
                    info.uiScale = data.uiScale;
                }
            }
            return info;
        };

        ctx->webBridge = std::make_unique<ScreenshotManager::WebPanel::Bridge>(
            ctx->engine, ctx->configStore, std::move(webHooks));

        ctx->webPanelService.abiVersion = GAKU_WEB_PANEL_VERSION;
        ctx->webPanelService.structSize = sizeof(GakuWebPanelServiceV1);
        ctx->webPanelService.context = ctx->webBridge.get();
        ctx->webPanelService.getAssets = &ApiWebGetAssets;
        ctx->webPanelService.onShow = &ApiWebOnShow;
        ctx->webPanelService.onHide = &ApiWebOnHide;
        ctx->webPanelService.onUiTick = &ApiWebOnUiTick;
        ctx->webPanelService.setEventSink = &ApiWebSetEventSink;
        ctx->webPanelService.handleMessage = &ApiWebHandleMessage;
        ctx->webPanelService.onShutdown = &ApiWebOnShutdown;

        const GakuStatus webReg = host->registerService(host->hostContext,
            "screenshotmanager" GAKU_WEB_PANEL_SERVICE_SUFFIX,
            sizeof(GakuWebPanelServiceV1), &ctx->webPanelService);
        if (webReg == GAKU_OK)
            ctx->Log(GAKU_LOG_INFO, "[ScreenshotManager] Registered screenshotmanager.web-panel/v1");
        else
            ctx->Log(GAKU_LOG_WARNING, "[ScreenshotManager] Failed to register screenshotmanager.web-panel/v1");

        // Optional foreground hotkey: VK_F7 (F11 belongs to GakuLocalify toggle-fullscreen)
        if (ctx->hotkeys.registerHotkey)
        {
            const GakuHotkeyBinding binding{
                GAKU_SERVICE_VERSION,
                sizeof(GakuHotkeyBinding),
                kTakeScreenshotActionId,
                "ScreenshotManager: Capture Screenshot",
                VK_F7,
                0u,
                &TakeScreenshotHotkey,
                ctx.get()
            };
            const GakuStatus hkStatus = ctx->hotkeys.registerHotkey(ctx->hotkeys.context, &binding);
            if (hkStatus == GAKU_OK)
            {
                ctx->hotkeyRegistered = true;
                ctx->Log(GAKU_LOG_INFO, "[ScreenshotManager] Registered hotkey F7 (take-screenshot)");
            }
            else
            {
                char msg[160]{};
                std::snprintf(msg, sizeof(msg),
                    "[ScreenshotManager] Hotkey F7 registration skipped or conflicted (status=%d)", hkStatus);
                ctx->Log(GAKU_LOG_WARNING, msg);
            }
        }

        ctx->Log(GAKU_LOG_INFO, "[ScreenshotManager] Activated; screenshotmanager.capture/v1 registered.");
        *pluginState = ctx.release();
        return GAKU_OK;
    }

    void OnRuntimeReady(void* pluginState)
    {
        auto* ctx = static_cast<PluginContext*>(pluginState);
        if (!ctx) return;
        ctx->runtimeNotified.store(true, std::memory_order_release);
        void* gameAssembly = ctx->runtime.getGameAssembly ? ctx->runtime.getGameAssembly() : nullptr;
        ctx->engine.OnRuntimeReady(gameAssembly);
    }

    void OnTick(void* pluginState, float /*deltaTime*/)
    {
        auto* ctx = static_cast<PluginContext*>(pluginState);
        if (!ctx) return;
        if (!ctx->runtimeNotified.load(std::memory_order_acquire) && ctx->runtime.getGameAssembly)
        {
            if (void* gameAssembly = ctx->runtime.getGameAssembly())
            {
                ctx->runtimeNotified.store(true, std::memory_order_release);
                ctx->engine.OnRuntimeReady(gameAssembly);
            }
        }
        ctx->engine.OnTick();
    }

    void Shutdown(void* pluginState)
    {
        auto* ctx = static_cast<PluginContext*>(pluginState);
        if (!ctx) return;
        if (ctx->hotkeyRegistered && ctx->hotkeys.unregisterHotkey)
        {
            ctx->hotkeys.unregisterHotkey(ctx->hotkeys.context, kTakeScreenshotActionId);
            ctx->hotkeyRegistered = false;
        }
        if (ctx->webBridge)
        {
            ctx->webBridge->Invalidate();
        }
        ctx->folderBrowser.CancelAndJoin();
        ctx->engine.Shutdown();
        ctx->Log(GAKU_LOG_INFO, "[ScreenshotManager] Shut down.");
        delete ctx;
    }
} // namespace

extern "C" GAKU_PLUGIN_EXPORT GakuStatus GakuPlugin_Query(
    uint32_t requestedAbi, uint32_t outputSize, GakuPluginDescriptor* output)
{
    if (requestedAbi != GAKU_ABI_VERSION) return GAKU_E_VERSION;
    if (!output) return GAKU_E_INVALID_ARGUMENT;
    if (outputSize < sizeof(GakuPluginDescriptor)) return GAKU_E_SIZE;

    GakuPluginDescriptor desc{};
    desc.abiVersion = GAKU_ABI_VERSION;
    desc.structSize = sizeof(GakuPluginDescriptor);
    desc.id = "screenshotmanager";
    desc.name = "ScreenshotManager";
    desc.version = "0.1.0";
    desc.activate = &Activate;
    desc.onRuntimeReady = &OnRuntimeReady;
    desc.onTick = &OnTick;
    desc.shutdown = &Shutdown;
    *output = desc;
    return GAKU_OK;
}

extern "C" GAKU_PLUGIN_EXPORT GakuStatus GakuPlugin_QueryMetadata(
    uint32_t requestedAbi, uint32_t outputSize, GakuPluginMetadataV1* output)
{
    if (requestedAbi != GAKU_METADATA_ABI_VERSION) return GAKU_E_VERSION;
    if (!output) return GAKU_E_INVALID_ARGUMENT;
    if (outputSize < sizeof(GakuPluginMetadataV1)) return GAKU_E_SIZE;

    static const char* const kMaintainers[] = {"crested-ezios"};
    static const GakuPluginLocalizedTextV1 kLocalizedSummaries[] = {
        {"en", "Multi-resolution offscreen screenshot capture and ACES green-screen matte plugin for GakuLoader."},
        {"zh-Hans", "适用于 GakuLoader 的多分辨率离屏截图与 ACES 绿幕抠像插件。"},
        {"zh-Hant", "適用於 GakuLoader 的多解析度離屏截圖與 ACES 綠幕去背外掛。"},
        {"ja", "GakuLoader 向けのマルチ解像度オフスクリーン撮影および ACES グリーンスクリーン透過プラグイン。"},
    };
    static const GakuPluginDependencyV1 kDependencies[] = {
        {"gakumotion", "GakuMotion", ">=0.1.1", GAKU_DEPENDENCY_OPTIONAL, 0u},
    };

    const GakuPluginMetadataV1 metadata{
        GAKU_METADATA_ABI_VERSION,
        sizeof(GakuPluginMetadataV1),
        "ScreenshotManager Contributors",
        static_cast<uint32_t>(std::size(kMaintainers)),
        0u,
        kMaintainers,
        "",
        "stable",
        "Multi-resolution offscreen screenshot capture and ACES green-screen matte plugin for GakuLoader.",
        static_cast<uint32_t>(std::size(kLocalizedSummaries)),
        0u,
        kLocalizedSummaries,
        "https://github.com/crested-ezios/ScreenshotManager",
        "https://github.com/crested-ezios/ScreenshotManager/releases",
        "https://github.com/crested-ezios/ScreenshotManager/issues",
        "MIT",
        "1.1.0",
        static_cast<uint32_t>(std::size(kDependencies)),
        0u,
        kDependencies,
        0u,
        0u,
        nullptr,
    };
    *output = metadata;
    return GAKU_OK;
}
