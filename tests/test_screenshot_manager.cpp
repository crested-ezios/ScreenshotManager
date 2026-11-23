// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "../include/GakuPlugin.h"
#include "../include/GakuPluginMetadata.h"
#include "../include/GakuPaths.h"
#include "../include/GakuScreenshot.h"
#include "../include/GakuWebPanel.h"
#include "../include/GakuMotionStageState.h"
#include "../src/AlphaMatte.hpp"
#include "../src/WicImageWriter.hpp"
#include "../src/ScreenshotConfig.hpp"
#include "../src/ScreenshotEngine.hpp"
#include "../src/ScreenshotWebPanel.hpp"
#include "../src/GkJsonLite.hpp"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    int g_pass = 0;
    int g_fail = 0;
    std::string g_testName;

    void BeginTest(const char* name)
    {
        g_testName = name;
        std::printf("[TEST] %s\n", name);
    }

    void Check(bool cond, const std::string& msg)
    {
        if (cond)
        {
            ++g_pass;
        }
        else
        {
            ++g_fail;
            std::printf("  [FAIL] %s :: %s\n", g_testName.c_str(), msg.c_str());
        }
    }

    bool NearByte(uint8_t a, uint8_t b, int tol)
    {
        return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= tol;
    }

    std::filesystem::path MakeTempTestDir(const wchar_t* tag)
    {
        std::filesystem::path base = std::filesystem::temp_directory_path() /
            (std::wstring(L"ScreenshotManagerTest_") + tag + L"_" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        std::filesystem::create_directories(base, ec);
        return base;
    }

    // ---- Mock GakuHostApi for contract tests -------------------------------

    struct FakeHost
    {
        std::filesystem::path localRoot;
        std::filesystem::path configRoot;
        std::filesystem::path userDataRoot;
        uint32_t greenScreenActive{1};
        std::map<std::string, std::vector<uint8_t>> services;
        std::vector<std::string> registeredHotkeyIds;
        uint32_t registeredHotkeyVk{0};
        std::string registeredHotkeyDisplay;
        bool hotkeyUnregistered{false};
        std::vector<std::string> logs;
        bool exposePluginsService{false};
        uint32_t stageStateQueryCount{0};
        static inline GakuPluginLoadState s_gakuMotionState{GAKU_PLUGIN_STATE_NOT_FOUND};

        static FakeHost* FromCtx(void* ctx) { return static_cast<FakeHost*>(ctx); }

        static void LogWrite(void* ctx, GakuLogLevel, const char* msg)
        {
            if (auto* h = FromCtx(ctx); h && msg) h->logs.emplace_back(msg);
        }

        static void* GetGameAssembly() { return nullptr; }

        static GakuStatus EnqueueTask(void*, GakuTaskFn fn, void* ud)
        {
            if (fn) fn(ud);
            return GAKU_OK;
        }

        static uint32_t IsMainThread(void*) { return 1u; }

        static GakuStatus GetAppearance(uint32_t size, GakuUiAppearanceV1* out)
        {
            if (!out || size < sizeof(GakuUiAppearanceV1)) return GAKU_E_SIZE;
            out->apiVersion = GAKU_ABI_VERSION;
            out->structSize = sizeof(GakuUiAppearanceV1);
            out->theme = GAKU_UI_THEME_SAPPHIRE;
            out->uiScale = 1.25f;
            return GAKU_OK;
        }

        static GakuStatus RegisterHotkey(void* ctx, const GakuHotkeyBinding* b)
        {
            auto* h = FromCtx(ctx);
            if (!h || !b || !b->actionId) return GAKU_E_INVALID_ARGUMENT;
            h->registeredHotkeyIds.emplace_back(b->actionId);
            h->registeredHotkeyVk = b->virtualKey;
            h->registeredHotkeyDisplay = b->displayName ? b->displayName : "";
            return GAKU_OK;
        }

        static GakuStatus UnregisterHotkey(void* ctx, const char* actionId)
        {
            auto* h = FromCtx(ctx);
            if (!h || !actionId) return GAKU_E_INVALID_ARGUMENT;
            h->hotkeyUnregistered = true;
            return GAKU_OK;
        }

        static GakuStatus PathsGetRoot(void* ctx, GakuPathKind kind,
            wchar_t* output, uint32_t capacity, uint32_t* required)
        {
            auto* h = FromCtx(ctx);
            if (!h || !required) return GAKU_E_INVALID_ARGUMENT;
            std::filesystem::path root;
            if (kind == GAKU_PATH_CONFIGS) root = h->configRoot;
            else if (kind == GAKU_PATH_USER_DATA) root = h->userDataRoot;
            else return GAKU_E_INVALID_ARGUMENT;

            const std::wstring w = root.wstring();
            const uint32_t needed = static_cast<uint32_t>(w.size() + 1u);
            *required = needed;
            if (!output && capacity == 0) return GAKU_E_SIZE;
            if (!output || capacity < needed) return GAKU_E_SIZE;
            std::memcpy(output, w.c_str(), needed * sizeof(wchar_t));
            return GAKU_OK;
        }

        static GakuStatus PathsEnsureRoot(void* ctx, GakuPathKind kind)
        {
            auto* h = FromCtx(ctx);
            if (!h) return GAKU_E_INVALID_ARGUMENT;
            std::error_code ec;
            if (kind == GAKU_PATH_CONFIGS) std::filesystem::create_directories(h->configRoot, ec);
            else if (kind == GAKU_PATH_USER_DATA) std::filesystem::create_directories(h->userDataRoot, ec);
            else return GAKU_E_INVALID_ARGUMENT;
            return ec ? GAKU_E_INTERNAL : GAKU_OK;
        }

        static GakuStatus GetMotionStageState(void* ctx, GakuMotionStageStateV1* out)
        {
            auto* h = FromCtx(ctx);
            if (!h || !out) return GAKU_E_INVALID_ARGUMENT;
            out->abiVersion = GAKU_MOTION_STAGE_STATE_VERSION;
            out->structSize = sizeof(GakuMotionStageStateV1);
            out->sceneAllowed = 1u;
            out->injectionActive = 1u;
            out->greenScreenActive = h->greenScreenActive;
            out->overrideCameraClear = h->greenScreenActive;
            out->hideBackground = h->greenScreenActive;
            out->greenScreenColor[0] = 0.0f;
            out->greenScreenColor[1] = 1.0f;
            out->greenScreenColor[2] = 0.0f;
            out->greenScreenColor[3] = 1.0f;
            return GAKU_OK;
        }

        static GakuStatus PluginsGetHostInfo(uint32_t outputSize, GakuHostInfoV1* output)
        {
            if (!output) return GAKU_E_INVALID_ARGUMENT;
            if (outputSize < sizeof(GakuHostInfoV1)) return GAKU_E_SIZE;
            GakuHostInfoV1 info{};
            info.abiVersion = GAKU_PLUGINS_SERVICE_VERSION;
            info.structSize = sizeof(GakuHostInfoV1);
            info.pluginAbiVersion = GAKU_ABI_VERSION;
            info.pluginCount = 1u;
            std::snprintf(info.loaderVersion, sizeof(info.loaderVersion), "%s", GAKU_LOADER_VERSION);
            *output = info;
            return GAKU_OK;
        }

        static GakuStatus PluginsFindPlugin(const char* pluginId, uint32_t outputSize, GakuPluginSummaryV1* output)
        {
            if (!pluginId || !output) return GAKU_E_INVALID_ARGUMENT;
            if (outputSize < sizeof(GakuPluginSummaryV1)) return GAKU_E_SIZE;
            GakuPluginSummaryV1 summary{};
            summary.abiVersion = GAKU_PLUGINS_SERVICE_VERSION;
            summary.structSize = sizeof(GakuPluginSummaryV1);
            summary.state = (std::strcmp(pluginId, "gakumotion") == 0)
                ? s_gakuMotionState
                : GAKU_PLUGIN_STATE_NOT_FOUND;
            if (std::strcmp(pluginId, "gakumotion") == 0)
            {
                summary.hasMetadata = 1u;
                std::snprintf(summary.id, sizeof(summary.id), "%s", "gakumotion");
                std::snprintf(summary.name, sizeof(summary.name), "%s", "GakuMotion");
                std::snprintf(summary.version, sizeof(summary.version), "%s", "0.1.1");
            }
            *output = summary;
            return GAKU_OK;
        }

        static GakuStatus PluginsGetPluginAt(uint32_t, uint32_t outputSize, GakuPluginSummaryV1* output)
        {
            if (!output) return GAKU_E_INVALID_ARGUMENT;
            if (outputSize < sizeof(GakuPluginSummaryV1)) return GAKU_E_SIZE;
            return GAKU_E_INVALID_ARGUMENT;
        }

        static GakuStatus QueryService(void* ctx, const char* name,
            uint32_t requestedVersion, uint32_t outputSize, void* output)
        {
            auto* h = FromCtx(ctx);
            if (!h || !name || !output) return GAKU_E_INVALID_ARGUMENT;
            if (requestedVersion != GAKU_SERVICE_VERSION) return GAKU_E_VERSION;

            if (std::strcmp(name, GAKU_SERVICE_LOG) == 0)
            {
                if (outputSize < sizeof(GakuLogService)) return GAKU_E_SIZE;
                GakuLogService tbl{GAKU_SERVICE_VERSION, sizeof(GakuLogService), h, &LogWrite};
                std::memcpy(output, &tbl, sizeof(tbl));
                return GAKU_OK;
            }
            if (std::strcmp(name, GAKU_SERVICE_RUNTIME) == 0)
            {
                if (outputSize < sizeof(GakuRuntimeService)) return GAKU_E_SIZE;
                static std::wstring gRoot, lRoot, pRoot;
                gRoot = h->localRoot.wstring();
                lRoot = h->localRoot.wstring();
                pRoot = h->localRoot.wstring();
                GakuRuntimeService tbl{GAKU_SERVICE_VERSION, sizeof(GakuRuntimeService),
                    gRoot.c_str(), lRoot.c_str(), pRoot.c_str(), &GetGameAssembly};
                std::memcpy(output, &tbl, sizeof(tbl));
                return GAKU_OK;
            }
            if (std::strcmp(name, GAKU_SERVICE_MAIN_THREAD) == 0)
            {
                if (outputSize < sizeof(GakuMainThreadService)) return GAKU_E_SIZE;
                GakuMainThreadService tbl{GAKU_SERVICE_VERSION, sizeof(GakuMainThreadService),
                    h, &EnqueueTask, &IsMainThread};
                std::memcpy(output, &tbl, sizeof(tbl));
                return GAKU_OK;
            }
            if (std::strcmp(name, GAKU_SERVICE_APPEARANCE) == 0)
            {
                if (outputSize < sizeof(GakuAppearanceService)) return GAKU_E_SIZE;
                GakuAppearanceService tbl{GAKU_SERVICE_VERSION, sizeof(GakuAppearanceService), &GetAppearance};
                std::memcpy(output, &tbl, sizeof(tbl));
                return GAKU_OK;
            }
            if (std::strcmp(name, GAKU_SERVICE_HOTKEYS) == 0)
            {
                if (outputSize < sizeof(GakuHotkeyService)) return GAKU_E_SIZE;
                GakuHotkeyService tbl{GAKU_SERVICE_VERSION, sizeof(GakuHotkeyService), h,
                    &RegisterHotkey, &UnregisterHotkey};
                std::memcpy(output, &tbl, sizeof(tbl));
                return GAKU_OK;
            }
            if (std::strcmp(name, GAKU_SERVICE_PATHS) == 0)
            {
                if (outputSize < sizeof(GakuPathsV1)) return GAKU_E_SIZE;
                GakuPathsV1 tbl{GAKU_PATHS_VERSION, sizeof(GakuPathsV1), h, &PathsGetRoot, &PathsEnsureRoot};
                std::memcpy(output, &tbl, sizeof(tbl));
                return GAKU_OK;
            }
            if (std::strcmp(name, GAKU_SERVICE_PLUGINS) == 0 && h->exposePluginsService)
            {
                if (outputSize < sizeof(GakuPluginsServiceV1)) return GAKU_E_SIZE;
                GakuPluginsServiceV1 tbl{GAKU_PLUGINS_SERVICE_VERSION, sizeof(GakuPluginsServiceV1),
                    &PluginsGetHostInfo, &PluginsFindPlugin, &PluginsGetPluginAt};
                std::memcpy(output, &tbl, sizeof(tbl));
                return GAKU_OK;
            }
            if (std::strcmp(name, GAKU_SERVICE_MOTION_STAGE_STATE) == 0)
            {
                ++h->stageStateQueryCount;
                if (outputSize < sizeof(GakuMotionStageStateServiceV1)) return GAKU_E_SIZE;
                GakuMotionStageStateServiceV1 tbl{GAKU_MOTION_STAGE_STATE_VERSION,
                    sizeof(GakuMotionStageStateServiceV1), h, &GetMotionStageState};
                std::memcpy(output, &tbl, sizeof(tbl));
                return GAKU_OK;
            }

            const auto it = h->services.find(name);
            if (it == h->services.end()) return GAKU_E_UNAVAILABLE;
            if (outputSize < it->second.size()) return GAKU_E_SIZE;
            std::memcpy(output, it->second.data(), it->second.size());
            return GAKU_OK;
        }

        static GakuStatus RegisterService(void* ctx, const char* name,
            uint32_t tableSize, const void* table)
        {
            auto* h = FromCtx(ctx);
            if (!h || !name || !table || tableSize == 0) return GAKU_E_INVALID_ARGUMENT;
            const auto* bytes = static_cast<const uint8_t*>(table);
            h->services[name] = std::vector<uint8_t>(bytes, bytes + tableSize);
            return GAKU_OK;
        }

        GakuHostApi MakeApi()
        {
            return GakuHostApi{GAKU_ABI_VERSION, sizeof(GakuHostApi), this, &QueryService, &RegisterService};
        }
    };

    // ---- Test 1: AlphaMatte ------------------------------------------------

    void TestAlphaMatte()
    {
        BeginTest("AlphaMatte ACES Calibration & Straight Recovery");
        using namespace ScreenshotManager::AlphaMatte;

        constexpr uint32_t W = 8;
        constexpr uint32_t H = 4;
        std::vector<uint8_t> black(W * H * 4, 0);
        std::vector<uint8_t> white(W * H * 4, 0);
        std::vector<uint8_t> out(W * H * 4, 0);

        auto setPx = [&](std::vector<uint8_t>& buf, uint32_t x, uint32_t y,
            uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
            const size_t i = (static_cast<size_t>(y) * W + x) * 4u;
            buf[i + 0] = r;
            buf[i + 1] = g;
            buf[i + 2] = b;
            buf[i + 3] = a;
        };

        // ACES tonemapped white ceiling: white clear = (224, 224, 224), black clear = (0, 0, 0).
        constexpr uint8_t kAcesWhite = 224;
        for (uint32_t y = 0; y < H; ++y)
        {
            for (uint32_t x = 0; x < W; ++x)
            {
                setPx(black, x, y, 0, 0, 0, 255);
                setPx(white, x, y, kAcesWhite, kAcesWhite, kAcesWhite, 255);
            }
        }

        // Pixel (1, 1): Opaque bright subject (210, 80, 40)
        setPx(black, 1, 1, 210, 80, 40);
        setPx(white, 1, 1, 210, 80, 40);

        // Pixel (2, 1): Opaque pure black hair (0, 0, 0) -> must stay A=255, not become transparent!
        setPx(black, 2, 1, 0, 0, 0);
        setPx(white, 2, 1, 0, 0, 0);

        // Pixel (3, 1): Semi-transparent hair edge alpha=0.5, straight color (220, 140, 80)
        // B = 0.5 * (220, 140, 80) = (110, 70, 40)
        // W = B + 0.5 * (224, 224, 224) = (222, 182, 152)
        setPx(black, 3, 1, 110, 70, 40);
        setPx(white, 3, 1, 222, 182, 152);

        // Pixel (4, 1): Soft bloom edge alpha=0.25, straight color (240, 200, 120)
        // B = 0.25 * (240, 200, 120) = (60, 50, 30)
        // W = B + 0.75 * (224, 224, 224) = (228, 218, 198)
        setPx(black, 4, 1, 60, 50, 30);
        setPx(white, 4, 1, 228, 218, 198);

        // Pixel (5, 1): Near-zero alpha (< 2/255) -> snaps to A=0
        setPx(black, 5, 1, 0, 0, 0);
        setPx(white, 5, 1, 223, 223, 223);

        // Pixel (6, 1): Near-one alpha (> 253/255) -> snaps to A=255
        setPx(black, 6, 1, 180, 90, 45);
        setPx(white, 6, 1, 181, 91, 46);

        Check(ExtractAlphaDifferenceMatte(W, H, black.data(), white.data(), out.data(), false),
            "ExtractAlphaDifferenceMatte succeeded");

        auto px = [&](uint32_t x, uint32_t y) -> const uint8_t* {
            return &out[(static_cast<size_t>(y) * W + x) * 4u];
        };

        // Background (0, 0)
        Check(px(0, 0)[0] == 0 && px(0, 0)[1] == 0 && px(0, 0)[2] == 0 && px(0, 0)[3] == 0,
            "ACES background <255 yields exact A=0 and clean black RGB");

        // Opaque subject (1, 1)
        Check(px(1, 1)[0] == 210 && px(1, 1)[1] == 80 && px(1, 1)[2] == 40 && px(1, 1)[3] == 255,
            "Opaque colored pixel retains exact RGB and A=255");

        // Opaque pure black subject (2, 1)
        Check(px(2, 1)[0] == 0 && px(2, 1)[1] == 0 && px(2, 1)[2] == 0 && px(2, 1)[3] == 255,
            "Opaque pure black pixel stays A=255");

        // Semi-transparent hair edge (3, 1)
        Check(NearByte(px(3, 1)[3], 128, 1) &&
            NearByte(px(3, 1)[0], 220, 2) && NearByte(px(3, 1)[1], 140, 2) && NearByte(px(3, 1)[2], 80, 2),
            "50% hair edge recovers straight RGB (220,140,80) and A~128 without halo");

        // Soft bloom edge (4, 1)
        Check(NearByte(px(4, 1)[3], 64, 1) &&
            NearByte(px(4, 1)[0], 240, 3) && NearByte(px(4, 1)[1], 200, 3) && NearByte(px(4, 1)[2], 120, 3),
            "25% bloom edge recovers straight RGB (240,200,120) and A~64");

        // Snap thresholds
        Check(px(5, 1)[3] == 0, "Sub-threshold alpha <2/255 snaps to 0");
        Check(px(6, 1)[3] == 255, "Near-opaque alpha >253/255 snaps to 255");

        // Flip vertical check (row 1 -> row H - 1 - 1 = 2)
        std::vector<uint8_t> flipped(W * H * 4, 0);
        Check(ExtractAlphaDifferenceMatte(W, H, black.data(), white.data(), flipped.data(), true),
            "ExtractAlphaDifferenceMatte flipVertical=true succeeded");
        const uint8_t* fpx = &flipped[(static_cast<size_t>(2) * W + 1) * 4u];
        Check(fpx[0] == 210 && fpx[1] == 80 && fpx[2] == 40 && fpx[3] == 255,
            "flipVertical=true maps row 1 to row 2");

        // Flip horizontal check (col 1 -> col W - 1 - 1 = 6)
        std::vector<uint8_t> hFlipped(W * H * 4, 0);
        Check(ExtractAlphaDifferenceMatte(W, H, black.data(), white.data(), hFlipped.data(), false, true),
            "ExtractAlphaDifferenceMatte flipHorizontal=true succeeded");
        const uint8_t* hpx = &hFlipped[(static_cast<size_t>(1) * W + 6) * 4u];
        Check(hpx[0] == 210 && hpx[1] == 80 && hpx[2] == 40 && hpx[3] == 255,
            "flipHorizontal=true maps (1,1) to (6,1)");

        // 180-degree dual-axis flip (flipVertical=true, flipHorizontal=true): (1,1) -> (6,2), (3,1) -> (4,2)
        std::vector<uint8_t> rot180(W * H * 4, 0);
        Check(ExtractAlphaDifferenceMatte(W, H, black.data(), white.data(), rot180.data(), true, true),
            "ExtractAlphaDifferenceMatte dual-axis 180 flip succeeded");
        const uint8_t* rpx1 = &rot180[(static_cast<size_t>(2) * W + 6) * 4u];
        const uint8_t* rpx3 = &rot180[(static_cast<size_t>(2) * W + 4) * 4u];
        const uint8_t* rbg = &rot180[(static_cast<size_t>(0) * W + 0) * 4u];
        Check(rpx1[0] == 210 && rpx1[1] == 80 && rpx1[2] == 40 && rpx1[3] == 255,
            "180-degree flip maps opaque (1,1) to (6,2)");
        Check(NearByte(rpx3[3], 128, 1) && NearByte(rpx3[0], 220, 2) &&
            NearByte(rpx3[1], 140, 2) && NearByte(rpx3[2], 80, 2),
            "180-degree flip maps semi-transparent (3,1) to (4,2) with straight RGB");
        Check(rbg[0] == 0 && rbg[1] == 0 && rbg[2] == 0 && rbg[3] == 0,
            "180-degree flip preserves transparent background");

        // CopyOpaqueToRgba vertical-only flip (flipVertical=true, flipHorizontal=false):
        // source bottom-left (0, H-1) -> output top-left (0, 0), NOT (W-1, 0).
        std::vector<uint8_t> opaqueSrc = {
            10, 20, 30, 0,   40, 50, 60, 100,  70, 80, 90, 200,
            110, 120, 130, 50,  140, 150, 160, 150,  170, 180, 190, 255
        };
        std::vector<uint8_t> opaqueVertOut(3 * 2 * 4, 0);
        Check(CopyOpaqueToRgba(3, 2, opaqueSrc.data(), true, opaqueVertOut.data(), false),
            "CopyOpaqueToRgba vertical-only flip succeeded");
        Check(opaqueVertOut[0] == 110 && opaqueVertOut[1] == 120 && opaqueVertOut[2] == 130 && opaqueVertOut[3] == 255 &&
            opaqueVertOut[2 * 4 + 0] == 170 && opaqueVertOut[2 * 4 + 1] == 180 && opaqueVertOut[2 * 4 + 2] == 190 &&
            opaqueVertOut[(1 * 3 + 0) * 4 + 0] == 10 && opaqueVertOut[(1 * 3 + 2) * 4 + 0] == 70,
            "CopyOpaqueToRgba vertical-only flip maps bottom-left (0,1) to top-left (0,0) without horizontal mirror");

        // CopyOpaqueToRgba 180-degree flip check
        std::vector<uint8_t> opaqueOut(3 * 2 * 4, 0);
        Check(CopyOpaqueToRgba(3, 2, opaqueSrc.data(), true, opaqueOut.data(), true),
            "CopyOpaqueToRgba dual-axis 180 flip succeeded");
        Check(opaqueOut[0] == 170 && opaqueOut[1] == 180 && opaqueOut[2] == 190 && opaqueOut[3] == 255 &&
            opaqueOut[(1 * 3 + 2) * 4 + 0] == 10 && opaqueOut[(1 * 3 + 2) * 4 + 1] == 20 &&
            opaqueOut[(1 * 3 + 2) * 4 + 2] == 30 && opaqueOut[(1 * 3 + 2) * 4 + 3] == 255,
            "CopyOpaqueToRgba 180 flip swaps corners and forces A=255");

        // Floor ceilDelta >= 16/255 safety test
        std::vector<uint8_t> lowBlack(4 * 4, 0);
        std::vector<uint8_t> lowWhite(4 * 4, 0);
        lowWhite[0] = lowWhite[1] = lowWhite[2] = 10;
        lowWhite[3] = 255;
        std::vector<uint8_t> lowOut(4 * 4, 0);
        Check(ExtractAlphaDifferenceMatte(2, 2, lowBlack.data(), lowWhite.data(), lowOut.data(), false),
            "Floor ceilDelta avoids divide-by-zero");
        Check(lowOut[3] > 0 && lowOut[3] < 255, "10/255 delta against 16/255 floor gives partial alpha");

        // UnpremultiplyAndConvertToRgba (including 180-degree flip) & RgbaToBgra
        const uint8_t premul[16] = {
            100, 50, 25, 128,   0, 0, 0, 0,
            10, 20, 30, 255,    40, 80, 120, 255
        };
        uint8_t unpremul[16]{};
        Check(UnpremultiplyAndConvertToRgba(2, 2, premul, false, unpremul),
            "UnpremultiplyAndConvertToRgba succeeded");
        Check(NearByte(unpremul[0], 199, 1) && NearByte(unpremul[1], 100, 1) && NearByte(unpremul[2], 50, 1) && unpremul[3] == 128,
            "Unpremultiply recovers straight RGB from premultiplied pixel");
        Check(unpremul[4] == 0 && unpremul[5] == 0 && unpremul[6] == 0 && unpremul[7] == 0,
            "Unpremultiply clears A=0 RGB to zero");

        uint8_t unpremul180[16]{};
        Check(UnpremultiplyAndConvertToRgba(2, 2, premul, true, unpremul180, true),
            "UnpremultiplyAndConvertToRgba 180 flip succeeded");
        Check(unpremul180[0] == 40 && unpremul180[1] == 80 && unpremul180[2] == 120 && unpremul180[3] == 255 &&
            NearByte(unpremul180[12], 199, 1) && NearByte(unpremul180[13], 100, 1) &&
            NearByte(unpremul180[14], 50, 1) && unpremul180[15] == 128,
            "Unpremultiply 180 flip swaps corners while recovering straight RGB");

        uint8_t bgra[16]{};
        Check(RgbaToBgra(2, 2, unpremul, bgra), "RgbaToBgra succeeded");
        Check(bgra[0] == unpremul[2] && bgra[1] == unpremul[1] && bgra[2] == unpremul[0] && bgra[3] == unpremul[3],
            "RgbaToBgra swaps R and B");
    }

    // ---- Test 1b: DXGI RowPitch Unpack & Format Parity ---------------------

    void TestDxgiRowPitchUnpack()
    {
        BeginTest("DXGI RowPitch Unpack & RGBA/BGRA/FP16 Parity");
        using ScreenshotManager::ScreenshotEngine;

        constexpr uint32_t W = 3;
        constexpr uint32_t H = 2;

        // 3 pixels * 4 bytes = 12 bytes/row, padded to 24 bytes RowPitch with 0xAA sentinel bytes.
        constexpr size_t kPitch4 = 24;
        std::vector<uint8_t> rgbaSrc(kPitch4 * H, 0xAA);
        std::vector<uint8_t> bgraSrc(kPitch4 * H, 0xAA);
        for (uint32_t y = 0; y < H; ++y)
        {
            for (uint32_t x = 0; x < W; ++x)
            {
                const uint8_t r = static_cast<uint8_t>(20 + y * 60 + x * 15);
                const uint8_t g = static_cast<uint8_t>(40 + y * 50 + x * 10);
                const uint8_t b = static_cast<uint8_t>(80 + y * 40 + x * 20);
                const uint8_t a = static_cast<uint8_t>(200 + x * 10);
                const size_t s = static_cast<size_t>(y) * kPitch4 + static_cast<size_t>(x) * 4u;
                rgbaSrc[s + 0] = r; rgbaSrc[s + 1] = g; rgbaSrc[s + 2] = b; rgbaSrc[s + 3] = a;
                bgraSrc[s + 0] = b; bgraSrc[s + 1] = g; bgraSrc[s + 2] = r; bgraSrc[s + 3] = a;
            }
        }

        std::vector<uint8_t> outRgba;
        std::vector<uint8_t> outFromBgra;
        Check(ScreenshotEngine::UnpackDxgiPixelsToRgba(
            DXGI_FORMAT_R8G8B8A8_UNORM, W, H, rgbaSrc.data(), kPitch4, outRgba),
            "UnpackDxgiPixelsToRgba R8G8B8A8_UNORM with padded RowPitch succeeded");
        Check(ScreenshotEngine::UnpackDxgiPixelsToRgba(
            DXGI_FORMAT_B8G8R8A8_UNORM, W, H, bgraSrc.data(), kPitch4, outFromBgra),
            "UnpackDxgiPixelsToRgba B8G8R8A8_UNORM with padded RowPitch succeeded");
        Check(outRgba.size() == W * H * 4u && outRgba == outFromBgra,
            "BGRA8 and RGBA8 padded RowPitch unpack produce byte-identical RGBA8 buffers");
        Check(outRgba[(1 * W + 2) * 4 + 0] == 110 && outRgba[(1 * W + 2) * 4 + 2] == 160,
            "Padded RowPitch offset skips stride padding bytes accurately");

        // R16G16B16A16_FLOAT (8 bytes/px = 24 bytes/row, padded to 32 bytes RowPitch)
        constexpr size_t kPitch8 = 32;
        std::vector<uint8_t> fp16Src(kPitch8 * H, 0x55);
        auto setFp16Px = [&](uint32_t x, uint32_t y, uint16_t hr, uint16_t hg, uint16_t hb, uint16_t ha) {
            uint16_t* p = reinterpret_cast<uint16_t*>(fp16Src.data() + static_cast<size_t>(y) * kPitch8 + static_cast<size_t>(x) * 8u);
            p[0] = hr; p[1] = hg; p[2] = hb; p[3] = ha;
        };
        // 0x3C00 = 1.0, 0x0000 = 0.0, 0x3800 = 0.5 (linear 0.5 -> sRGB ~0.735 -> ~187)
        setFp16Px(0, 0, 0x3C00, 0x0000, 0x3800, 0x3C00);
        setFp16Px(2, 1, 0x0000, 0x3C00, 0x0000, 0x3800);
        std::vector<uint8_t> outFp16;
        Check(ScreenshotEngine::UnpackDxgiPixelsToRgba(
            DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, fp16Src.data(), kPitch8, outFp16),
            "UnpackDxgiPixelsToRgba R16G16B16A16_FLOAT with padded RowPitch succeeded");
        Check(outFp16[0] == 255 && outFp16[1] == 0 && NearByte(outFp16[2], 188, 2) && outFp16[3] == 255,
            "FP16 row 0 pixel (0,0) linear-to-sRGB conversion accurate");
        const size_t fp16Corner = (1 * W + 2) * 4u;
        Check(outFp16[fp16Corner + 0] == 0 && outFp16[fp16Corner + 1] == 255 &&
            outFp16[fp16Corner + 2] == 0 && NearByte(outFp16[fp16Corner + 3], 128, 1),
            "FP16 row 1 pixel (2,1) skips stride padding accurately");

        std::vector<uint8_t> tooSmall;
        Check(!ScreenshotEngine::UnpackDxgiPixelsToRgba(
            DXGI_FORMAT_R8G8B8A8_UNORM, W, H, rgbaSrc.data(), 8, tooSmall),
            "Rejects undersized RowPitch < width * bpp");
    }

    // ---- Test 2: WicImageWriter 4K PNG & JPEG --------------------------------

    void TestWicImageWriter()
    {
        BeginTest("WicImageWriter 4K 3840x2160 RGBA PNG & JPEG");
        using namespace ScreenshotManager::WicWriter;

        const std::filesystem::path dir = MakeTempTestDir(L"WIC");
        constexpr uint32_t W = 3840;
        constexpr uint32_t H = 2160;
        std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4u, 0);

        // Top-left quadrant: transparent A=0
        // Top-right quadrant: semi-transparent A=128 (straight RGB 200, 120, 60)
        // Bottom half: opaque A=255 (RGB 40, 160, 220)
        for (uint32_t y = 0; y < H; ++y)
        {
            for (uint32_t x = 0; x < W; ++x)
            {
                const size_t idx = (static_cast<size_t>(y) * W + x) * 4u;
                if (y < H / 2)
                {
                    if (x < W / 2)
                    {
                        rgba[idx + 0] = 0;
                        rgba[idx + 1] = 0;
                        rgba[idx + 2] = 0;
                        rgba[idx + 3] = 0;
                    }
                    else
                    {
                        rgba[idx + 0] = 200;
                        rgba[idx + 1] = 120;
                        rgba[idx + 2] = 60;
                        rgba[idx + 3] = 128;
                    }
                }
                else
                {
                    rgba[idx + 0] = 40;
                    rgba[idx + 1] = 160;
                    rgba[idx + 2] = 220;
                    rgba[idx + 3] = 255;
                }
            }
        }

        const std::filesystem::path png4kPath = dir / L"test_4k_rgba.png";
        std::string err;
        Check(WritePngRgba(png4kPath, W, H, rgba.data(), true, &err), "WritePngRgba 4K RGBA succeeded: " + err);

        DecodedImage decoded;
        Check(ReadImageForTest(png4kPath, decoded, &err), "ReadImageForTest 4K PNG succeeded: " + err);
        Check(decoded.width == W && decoded.height == H, "Decoded 4K PNG matches 3840x2160");
        Check(decoded.hasAlphaChannel, "Decoded 4K PNG has alpha channel");
        Check(IsEqualGUID(decoded.nativePixelFormat, GUID_WICPixelFormat32bppBGRA) != FALSE,
            "4K RGBA PNG stored as 32bppBGRA");

        auto dpx = [&](uint32_t x, uint32_t y) -> const uint8_t* {
            return &decoded.rgbaTopDown[(static_cast<size_t>(y) * W + x) * 4u];
        };
        Check(dpx(100, 100)[3] == 0, "Top-left quadrant preserves exact A=0");
        Check(dpx(3000, 100)[0] == 200 && dpx(3000, 100)[1] == 120 &&
            dpx(3000, 100)[2] == 60 && dpx(3000, 100)[3] == 128,
            "Top-right quadrant preserves exact RGB(200,120,60) and A=128");
        Check(dpx(1920, 1500)[0] == 40 && dpx(1920, 1500)[1] == 160 &&
            dpx(1920, 1500)[2] == 220 && dpx(1920, 1500)[3] == 255,
            "Bottom half preserves exact RGB(40,160,220) and A=255");

        // Opaque PNG -> 24bppBGR
        const std::filesystem::path pngOpaquePath = dir / L"test_opaque.png";
        Check(WritePngRgba(pngOpaquePath, 640, 360, rgba.data(), false, &err), "WritePngRgba opaque succeeded");
        DecodedImage decodedOpaque;
        Check(ReadImageForTest(pngOpaquePath, decodedOpaque, &err), "ReadImageForTest opaque PNG succeeded");
        Check(!decodedOpaque.hasAlphaChannel &&
            IsEqualGUID(decodedOpaque.nativePixelFormat, GUID_WICPixelFormat24bppBGR) != FALSE,
            "Opaque PNG stored as 24bppBGR");

        // JPEG write & decode
        const std::filesystem::path jpgPath = dir / L"test_quality.jpg";
        std::vector<uint8_t> jpgSrc(640 * 360 * 4u, 0);
        for (size_t i = 0; i < 640 * 360; ++i)
        {
            jpgSrc[i * 4 + 0] = 180;
            jpgSrc[i * 4 + 1] = 110;
            jpgSrc[i * 4 + 2] = 70;
            jpgSrc[i * 4 + 3] = 255;
        }
        Check(WriteJpegRgba(jpgPath, 640, 360, jpgSrc.data(), 90, &err), "WriteJpegRgba succeeded: " + err);
        DecodedImage decodedJpg;
        Check(ReadImageForTest(jpgPath, decodedJpg, &err), "ReadImageForTest JPEG succeeded");
        Check(decodedJpg.width == 640 && decodedJpg.height == 360 && !decodedJpg.hasAlphaChannel,
            "Decoded JPEG matches 640x360 opaque");

        // Ensure no leftover .tmp.* files
        size_t tmpCount = 0;
        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (entry.path().wstring().find(L".tmp.") != std::wstring::npos) ++tmpCount;
        }
        Check(tmpCount == 0, "No leftover temp files after atomic WIC writes");

        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // ---- Test 3: ConfigStore atomic persistence ----------------------------

    void TestConfigStore()
    {
        BeginTest("ScreenshotConfig Atomic Save & Load");
        const std::filesystem::path dir = MakeTempTestDir(L"Cfg");
        const std::filesystem::path cfgFile = dir / L"config.json";
        const std::string defaultOut = ScreenshotManager::PathToUtf8(dir / L"DefaultShots");

        ScreenshotManager::ConfigStore store;
        store.Configure(cfgFile, defaultOut);

        ScreenshotManager::ScreenshotConfig next;
        next.width = 2560;
        next.height = 1440;
        next.imageFormat = GAKU_IMAGE_JPEG;
        next.jpegQuality = 88;
        next.alphaMode = GAKU_SCREENSHOT_ALPHA_FORCE_TRANSPARENT;
        next.outputDirUtf8 = ScreenshotManager::PathToUtf8(dir / L"截图输出");
        Check(store.Update(next, true), "SaveConfigAtomic succeeded");
        Check(std::filesystem::is_regular_file(cfgFile), "config.json exists");

        ScreenshotManager::ConfigStore reloaded;
        reloaded.Configure(cfgFile, defaultOut);
        const auto got = reloaded.Get();
        Check(got.width == 2560 && got.height == 1440, "Reloaded width/height match");
        Check(got.imageFormat == GAKU_IMAGE_JPEG && got.jpegQuality == 88, "Reloaded JPEG settings match");
        Check(got.alphaMode == GAKU_SCREENSHOT_ALPHA_FORCE_TRANSPARENT, "Reloaded alphaMode matches");
        Check(got.outputDirUtf8 == next.outputDirUtf8, "Reloaded UTF-8 CJK outputDir matches");

        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // ---- Test 4: ScreenshotEngine & GakuScreenshotServiceV1 ----------------

    void TestEngineAndCaptureService()
    {
        BeginTest("ScreenshotEngine & GakuScreenshotServiceV1 ABI");
        const std::filesystem::path dir = MakeTempTestDir(L"Engine");

        FakeHost fakeHost;
        fakeHost.localRoot = dir;
        fakeHost.configRoot = dir / L"Configs";
        fakeHost.userDataRoot = dir / L"UserData";
        fakeHost.greenScreenActive = 1u;
        const GakuHostApi api = fakeHost.MakeApi();

        ScreenshotManager::ScreenshotEngine engine;
        engine.SetHostApi(&api);
        engine.SetTestUnityThreadBypass(true);
        engine.InitializeWorker();
        engine.SetTestRenderHook(
            [](uint32_t w, uint32_t h, bool dualPass,
               std::vector<uint8_t>& outB, std::vector<uint8_t>& outW,
               ScreenshotManager::PixelOrientation& outOrient, std::string&) -> bool {
                const size_t n = static_cast<size_t>(w) * h * 4u;
                outB.assign(n, 0);
                outOrient = {false, false};
                if (dualPass)
                {
                    outW.assign(n, 0);
                    for (uint32_t y = 0; y < h; ++y)
                    {
                        for (uint32_t x = 0; x < w; ++x)
                        {
                            const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
                            const bool inside = (x >= w / 4 && x < (3 * w) / 4 && y >= h / 4 && y < (3 * h) / 4);
                            if (inside)
                            {
                                outB[i + 0] = 180; outB[i + 1] = 60; outB[i + 2] = 90; outB[i + 3] = 255;
                                outW[i + 0] = 180; outW[i + 1] = 60; outW[i + 2] = 90; outW[i + 3] = 255;
                            }
                            else
                            {
                                outB[i + 0] = 0; outB[i + 1] = 0; outB[i + 2] = 0; outB[i + 3] = 255;
                                outW[i + 0] = 220; outW[i + 1] = 220; outW[i + 2] = 220; outW[i + 3] = 255;
                            }
                        }
                    }
                }
                else
                {
                    for (size_t i = 0; i < n; i += 4)
                    {
                        outB[i + 0] = 25; outB[i + 1] = 140; outB[i + 2] = 210; outB[i + 3] = 255;
                    }
                }
                return true;
            },
            1280u, 720u);

        GakuScreenshotCapabilitiesV1 caps{};
        Check(engine.GetCapabilities(&caps) == GAKU_OK, "GetCapabilities returns GAKU_OK");
        Check(caps.abiVersion == GAKU_SCREENSHOT_VERSION && caps.structSize == sizeof(caps),
            "Capabilities ABI fields valid");
        Check(caps.runtimeReady == 1u && caps.cameraAvailable == 1u && caps.greenScreenDetected == 1u,
            "Lazy gakumotion.stage-state/v1 detects greenScreenActive=1");
        Check(caps.maxDimension == 7680u && caps.windowWidth == 1280u && caps.windowHeight == 720u,
            "Capabilities maxDimension=7680 and window dimensions match");

        fakeHost.greenScreenActive = 0u;
        Check(engine.GetCapabilities(&caps) == GAKU_OK && caps.greenScreenDetected == 0u,
            "Capabilities tracks greenScreenActive=0 dynamically");
        fakeHost.greenScreenActive = 1u;

        // Async file capture (PNG with green screen alpha)
        const std::filesystem::path outPng = dir / L"async_green.png";
        const std::string outPngUtf8 = ScreenshotManager::PathToUtf8(outPng);
        GakuScreenshotFileRequestV1 fileReq{};
        fileReq.abiVersion = GAKU_SCREENSHOT_VERSION;
        fileReq.structSize = sizeof(fileReq);
        fileReq.width = 64;
        fileReq.height = 64;
        fileReq.imageFormat = GAKU_IMAGE_PNG;
        fileReq.alphaMode = GAKU_SCREENSHOT_ALPHA_AUTO_GREENSCREEN;
        std::memcpy(fileReq.outputPathUtf8, outPngUtf8.c_str(), outPngUtf8.size() + 1);

        uint64_t ticket = 0;
        Check(engine.RequestFileCapture(&fileReq, &ticket, dir) == GAKU_OK && ticket > 0,
            "RequestFileCapture accepted ticket");

        GakuScreenshotResultV1 res{};
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            if (engine.PollCapture(ticket, &res) == GAKU_OK && res.state != GAKU_CAPTURE_PENDING) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(res.state == GAKU_CAPTURE_SUCCEEDED && res.width == 64 && res.height == 64 && res.hasAlpha == 1u,
            "Async capture succeeded with 64x64 RGBA");

        ScreenshotManager::WicWriter::DecodedImage decoded;
        std::string decErr;
        Check(ScreenshotManager::WicWriter::ReadImageForTest(outPng, decoded, &decErr),
            "Decoded async captured PNG: " + decErr);
        Check(decoded.rgbaTopDown[3] == 0 && decoded.rgbaTopDown[(32 * 64 + 32) * 4 + 3] == 255,
            "Async PNG has transparent border (A=0) and opaque center (A=255)");
        Check(engine.ReleaseTicket(ticket) == GAKU_OK, "ReleaseTicket succeeded");
        Check(engine.PollCapture(ticket, &res) == GAKU_E_INVALID_ARGUMENT,
            "Released ticket rejects subsequent pollCapture");

        // Immediate frame capture (BGRA8 top-down for VideoExporter)
        GakuScreenshotFrameRequestV1 immReq{};
        immReq.abiVersion = GAKU_SCREENSHOT_VERSION;
        immReq.structSize = sizeof(immReq);
        immReq.width = 32;
        immReq.height = 16;
        immReq.pixelFormat = GAKU_PIXEL_BGRA8_SRGB_TOPDOWN;
        immReq.alphaMode = GAKU_SCREENSHOT_ALPHA_AUTO_GREENSCREEN;

        GakuScreenshotFrameInfoV1 immInfo{};
        char errBuf[256]{};
        std::vector<uint8_t> smallBuf(16);
        Check(engine.CaptureFrameImmediate(&immReq, smallBuf.data(), smallBuf.size(), &immInfo, errBuf, sizeof(errBuf)) == GAKU_E_SIZE,
            "captureFrameImmediate rejects too-small buffer with GAKU_E_SIZE");
        Check(immInfo.width == 32 && immInfo.height == 16 && immInfo.rowPitch == 128,
            "GAKU_E_SIZE populates required frame dimensions and rowPitch");

        std::vector<uint8_t> pixels(32 * 16 * 4u, 0);
        Check(engine.CaptureFrameImmediate(&immReq, pixels.data(), pixels.size(), &immInfo, errBuf, sizeof(errBuf)) == GAKU_OK,
            "captureFrameImmediate BGRA8 succeeded");
        Check(immInfo.hasAlpha == 1u && immInfo.width == 32 && immInfo.height == 16,
            "captureFrameImmediate sets hasAlpha=1");
        // Center pixel (16, 8) RGB=(180,60,90) -> BGRA=(90,60,180,255)
        const uint8_t* center = &pixels[(8 * 32 + 16) * 4u];
        Check(center[0] == 90 && center[1] == 60 && center[2] == 180 && center[3] == 255,
            "captureFrameImmediate outputs BGRA8 byte order");
        Check(pixels[3] == 0, "captureFrameImmediate border pixel has A=0");

        // Oversized > 7680 rejected
        immReq.width = 8192;
        Check(engine.CaptureFrameImmediate(&immReq, pixels.data(), pixels.size(), &immInfo, errBuf, sizeof(errBuf)) == GAKU_E_INVALID_ARGUMENT,
            "captureFrameImmediate rejects width > 7680");

        // 180-degree rotated raw buffers (flipHorizontal=true, flipVertical=true)
        // in both opaque and dual-pass alpha modes, verifying PNG, RGBA8, and BGRA8 parity.
        engine.SetTestRenderHook(
            [](uint32_t w, uint32_t h, bool dualPass,
               std::vector<uint8_t>& outB, std::vector<uint8_t>& outW,
               ScreenshotManager::PixelOrientation& outOrient, std::string&) -> bool {
                const size_t n = static_cast<size_t>(w) * h * 4u;
                outB.assign(n, 0);
                outOrient = {true, true};
                if (dualPass) outW.assign(n, 0);

                // Define upright scene pixel at (ux, uy), then store at 180-rotated (w-1-ux, h-1-uy).
                for (uint32_t uy = 0; uy < h; ++uy)
                {
                    for (uint32_t ux = 0; ux < w; ++ux)
                    {
                        const uint32_t rx = w - 1u - ux;
                        const uint32_t ry = h - 1u - uy;
                        const size_t ri = (static_cast<size_t>(ry) * w + rx) * 4u;

                        const bool topHalf = (uy < h / 2u);
                        const bool leftHalf = (ux < w / 2u);
                        if (!dualPass)
                        {
                            // Four distinct opaque quadrants in upright space:
                            // TL=(230,20,30), TR=(20,210,40), BL=(30,50,235), BR=(240,200,20)
                            if (topHalf && leftHalf) { outB[ri+0]=230; outB[ri+1]=20; outB[ri+2]=30; outB[ri+3]=255; }
                            else if (topHalf)        { outB[ri+0]=20;  outB[ri+1]=210; outB[ri+2]=40; outB[ri+3]=255; }
                            else if (leftHalf)       { outB[ri+0]=30;  outB[ri+1]=50; outB[ri+2]=235; outB[ri+3]=255; }
                            else                     { outB[ri+0]=240; outB[ri+1]=200; outB[ri+2]=20; outB[ri+3]=255; }
                        }
                        else
                        {
                            // Upright TL=(230,20,30,A=255), TR=(transparent), BL=(30,50,235,A=255), BR=(200,120,60,A~128)
                            if (topHalf && leftHalf)
                            {
                                outB[ri+0]=230; outB[ri+1]=20; outB[ri+2]=30; outB[ri+3]=255;
                                outW[ri+0]=230; outW[ri+1]=20; outW[ri+2]=30; outW[ri+3]=255;
                            }
                            else if (topHalf)
                            {
                                outB[ri+0]=0; outB[ri+1]=0; outB[ri+2]=0; outB[ri+3]=255;
                                outW[ri+0]=224; outW[ri+1]=224; outW[ri+2]=224; outW[ri+3]=255;
                            }
                            else if (leftHalf)
                            {
                                outB[ri+0]=30; outB[ri+1]=50; outB[ri+2]=235; outB[ri+3]=255;
                                outW[ri+0]=30; outW[ri+1]=50; outW[ri+2]=235; outW[ri+3]=255;
                            }
                            else
                            {
                                // straight (200,120,60) at alpha=0.5 over ACES white 224 -> B=(100,60,30), W=(212,172,142)
                                outB[ri+0]=100; outB[ri+1]=60; outB[ri+2]=30; outB[ri+3]=255;
                                outW[ri+0]=212; outW[ri+1]=172; outW[ri+2]=142; outW[ri+3]=255;
                            }
                        }
                    }
                }
                return true;
            },
            1280u, 720u);

        auto verifyOrientedMode = [&](uint32_t alphaMode, const std::wstring& fileName, bool expectAlpha) {
            constexpr uint32_t CW = 16;
            constexpr uint32_t CH = 8;
            const std::filesystem::path pngPath = dir / fileName;
            const std::string pngUtf8 = ScreenshotManager::PathToUtf8(pngPath);

            GakuScreenshotFileRequestV1 fReq{};
            fReq.abiVersion = GAKU_SCREENSHOT_VERSION;
            fReq.structSize = sizeof(fReq);
            fReq.width = CW;
            fReq.height = CH;
            fReq.imageFormat = GAKU_IMAGE_PNG;
            fReq.alphaMode = alphaMode;
            std::memcpy(fReq.outputPathUtf8, pngUtf8.c_str(), pngUtf8.size() + 1);

            uint64_t tId = 0;
            Check(engine.RequestFileCapture(&fReq, &tId, dir) == GAKU_OK,
                "180-deg oriented RequestFileCapture accepted");
            GakuScreenshotResultV1 fRes{};
            for (int attempt = 0; attempt < 200; ++attempt)
            {
                if (engine.PollCapture(tId, &fRes) == GAKU_OK && fRes.state != GAKU_CAPTURE_PENDING) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            Check(fRes.state == GAKU_CAPTURE_SUCCEEDED && fRes.hasAlpha == (expectAlpha ? 1u : 0u),
                "180-deg oriented file capture succeeded");
            engine.ReleaseTicket(tId);

            ScreenshotManager::WicWriter::DecodedImage dec;
            std::string dErr;
            Check(ScreenshotManager::WicWriter::ReadImageForTest(pngPath, dec, &dErr),
                "Decoded 180-deg oriented PNG: " + dErr);

            // Immediate RGBA8 & BGRA8 capture
            GakuScreenshotFrameRequestV1 rReq{GAKU_SCREENSHOT_VERSION, sizeof(rReq), CW, CH,
                GAKU_PIXEL_RGBA8_SRGB_TOPDOWN, alphaMode};
            GakuScreenshotFrameRequestV1 bReq{GAKU_SCREENSHOT_VERSION, sizeof(bReq), CW, CH,
                GAKU_PIXEL_BGRA8_SRGB_TOPDOWN, alphaMode};
            GakuScreenshotFrameInfoV1 rInfo{}, bInfo{};
            std::vector<uint8_t> rgbaBuf(CW * CH * 4u, 0);
            std::vector<uint8_t> bgraBuf(CW * CH * 4u, 0);
            Check(engine.CaptureFrameImmediate(&rReq, rgbaBuf.data(), rgbaBuf.size(), &rInfo, errBuf, sizeof(errBuf)) == GAKU_OK,
                "180-deg oriented CaptureFrameImmediate RGBA8 succeeded");
            Check(engine.CaptureFrameImmediate(&bReq, bgraBuf.data(), bgraBuf.size(), &bInfo, errBuf, sizeof(errBuf)) == GAKU_OK,
                "180-deg oriented CaptureFrameImmediate BGRA8 succeeded");
            Check(rInfo.rowPitch == CW * 4u && bInfo.rowPitch == CW * 4u &&
                rInfo.hasAlpha == (expectAlpha ? 1u : 0u) && bInfo.hasAlpha == (expectAlpha ? 1u : 0u),
                "180-deg oriented immediate captures report matching rowPitch and hasAlpha");

            // Byte-for-byte parity between file PNG, immediate RGBA8, and immediate BGRA8 (swapped R<->B)
            bool fileMatchesRgba = (dec.rgbaTopDown == rgbaBuf);
            bool bgraMatchesRgba = true;
            for (size_t i = 0; i < CW * CH; ++i)
            {
                const size_t k = i * 4u;
                if (bgraBuf[k + 0] != rgbaBuf[k + 2] || bgraBuf[k + 1] != rgbaBuf[k + 1] ||
                    bgraBuf[k + 2] != rgbaBuf[k + 0] || bgraBuf[k + 3] != rgbaBuf[k + 3])
                {
                    bgraMatchesRgba = false;
                    break;
                }
            }
            Check(fileMatchesRgba, "File PNG pixels match CaptureFrameImmediate RGBA8");
            Check(bgraMatchesRgba, "CaptureFrameImmediate BGRA8 matches RGBA8 with R<->B swap");

            auto px = [&](uint32_t x, uint32_t y) -> const uint8_t* {
                return &rgbaBuf[(static_cast<size_t>(y) * CW + x) * 4u];
            };
            Check(px(1, 1)[0] == 230 && px(1, 1)[1] == 20 && px(1, 1)[2] == 30 && px(1, 1)[3] == 255,
                "Upright top-left quadrant restored accurately");
            Check(px(1, 6)[0] == 30 && px(1, 6)[1] == 50 && px(1, 6)[2] == 235 && px(1, 6)[3] == 255,
                "Upright bottom-left quadrant restored accurately");
            if (!expectAlpha)
            {
                Check(px(14, 1)[0] == 20 && px(14, 1)[1] == 210 && px(14, 1)[2] == 40 && px(14, 1)[3] == 255,
                    "Opaque upright top-right quadrant restored accurately");
                Check(px(14, 6)[0] == 240 && px(14, 6)[1] == 200 && px(14, 6)[2] == 20 && px(14, 6)[3] == 255,
                    "Opaque upright bottom-right quadrant restored accurately");
            }
            else
            {
                Check(px(14, 1)[0] == 0 && px(14, 1)[1] == 0 && px(14, 1)[2] == 0 && px(14, 1)[3] == 0,
                    "Alpha upright top-right quadrant transparent");
                Check(NearByte(px(14, 6)[3], 128, 1) && NearByte(px(14, 6)[0], 200, 3) &&
                    NearByte(px(14, 6)[1], 120, 3) && NearByte(px(14, 6)[2], 60, 3),
                    "Alpha upright bottom-right semi-transparent quadrant restored accurately");
            }
        };

        verifyOrientedMode(GAKU_SCREENSHOT_ALPHA_OPAQUE, L"oriented_opaque.png", false);
        verifyOrientedMode(GAKU_SCREENSHOT_ALPHA_FORCE_TRANSPARENT, L"oriented_alpha.png", true);

        engine.Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // ---- Test 5: ScreenshotWebPanel Bridge Contract ------------------------

    void TestWebPanelBridge()
    {
        BeginTest("GakuWebPanelServiceV1 Bridge Contract");
        const std::filesystem::path dir = MakeTempTestDir(L"Web");

        ScreenshotManager::ConfigStore cfgStore;
        cfgStore.Configure(dir / L"config.json", ScreenshotManager::PathToUtf8(dir / L"Shots"));

        ScreenshotManager::ScreenshotEngine engine;
        engine.SetTestUnityThreadBypass(true);
        engine.SetTestGreenScreenOverride(1);
        engine.InitializeWorker();
        engine.SetTestRenderHook(
            [](uint32_t w, uint32_t h, bool, std::vector<uint8_t>& b, std::vector<uint8_t>& wBuf,
               ScreenshotManager::PixelOrientation& orient, std::string&) {
                b.assign(static_cast<size_t>(w) * h * 4u, 200);
                wBuf.assign(static_cast<size_t>(w) * h * 4u, 200);
                orient = {false, false};
                return true;
            },
            1920u, 1080u);

        bool folderOpened = false;
        ScreenshotManager::WebPanel::HostHooks hooks;
        hooks.assetsDir = [dir]() { return (dir / L"ScreenshotManager.UI").wstring(); };
        hooks.openFolder = [&](const std::string&, std::string&) {
            folderOpened = true;
            return true;
        };
        hooks.browseFolder = [&](const std::string&) {
            ScreenshotManager::WebPanel::BrowseFolderResult r;
            r.started = true;
            r.pending = false;
            r.cancelled = false;
            r.pathUtf8 = ScreenshotManager::PathToUtf8(dir / L"CustomShots");
            return r;
        };
        hooks.appearance = []() {
            return ScreenshotManager::WebPanel::AppearanceInfo{3, 1.0f};
        };

        ScreenshotManager::WebPanel::Bridge bridge(engine, cfgStore, hooks);
        std::vector<std::string> events;
        bridge.SetEventSink([](void* ud, const char* json) {
            static_cast<std::vector<std::string>*>(ud)->emplace_back(json);
        }, &events);
        bridge.OnShow();

        GakuWebPanelAssetsV1 assets{};
        Check(bridge.GetAssets(&assets) == GAKU_OK, "GetAssets returns GAKU_OK");
        Check(assets.abiVersion == GAKU_WEB_PANEL_VERSION && assets.structSize == sizeof(assets),
            "WebPanel assets ABI valid");
        Check(std::wstring(assets.virtualHost) == L"screenshotmanager.invalid" &&
            std::wstring(assets.entryPath) == L"index.html",
            "virtualHost == screenshotmanager.invalid and entryPath == index.html");

        auto req = [&](const std::string& method, const std::string& params = "{}") {
            const std::string payload = "{\"v\":1,\"id\":1,\"type\":\"request\",\"method\":\"" +
                method + "\",\"params\":" + params + "}";
            GakuMotion::jsonlite::Value v;
            std::string err;
            GakuMotion::jsonlite::ParseText(bridge.HandleMessageForTest(payload), v, err);
            return v;
        };

        const auto hello = req("bridge.hello");
        Check(hello.BoolPath("ok", false) && hello.IntPath("result.protocol", 0) == 1,
            "bridge.hello returns protocol=1");

        const auto setCfg = req("config.set",
            "{\"width\":3840,\"height\":2160,\"imageFormat\":\"png\",\"jpegQuality\":95,\"alphaMode\":\"force_transparent\"}");
        Check(setCfg.BoolPath("ok", false) && setCfg.IntPath("result.config.width", 0) == 3840,
            "config.set persists 4K resolution");

        const auto badCfg = req("config.set", "{\"width\":9999,\"height\":2160}");
        Check(!badCfg.BoolPath("ok", true) && badCfg.StringPath("error.code", "") == "invalid_params",
            "config.set rejects out-of-range resolution");

        const auto take = req("capture.take", "{\"width\":64,\"height\":64}");
        Check(take.BoolPath("ok", false) && take.IntPath("result.ticket", 0) > 0,
            "capture.take starts ticketed capture");

        // Wait for worker completion then pump OnUiTick
        for (int i = 0; i < 100; ++i)
        {
            bridge.OnUiTick();
            bool sawCompleted = false;
            for (const auto& ev : events)
            {
                if (ev.find("\"capture.completed\"") != std::string::npos) sawCompleted = true;
            }
            if (sawCompleted) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        bool sawStateChanged = false;
        bool sawCaptureCompleted = false;
        for (const auto& ev : events)
        {
            if (ev.find("\"state.changed\"") != std::string::npos) sawStateChanged = true;
            if (ev.find("\"capture.completed\"") != std::string::npos) sawCaptureCompleted = true;
        }
        Check(sawStateChanged && sawCaptureCompleted,
            "OnUiTick emits state.changed and capture.completed events");

        Check(req("folder.open").BoolPath("ok", false) && folderOpened,
            "folder.open invokes host folder opener");

        const auto browsed = req("folder.browse");
        Check(browsed.BoolPath("ok", false) && !browsed.BoolPath("result.cancelled", true),
            "folder.browse updates outputDir");

        const auto stateAfter = req("state.get");
        Check(stateAfter.StringPath("result.config.outputDir", "").find("CustomShots") != std::string::npos,
            "state.get reflects updated outputDir");

        bridge.OnShutdown();
        bool sawClosed = false;
        for (const auto& ev : events)
        {
            if (ev.find("\"bridge.closed\"") != std::string::npos) sawClosed = true;
        }
        Check(sawClosed, "OnShutdown emits bridge.closed");

        engine.Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // ---- Test 6: Compiled DLL Export & Service Registration ABI ------------

    void TestCompiledPluginDll(const std::filesystem::path& dllPath)
    {
        BeginTest("Compiled ScreenshotManager.dll GakuPlugin_Query & Activate ABI");
        if (!std::filesystem::is_regular_file(dllPath))
        {
            std::printf("  [SKIP] DLL not built at %ls\n", dllPath.c_str());
            return;
        }

        HMODULE mod = LoadLibraryW(dllPath.c_str());
        Check(mod != nullptr, "LoadLibraryW(ScreenshotManager.dll) succeeded");
        if (!mod) return;

        auto queryFn = reinterpret_cast<FnGakuPlugin_Query>(GetProcAddress(mod, "GakuPlugin_Query"));
        Check(queryFn != nullptr, "GakuPlugin_Query exported");
        if (!queryFn) { FreeLibrary(mod); return; }

        GakuPluginDescriptor desc{};
        Check(queryFn(99u, sizeof(desc), &desc) == GAKU_E_VERSION, "Rejects wrong ABI version");
        Check(queryFn(GAKU_ABI_VERSION, 4u, &desc) == GAKU_E_SIZE, "Rejects small struct size");
        Check(queryFn(GAKU_ABI_VERSION, sizeof(desc), &desc) == GAKU_OK, "GakuPlugin_Query succeeds");
        Check(std::string(desc.id) == "screenshotmanager" &&
            std::string(desc.name) == "ScreenshotManager" &&
            std::string(desc.version) == "0.1.0",
            "Plugin descriptor metadata matches screenshotmanager / ScreenshotManager / 0.1.0");
        Check(desc.activate && desc.onRuntimeReady && desc.onTick && desc.shutdown,
            "Plugin lifecycle callbacks populated");

        auto queryMetaFn = reinterpret_cast<FnGakuPlugin_QueryMetadata>(
            GetProcAddress(mod, "GakuPlugin_QueryMetadata"));
        Check(queryMetaFn != nullptr, "GakuPlugin_QueryMetadata exported");
        if (queryMetaFn)
        {
            GakuPluginMetadataV1 meta{};
            Check(queryMetaFn(99u, sizeof(meta), &meta) == GAKU_E_VERSION,
                "QueryMetadata rejects wrong ABI version");
            Check(queryMetaFn(GAKU_METADATA_ABI_VERSION, sizeof(meta), nullptr) == GAKU_E_INVALID_ARGUMENT,
                "QueryMetadata rejects null output");
            Check(queryMetaFn(GAKU_METADATA_ABI_VERSION, 4u, &meta) == GAKU_E_SIZE,
                "QueryMetadata rejects undersized output (4 bytes)");
            Check(queryMetaFn(GAKU_METADATA_ABI_VERSION, sizeof(meta) - 1u, &meta) == GAKU_E_SIZE,
                "QueryMetadata rejects undersized output (sizeof - 1)");
            Check(queryMetaFn(GAKU_METADATA_ABI_VERSION, sizeof(meta), &meta) == GAKU_OK,
                "GakuPlugin_QueryMetadata succeeds");
            Check(meta.abiVersion == GAKU_METADATA_ABI_VERSION &&
                meta.structSize == sizeof(GakuPluginMetadataV1),
                "Metadata ABI version and structSize match");
            Check(meta.originalAuthor && std::string(meta.originalAuthor) == "ScreenshotManager Contributors" &&
                meta.maintainerCount == 1u && meta.maintainers && meta.maintainers[0] &&
                std::string(meta.maintainers[0]) == "crested-ezios" &&
                meta.subVersion && std::string(meta.subVersion).empty() &&
                meta.releaseChannel && std::string(meta.releaseChannel) == "stable",
                "Metadata author, maintainers, subVersion, and releaseChannel match");
            Check(meta.summary &&
                std::string(meta.summary) == "Multi-resolution offscreen screenshot capture and ACES green-screen matte plugin for GakuLoader." &&
                meta.license && std::string(meta.license) == "MIT" &&
                meta.minLoaderVersion && std::string(meta.minLoaderVersion) == "1.1.0",
                "Metadata summary, license, and minLoaderVersion match");
            Check(meta.repositoryUrl &&
                std::string(meta.repositoryUrl) == "https://github.com/crested-ezios/ScreenshotManager" &&
                meta.releaseUrl &&
                std::string(meta.releaseUrl) == "https://github.com/crested-ezios/ScreenshotManager/releases" &&
                meta.issuesUrl &&
                std::string(meta.issuesUrl) == "https://github.com/crested-ezios/ScreenshotManager/issues",
                "Metadata repository/release/issues URLs match");
            bool hasEn = false, hasZhHans = false, hasZhHant = false, hasJa = false;
            if (meta.localizedSummaries && meta.localizedSummaryCount == 4u)
            {
                for (uint32_t i = 0; i < meta.localizedSummaryCount; ++i)
                {
                    const auto& item = meta.localizedSummaries[i];
                    if (!item.languageTag || !item.text || !item.text[0]) continue;
                    const std::string tag(item.languageTag);
                    if (tag == "en") hasEn = (std::string(item.text) == meta.summary);
                    else if (tag == "zh-Hans") hasZhHans = true;
                    else if (tag == "zh-Hant") hasZhHant = true;
                    else if (tag == "ja") hasJa = true;
                }
            }
            Check(hasEn && hasZhHans && hasZhHant && hasJa,
                "Metadata localizedSummaries include en, zh-Hans, zh-Hant, ja");
            Check(meta.dependencyCount == 1u && meta.dependencies &&
                meta.dependencies[0].pluginId && std::string(meta.dependencies[0].pluginId) == "gakumotion" &&
                meta.dependencies[0].displayName && std::string(meta.dependencies[0].displayName) == "GakuMotion" &&
                meta.dependencies[0].versionRange && std::string(meta.dependencies[0].versionRange) == ">=0.1.1" &&
                meta.dependencies[0].kind == GAKU_DEPENDENCY_OPTIONAL &&
                meta.dependencies[0].reserved == 0u &&
                meta.auxiliaryDllCount == 0u && meta.auxiliaryDlls == nullptr,
                "Metadata declares optional gakumotion >=0.1.1 dependency and no auxiliary DLLs");
        }

        const std::filesystem::path dir = MakeTempTestDir(L"DllHost");
        FakeHost host;
        host.localRoot = dir;
        host.configRoot = dir / L"Configs" / L"screenshotmanager";
        host.userDataRoot = dir / L"UserData" / L"screenshotmanager";
        host.greenScreenActive = 1u;
        const GakuHostApi api = host.MakeApi();

        void* pluginState = nullptr;
        Check(desc.activate(&api, &pluginState) == GAKU_OK && pluginState != nullptr,
            "Plugin activate succeeded");

        Check(host.services.count(GAKU_SERVICE_SCREENSHOT_CAPTURE) == 1u,
            "Registered screenshotmanager.capture/v1");
        Check(host.services.count("screenshotmanager.web-panel/v1") == 1u,
            "Registered screenshotmanager.web-panel/v1");
        Check(!host.registeredHotkeyIds.empty() && host.registeredHotkeyIds[0] == "take-screenshot" &&
            host.registeredHotkeyVk == VK_F7 &&
            host.registeredHotkeyDisplay == "ScreenshotManager: Capture Screenshot",
            "Registered VK_F7 take-screenshot hotkey");

        GakuScreenshotServiceV1 capSvc{};
        if (host.services[GAKU_SERVICE_SCREENSHOT_CAPTURE].size() == sizeof(capSvc))
            std::memcpy(&capSvc, host.services[GAKU_SERVICE_SCREENSHOT_CAPTURE].data(), sizeof(capSvc));
        Check(capSvc.abiVersion == 1u && capSvc.structSize == sizeof(capSvc) && capSvc.getCapabilities != nullptr,
            "GakuScreenshotServiceV1 table valid");

        GakuScreenshotCapabilitiesV1 caps{};
        Check(capSvc.getCapabilities(capSvc.context, &caps) == GAKU_OK && caps.greenScreenDetected == 1u,
            "DLL getCapabilities lazily detects gakumotion.stage-state/v1 greenScreenActive=1");

        GakuWebPanelServiceV1 webSvc{};
        if (host.services["screenshotmanager.web-panel/v1"].size() == sizeof(webSvc))
            std::memcpy(&webSvc, host.services["screenshotmanager.web-panel/v1"].data(), sizeof(webSvc));
        Check(webSvc.abiVersion == 1u && webSvc.structSize == sizeof(webSvc) && webSvc.getAssets != nullptr,
            "GakuWebPanelServiceV1 table valid");

        GakuWebPanelAssetsV1 assets{};
        Check(webSvc.getAssets(webSvc.context, &assets) == GAKU_OK &&
            std::wstring(assets.virtualHost) == L"screenshotmanager.invalid",
            "DLL WebPanel virtualHost is screenshotmanager.invalid");
        const std::wstring assetsDir(assets.assetsDir ? assets.assetsDir : L"");
        Check(assetsDir.ends_with(L"ScreenshotManager.UI"),
            "DLL WebPanel assetsDir ends with ScreenshotManager.UI");

        char resp[4096]{};
        uint32_t needed = 0;
        const char* cfgReq = "{\"v\":1,\"id\":2,\"type\":\"request\",\"method\":\"config.set\",\"params\":{\"width\":1920,\"height\":1080}}";
        Check(webSvc.handleMessage(webSvc.context, cfgReq, sizeof(resp), resp, &needed) == GAKU_OK,
            "DLL webPanel handleMessage(config.set) succeeded");
        Check(std::filesystem::is_regular_file(host.configRoot / L"config.json"),
            "DLL config.set persisted config.json via GakuPaths");

        desc.onTick(pluginState, 0.016f);
        desc.shutdown(pluginState);
        Check(host.hotkeyUnregistered, "Shutdown unregisters take-screenshot hotkey");

        GakuScreenshotServiceV1 capSvcCheck{};
        if (host.services["screenshotmanager.capture/v1"].size() == sizeof(capSvcCheck))
            std::memcpy(&capSvcCheck, host.services["screenshotmanager.capture/v1"].data(), sizeof(capSvcCheck));
        Check(capSvcCheck.armPresentCapture != nullptr && capSvcCheck.consumePresentCapture != nullptr &&
            capSvcCheck.cancelPresentCapture != nullptr,
            "DLL GakuScreenshotServiceV1 exports Present capture methods");

        FreeLibrary(mod);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void TestSwapchainPresentCaptureStateMachine()
    {
        BeginTest("Swapchain Present Backbuffer Capture State Machine");
        ScreenshotManager::ScreenshotEngine engine;
        engine.SetTestRenderHook(
            [](uint32_t, uint32_t, bool, std::vector<uint8_t>&, std::vector<uint8_t>&,
                ScreenshotManager::PixelOrientation&, std::string&) {
                return true;
            },
            4u, 2u);
        engine.SetTestUnityThreadBypass(true);
        engine.SetTestPresentHookOverride(true);

        // 1. Before any arm, consume reports outToken=0 cleanly.
        uint64_t tok = 99;
        std::vector<uint8_t> out(4u * 2u * 4u, 0);
        GakuScreenshotFrameInfoV1 info{};
        info.abiVersion = GAKU_SCREENSHOT_VERSION;
        info.structSize = sizeof(info);
        char err[128]{};
        Check(engine.ConsumePresentCapture(&tok, out.data(), out.size(), &info, err, sizeof(err)) == GAKU_OK && tok == 0,
            "Unarmed consume returns GAKU_OK with token=0");

        // 2. Arm 4x2 BGRA8 -> simulate DXGI B8G8R8A8_UNORM_SRGB upright Present (no 180 flip!)
        Check(engine.ArmPresentCapture(7u, 4u, 2u, GAKU_PIXEL_RGBA8_SRGB_TOPDOWN) == GAKU_OK,
            "ArmPresentCapture 4x2 succeeded");
        Check(engine.ConsumePresentCapture(&tok, out.data(), out.size(), &info, err, sizeof(err)) == GAKU_OK && tok == 0,
            "Pending consume before Present returns token=0");

        std::vector<uint8_t> raw(4u * 2u * 4u, 0);
        // Pixel (0, 0) in BGRA = (B=30, G=20, R=10, A=0), Pixel (3, 1) = (B=130, G=120, R=110, A=12)
        raw[0] = 30; raw[1] = 20; raw[2] = 10; raw[3] = 0;
        const size_t br = (1u * 4u + 3u) * 4u;
        raw[br + 0] = 130; raw[br + 1] = 120; raw[br + 2] = 110; raw[br + 3] = 12;

        Check(engine.SimulatePresentBackbufferForTest(4u, 2u, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            raw.data(), 4u * 4u, 555u), "SimulatePresentBackbufferForTest succeeded");

        Check(engine.ConsumePresentCapture(&tok, out.data(), out.size(), &info, err, sizeof(err)) == GAKU_OK &&
            tok == 7u && info.width == 4u && info.height == 2u && info.hasAlpha == 0u && info.unityFrame == 555u,
            "ConsumePresentCapture returned captured token and metadata");
        Check(out[0] == 10 && out[1] == 20 && out[2] == 30 && out[3] == 255 &&
            out[br + 0] == 110 && out[br + 1] == 120 && out[br + 2] == 130 && out[br + 3] == 255,
            "Present backbuffer preserves upright orientation, converts BGRA->RGBA, forces opaque alpha=255");

        // 3. Resolution mismatch triggers failure status so VideoExporter falls back cleanly
        Check(engine.ArmPresentCapture(8u, 4u, 2u, GAKU_PIXEL_RGBA8_SRGB_TOPDOWN) == GAKU_OK,
            "Re-arm token=8 succeeded");
        Check(!engine.SimulatePresentBackbufferForTest(8u, 2u, DXGI_FORMAT_R8G8B8A8_UNORM,
            raw.data(), 8u * 4u, 556u), "Mismatched backbuffer size rejected");
        Check(engine.ConsumePresentCapture(&tok, out.data(), out.size(), &info, err, sizeof(err)) == GAKU_E_UNAVAILABLE,
            "ConsumePresentCapture surfaces GAKU_E_UNAVAILABLE after size mismatch");

        engine.Shutdown();
    }

    void TestCameraRenderBottomUpOrientation()
    {
        BeginTest("Camera.Render() Bottom-Up Un-Mirrored Orientation & Present Parity");
        using ScreenshotManager::ScreenshotEngine;

        // 1. Verify offscreen Camera.Render() readback orientation contract directly.
        const auto offscreenOrient = ScreenshotEngine::OffscreenRenderOrientation();
        Check(!offscreenOrient.flipHorizontal && offscreenOrient.flipVertical,
            "OffscreenRenderOrientation is flipVertical=true, flipHorizontal=false");

        const std::filesystem::path dir = MakeTempTestDir(L"CameraOrient");
        constexpr uint32_t W = 8;
        constexpr uint32_t H = 4;

        // Distinct asymmetric upright scene so any horizontal or vertical flip is caught:
        // - Top-left     (0, 0)     -> (10, 20, 30, 255)
        // - Top-right    (W-1, 0)   -> (40, 50, 60, 255) in opaque, transparent (A=0) in alpha
        // - Bottom-left  (0, H-1)   -> (70, 80, 90, 255)
        // - Bottom-right (W-1, H-1) -> (100, 110, 120, 255) in opaque, semi-transparent (200,120,60,A~128) in alpha
        // - Remaining pixels: distinct gradient/midtones in opaque, background in alpha.
        auto makeUprightPixel = [&](uint32_t ux, uint32_t uy, bool dualPass, bool isWhitePass,
            uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
            if (ux == 0 && uy == 0)
            {
                r = 10; g = 20; b = 30; a = 255;
            }
            else if (ux == W - 1u && uy == 0)
            {
                if (dualPass)
                {
                    r = g = b = isWhitePass ? 224 : 0;
                    a = 255;
                }
                else
                {
                    r = 40; g = 50; b = 60; a = 255;
                }
            }
            else if (ux == 0 && uy == H - 1u)
            {
                r = 70; g = 80; b = 90; a = 255;
            }
            else if (ux == W - 1u && uy == H - 1u)
            {
                if (dualPass)
                {
                    // straight (200,120,60) at alpha=0.5 over ACES white 224 -> B=(100,60,30), W=(212,172,142)
                    r = isWhitePass ? 212 : 100;
                    g = isWhitePass ? 172 : 60;
                    b = isWhitePass ? 142 : 30;
                    a = 255;
                }
                else
                {
                    r = 100; g = 110; b = 120; a = 255;
                }
            }
            else
            {
                if (dualPass)
                {
                    r = g = b = isWhitePass ? 224 : 0;
                    a = 255;
                }
                else
                {
                    r = static_cast<uint8_t>(20 + ux * 10);
                    g = static_cast<uint8_t>(40 + uy * 20);
                    b = static_cast<uint8_t>(150 + ux * 5);
                    a = 255;
                }
            }
        };

        ScreenshotEngine engine;
        engine.SetTestUnityThreadBypass(true);
        engine.SetTestPresentHookOverride(true);
        engine.InitializeWorker();
        engine.SetTestRenderHook(
            [&](uint32_t w, uint32_t h, bool dualPass,
                std::vector<uint8_t>& outB, std::vector<uint8_t>& outW,
                ScreenshotManager::PixelOrientation& outOrient, std::string&) -> bool {
                const size_t n = static_cast<size_t>(w) * h * 4u;
                outB.assign(n, 0);
                if (dualPass) outW.assign(n, 0);
                outOrient = ScreenshotEngine::OffscreenRenderOrientation();

                // Simulate D3D11 RenderTexture / Texture2D bottom-up readback:
                // Source row (h - 1 - uy) stores upright row uy, with x UN-MIRRORED (sx == ux).
                // Specifically, bottom-left (0, h-1) in source holds upright top-left (0, 0).
                for (uint32_t uy = 0; uy < h; ++uy)
                {
                    const uint32_t sy = h - 1u - uy;
                    for (uint32_t ux = 0; ux < w; ++ux)
                    {
                        const size_t si = (static_cast<size_t>(sy) * w + ux) * 4u;
                        uint8_t r = 0, g = 0, b = 0, a = 255;
                        makeUprightPixel(ux, uy, dualPass, false, r, g, b, a);
                        outB[si + 0] = r; outB[si + 1] = g; outB[si + 2] = b; outB[si + 3] = a;
                        if (dualPass)
                        {
                            makeUprightPixel(ux, uy, dualPass, true, r, g, b, a);
                            outW[si + 0] = r; outW[si + 1] = g; outW[si + 2] = b; outW[si + 3] = a;
                        }
                    }
                }
                return true;
            },
            W, H);

        char errBuf[256]{};

        // 2. Opaque path (file PNG + immediate RGBA8 + immediate BGRA8)
        {
            const std::filesystem::path opaquePng = dir / L"camera_opaque.png";
            const std::string opaquePngUtf8 = ScreenshotManager::PathToUtf8(opaquePng);
            GakuScreenshotFileRequestV1 fReq{};
            fReq.abiVersion = GAKU_SCREENSHOT_VERSION;
            fReq.structSize = sizeof(fReq);
            fReq.width = W;
            fReq.height = H;
            fReq.imageFormat = GAKU_IMAGE_PNG;
            fReq.alphaMode = GAKU_SCREENSHOT_ALPHA_OPAQUE;
            std::memcpy(fReq.outputPathUtf8, opaquePngUtf8.c_str(), opaquePngUtf8.size() + 1);

            uint64_t ticket = 0;
            Check(engine.RequestFileCapture(&fReq, &ticket, dir) == GAKU_OK,
                "Opaque bottom-up RequestFileCapture accepted");
            GakuScreenshotResultV1 fRes{};
            for (int i = 0; i < 200; ++i)
            {
                if (engine.PollCapture(ticket, &fRes) == GAKU_OK && fRes.state != GAKU_CAPTURE_PENDING) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            Check(fRes.state == GAKU_CAPTURE_SUCCEEDED && fRes.hasAlpha == 0u,
                "Opaque bottom-up file PNG capture succeeded");
            engine.ReleaseTicket(ticket);

            ScreenshotManager::WicWriter::DecodedImage dec;
            std::string dErr;
            Check(ScreenshotManager::WicWriter::ReadImageForTest(opaquePng, dec, &dErr),
                "Decoded opaque bottom-up PNG: " + dErr);

            GakuScreenshotFrameRequestV1 rReq{GAKU_SCREENSHOT_VERSION, sizeof(rReq), W, H,
                GAKU_PIXEL_RGBA8_SRGB_TOPDOWN, GAKU_SCREENSHOT_ALPHA_OPAQUE};
            GakuScreenshotFrameRequestV1 bReq{GAKU_SCREENSHOT_VERSION, sizeof(bReq), W, H,
                GAKU_PIXEL_BGRA8_SRGB_TOPDOWN, GAKU_SCREENSHOT_ALPHA_OPAQUE};
            GakuScreenshotFrameInfoV1 rInfo{}, bInfo{};
            std::vector<uint8_t> rgbaBuf(W * H * 4u, 0);
            std::vector<uint8_t> bgraBuf(W * H * 4u, 0);
            Check(engine.CaptureFrameImmediate(&rReq, rgbaBuf.data(), rgbaBuf.size(), &rInfo, errBuf, sizeof(errBuf)) == GAKU_OK,
                "Opaque bottom-up CaptureFrameImmediate RGBA8 succeeded");
            Check(engine.CaptureFrameImmediate(&bReq, bgraBuf.data(), bgraBuf.size(), &bInfo, errBuf, sizeof(errBuf)) == GAKU_OK,
                "Opaque bottom-up CaptureFrameImmediate BGRA8 succeeded");
            Check(dec.rgbaTopDown == rgbaBuf,
                "Opaque Camera.Render() file PNG matches CaptureFrameImmediate RGBA8");

            auto px = [&](uint32_t x, uint32_t y) -> const uint8_t* {
                return &rgbaBuf[(static_cast<size_t>(y) * W + x) * 4u];
            };
            // Bottom-left source (0, H-1) -> top-left output (0, 0), NOT (W-1, 0)
            Check(px(0, 0)[0] == 10 && px(0, 0)[1] == 20 && px(0, 0)[2] == 30 && px(0, 0)[3] == 255,
                "Opaque bottom-left source (0,H-1) maps to top-left output (0,0)");
            Check(px(W - 1u, 0)[0] == 40 && px(W - 1u, 0)[1] == 50 && px(W - 1u, 0)[2] == 60 && px(W - 1u, 0)[3] == 255,
                "Opaque bottom-right source (W-1,H-1) maps to top-right output (W-1,0) without horizontal mirror");
            Check(px(0, H - 1u)[0] == 70 && px(0, H - 1u)[1] == 80 && px(0, H - 1u)[2] == 90 && px(0, H - 1u)[3] == 255,
                "Opaque top-left source (0,0) maps to bottom-left output (0,H-1)");
            Check(px(W - 1u, H - 1u)[0] == 100 && px(W - 1u, H - 1u)[1] == 110 && px(W - 1u, H - 1u)[2] == 120 && px(W - 1u, H - 1u)[3] == 255,
                "Opaque top-right source (W-1,0) maps to bottom-right output (W-1,H-1)");

            // BGRA8 top-left pixel must be (B=30, G=20, R=10, A=255)
            Check(bgraBuf[0] == 30 && bgraBuf[1] == 20 && bgraBuf[2] == 10 && bgraBuf[3] == 255,
                "Opaque BGRA8 top-left output matches un-mirrored RGBA8");

            // Parity check against swapchain-present upright top-down capture for the same scene
            std::vector<uint8_t> presentSrc(W * H * 4u, 0);
            for (uint32_t uy = 0; uy < H; ++uy)
            {
                for (uint32_t ux = 0; ux < W; ++ux)
                {
                    const size_t i = (static_cast<size_t>(uy) * W + ux) * 4u;
                    uint8_t r = 0, g = 0, b = 0, a = 255;
                    makeUprightPixel(ux, uy, false, false, r, g, b, a);
                    presentSrc[i + 0] = r; presentSrc[i + 1] = g; presentSrc[i + 2] = b; presentSrc[i + 3] = 0;
                }
            }
            Check(engine.ArmPresentCapture(42u, W, H, GAKU_PIXEL_RGBA8_SRGB_TOPDOWN) == GAKU_OK,
                "ArmPresentCapture for orientation parity succeeded");
            Check(engine.SimulatePresentBackbufferForTest(W, H, DXGI_FORMAT_R8G8B8A8_UNORM,
                presentSrc.data(), W * 4u, 900u), "SimulatePresentBackbufferForTest parity frame succeeded");
            uint64_t gotTok = 0;
            GakuScreenshotFrameInfoV1 pInfo{};
            pInfo.abiVersion = GAKU_SCREENSHOT_VERSION;
            pInfo.structSize = sizeof(pInfo);
            std::vector<uint8_t> presentOut(W * H * 4u, 0);
            Check(engine.ConsumePresentCapture(&gotTok, presentOut.data(), presentOut.size(), &pInfo, errBuf, sizeof(errBuf)) == GAKU_OK &&
                gotTok == 42u && presentOut == rgbaBuf,
                "Offscreen Camera.Render() opaque RGBA8 is byte-identical to swapchain-present upright output");
        }

        // 3. Dual-pass Alpha path (file PNG + immediate RGBA8 + immediate BGRA8)
        {
            const std::filesystem::path alphaPng = dir / L"camera_alpha.png";
            const std::string alphaPngUtf8 = ScreenshotManager::PathToUtf8(alphaPng);
            GakuScreenshotFileRequestV1 fReq{};
            fReq.abiVersion = GAKU_SCREENSHOT_VERSION;
            fReq.structSize = sizeof(fReq);
            fReq.width = W;
            fReq.height = H;
            fReq.imageFormat = GAKU_IMAGE_PNG;
            fReq.alphaMode = GAKU_SCREENSHOT_ALPHA_FORCE_TRANSPARENT;
            std::memcpy(fReq.outputPathUtf8, alphaPngUtf8.c_str(), alphaPngUtf8.size() + 1);

            uint64_t ticket = 0;
            Check(engine.RequestFileCapture(&fReq, &ticket, dir) == GAKU_OK,
                "Alpha bottom-up RequestFileCapture accepted");
            GakuScreenshotResultV1 fRes{};
            for (int i = 0; i < 200; ++i)
            {
                if (engine.PollCapture(ticket, &fRes) == GAKU_OK && fRes.state != GAKU_CAPTURE_PENDING) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            Check(fRes.state == GAKU_CAPTURE_SUCCEEDED && fRes.hasAlpha == 1u,
                "Alpha bottom-up file PNG capture succeeded");
            engine.ReleaseTicket(ticket);

            ScreenshotManager::WicWriter::DecodedImage dec;
            std::string dErr;
            Check(ScreenshotManager::WicWriter::ReadImageForTest(alphaPng, dec, &dErr),
                "Decoded alpha bottom-up PNG: " + dErr);

            GakuScreenshotFrameRequestV1 rReq{GAKU_SCREENSHOT_VERSION, sizeof(rReq), W, H,
                GAKU_PIXEL_RGBA8_SRGB_TOPDOWN, GAKU_SCREENSHOT_ALPHA_FORCE_TRANSPARENT};
            GakuScreenshotFrameInfoV1 rInfo{};
            std::vector<uint8_t> rgbaBuf(W * H * 4u, 0);
            Check(engine.CaptureFrameImmediate(&rReq, rgbaBuf.data(), rgbaBuf.size(), &rInfo, errBuf, sizeof(errBuf)) == GAKU_OK,
                "Alpha bottom-up CaptureFrameImmediate RGBA8 succeeded");
            Check(dec.rgbaTopDown == rgbaBuf,
                "Alpha Camera.Render() file PNG matches CaptureFrameImmediate RGBA8");

            auto px = [&](uint32_t x, uint32_t y) -> const uint8_t* {
                return &rgbaBuf[(static_cast<size_t>(y) * W + x) * 4u];
            };
            // Bottom-left source (0, H-1) -> top-left output (0, 0) opaque (10,20,30,255), NOT (W-1, 0)
            Check(px(0, 0)[0] == 10 && px(0, 0)[1] == 20 && px(0, 0)[2] == 30 && px(0, 0)[3] == 255,
                "Alpha bottom-left source (0,H-1) maps to top-left output (0,0)");
            // Top-right output (W-1, 0) is transparent background (NOT mirrored opaque (10,20,30))
            Check(px(W - 1u, 0)[0] == 0 && px(W - 1u, 0)[1] == 0 && px(W - 1u, 0)[2] == 0 && px(W - 1u, 0)[3] == 0,
                "Alpha top-right output (W-1,0) stays transparent (not mirrored from top-left)");
            // Bottom-left output (0, H-1) is opaque (70,80,90,255)
            Check(px(0, H - 1u)[0] == 70 && px(0, H - 1u)[1] == 80 && px(0, H - 1u)[2] == 90 && px(0, H - 1u)[3] == 255,
                "Alpha top-left source (0,0) maps to bottom-left output (0,H-1)");
            // Bottom-right output (W-1, H-1) recovers semi-transparent straight RGB(200,120,60), A~128
            Check(NearByte(px(W - 1u, H - 1u)[3], 128, 1) && NearByte(px(W - 1u, H - 1u)[0], 200, 3) &&
                NearByte(px(W - 1u, H - 1u)[1], 120, 3) && NearByte(px(W - 1u, H - 1u)[2], 60, 3),
                "Alpha bottom-right output (W-1,H-1) recovers straight RGB and alpha without horizontal mirror");
        }

        engine.Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void TestGakuLoaderPluginsAwareness()
    {
        BeginTest("gakuloader.plugins Runtime Binding Awareness & Lazy Fallback");
        using ScreenshotManager::ScreenshotEngine;

        auto countUnavailableLogs = [](const FakeHost& host) -> int {
            int count = 0;
            for (const auto& msg : host.logs)
            {
                if (msg.find("GakuMotion unavailable") != std::string::npos) ++count;
            }
            return count;
        };

        // 1. Absent gakuloader.plugins (older host): keeps full lazy fallback to gakumotion.stage-state/v1
        {
            FakeHost host;
            host.exposePluginsService = false;
            host.greenScreenActive = 1u;
            const GakuHostApi api = host.MakeApi();

            ScreenshotEngine engine;
            engine.SetHostApi(&api);
            GakuScreenshotCapabilitiesV1 caps{};
            Check(engine.GetCapabilities(&caps) == GAKU_OK && caps.greenScreenDetected == 1u,
                "Absent gakuloader.plugins falls back to lazy gakumotion.stage-state/v1 query");
            Check(host.stageStateQueryCount == 1u && countUnavailableLogs(host) == 0,
                "Absent gakuloader.plugins queried gakumotion.stage-state/v1 once with no unavailable log");
            engine.Shutdown();
        }

        // 2. gakuloader.plugins reports gakumotion disabled: logs once and skips gakumotion.stage-state/v1
        {
            FakeHost host;
            host.exposePluginsService = true;
            host.greenScreenActive = 1u;
            FakeHost::s_gakuMotionState = GAKU_PLUGIN_STATE_DISABLED;
            const GakuHostApi api = host.MakeApi();

            ScreenshotEngine engine;
            engine.SetHostApi(&api);
            GakuScreenshotCapabilitiesV1 caps{};
            Check(engine.GetCapabilities(&caps) == GAKU_OK && caps.greenScreenDetected == 0u,
                "Disabled gakumotion via gakuloader.plugins yields greenScreenDetected=0");
            Check(engine.GetCapabilities(&caps) == GAKU_OK && caps.greenScreenDetected == 0u,
                "Repeated GetCapabilities remains greenScreenDetected=0");
            Check(host.stageStateQueryCount == 0u && countUnavailableLogs(host) == 1,
                "Disabled gakumotion skips gakumotion.stage-state/v1 and logs once cleanly");
            engine.Shutdown();
        }

        // 3. gakuloader.plugins reports queried at Activate, then transitions to failed before GetCapabilities
        {
            FakeHost host;
            host.exposePluginsService = true;
            host.greenScreenActive = 1u;
            FakeHost::s_gakuMotionState = GAKU_PLUGIN_STATE_QUERIED;
            const GakuHostApi api = host.MakeApi();

            ScreenshotEngine engine;
            engine.SetHostApi(&api);
            Check(countUnavailableLogs(host) == 0,
                "Queried gakumotion at SetHostApi does not emit unavailable log prematurely");

            FakeHost::s_gakuMotionState = GAKU_PLUGIN_STATE_FAILED;
            GakuScreenshotCapabilitiesV1 caps{};
            Check(engine.GetCapabilities(&caps) == GAKU_OK && caps.greenScreenDetected == 0u,
                "Failed gakumotion detected lazily skips gakumotion.stage-state/v1");
            Check(host.stageStateQueryCount == 0u && countUnavailableLogs(host) == 1,
                "Failed gakumotion logs once and avoids querying gakumotion.stage-state/v1");
            engine.Shutdown();
        }

        // 4. gakuloader.plugins reports gakumotion activated: proceeds to query gakumotion.stage-state/v1
        {
            FakeHost host;
            host.exposePluginsService = true;
            host.greenScreenActive = 1u;
            FakeHost::s_gakuMotionState = GAKU_PLUGIN_STATE_ACTIVATED;
            const GakuHostApi api = host.MakeApi();

            ScreenshotEngine engine;
            engine.SetHostApi(&api);
            GakuScreenshotCapabilitiesV1 caps{};
            Check(engine.GetCapabilities(&caps) == GAKU_OK && caps.greenScreenDetected == 1u,
                "Activated gakumotion proceeds to gakumotion.stage-state/v1");
            Check(host.stageStateQueryCount == 1u && countUnavailableLogs(host) == 0,
                "Activated gakumotion queries gakumotion.stage-state/v1 once without unavailable log");
            engine.Shutdown();
        }
    }
} // namespace

int main(int argc, char** argv)
{
    TestAlphaMatte();
    TestDxgiRowPitchUnpack();
    TestWicImageWriter();
    TestConfigStore();
    TestEngineAndCaptureService();
    TestSwapchainPresentCaptureStateMachine();
    TestCameraRenderBottomUpOrientation();
    TestWebPanelBridge();
    TestGakuLoaderPluginsAwareness();

    std::filesystem::path dllPath;
    if (argc > 1 && argv[1] && argv[1][0])
    {
        dllPath = argv[1];
    }
    else
    {
        wchar_t exeBuf[32768]{};
        const DWORD len = GetModuleFileNameW(nullptr, exeBuf, static_cast<DWORD>(std::size(exeBuf)));
        if (len > 0 && len < std::size(exeBuf))
        {
            dllPath = std::filesystem::path(exeBuf, exeBuf + len).parent_path().parent_path() /
                L"native" / L"ScreenshotManager.dll";
        }
    }
    TestCompiledPluginDll(dllPath);

    std::printf("\nSummary: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
