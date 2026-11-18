// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "../include/GakuPlugin.h"
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
            if (std::strcmp(name, GAKU_SERVICE_MOTION_STAGE_STATE) == 0)
            {
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

        // Floor ceilDelta >= 16/255 safety test
        std::vector<uint8_t> lowBlack(4 * 4, 0);
        std::vector<uint8_t> lowWhite(4 * 4, 0);
        lowWhite[0] = lowWhite[1] = lowWhite[2] = 10;
        lowWhite[3] = 255;
        std::vector<uint8_t> lowOut(4 * 4, 0);
        Check(ExtractAlphaDifferenceMatte(2, 2, lowBlack.data(), lowWhite.data(), lowOut.data(), false),
            "Floor ceilDelta avoids divide-by-zero");
        Check(lowOut[3] > 0 && lowOut[3] < 255, "10/255 delta against 16/255 floor gives partial alpha");

        // UnpremultiplyAndConvertToRgba & RgbaToBgra
        const uint8_t premul[8] = {100, 50, 25, 128, 0, 0, 0, 0};
        uint8_t unpremul[8]{};
        Check(UnpremultiplyAndConvertToRgba(2, 1, premul, false, unpremul),
            "UnpremultiplyAndConvertToRgba succeeded");
        Check(NearByte(unpremul[0], 199, 1) && NearByte(unpremul[1], 100, 1) && NearByte(unpremul[2], 50, 1) && unpremul[3] == 128,
            "Unpremultiply recovers straight RGB from premultiplied pixel");
        Check(unpremul[4] == 0 && unpremul[5] == 0 && unpremul[6] == 0 && unpremul[7] == 0,
            "Unpremultiply clears A=0 RGB to zero");

        uint8_t bgra[8]{};
        Check(RgbaToBgra(2, 1, unpremul, bgra), "RgbaToBgra succeeded");
        Check(bgra[0] == unpremul[2] && bgra[1] == unpremul[1] && bgra[2] == unpremul[0] && bgra[3] == unpremul[3],
            "RgbaToBgra swaps R and B");
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
               bool& outFlip, std::string&) -> bool {
                const size_t n = static_cast<size_t>(w) * h * 4u;
                outB.assign(n, 0);
                outFlip = false;
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
               bool& flip, std::string&) {
                b.assign(static_cast<size_t>(w) * h * 4u, 200);
                wBuf.assign(static_cast<size_t>(w) * h * 4u, 200);
                flip = false;
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
            host.registeredHotkeyVk == VK_F11 &&
            host.registeredHotkeyDisplay == "ScreenshotManager: Capture Screenshot",
            "Registered VK_F11 take-screenshot hotkey");

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

        FreeLibrary(mod);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
} // namespace

int main(int argc, char** argv)
{
    TestAlphaMatte();
    TestWicImageWriter();
    TestConfigStore();
    TestEngineAndCaptureService();
    TestWebPanelBridge();

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
