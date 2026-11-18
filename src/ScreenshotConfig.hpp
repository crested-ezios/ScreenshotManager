// SPDX-License-Identifier: MIT
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "../include/GakuScreenshot.h"
#include "GkJsonLite.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ScreenshotManager
{
    inline constexpr uint32_t kMaxCaptureDimension = 7680u;
    inline constexpr uint32_t kDefaultJpegQuality = 92u;

    inline bool Utf8ToWide(std::string_view input, std::wstring& output)
    {
        if (input.find('\0') != std::string_view::npos ||
            input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            return false;
        if (input.empty())
        {
            output.clear();
            return true;
        }
        const int len = static_cast<int>(input.size());
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), len, nullptr, 0);
        if (count <= 0) return false;
        std::wstring wide(static_cast<size_t>(count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), len, wide.data(), count) != count)
            return false;
        output = std::move(wide);
        return true;
    }

    inline bool WideToUtf8(std::wstring_view input, std::string& output)
    {
        if (input.find(L'\0') != std::wstring_view::npos ||
            input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            return false;
        if (input.empty())
        {
            output.clear();
            return true;
        }
        const int len = static_cast<int>(input.size());
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), len, nullptr, 0, nullptr, nullptr);
        if (count <= 0) return false;
        std::string utf8(static_cast<size_t>(count), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), len, utf8.data(), count, nullptr, nullptr) != count)
            return false;
        output = std::move(utf8);
        return true;
    }

    inline std::filesystem::path PathFromUtf8(std::string_view utf8)
    {
        std::wstring wide;
        if (!Utf8ToWide(utf8, wide)) return {};
        return std::filesystem::path(wide);
    }

    inline std::string PathToUtf8(const std::filesystem::path& path)
    {
        std::string utf8;
        if (!WideToUtf8(path.wstring(), utf8)) return {};
        return utf8;
    }

    inline void AppendJsonEscaped(std::string& out, std::string_view text)
    {
        out.push_back('"');
        for (const unsigned char ch : text)
        {
            switch (ch)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20)
                {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(ch));
                    out += buf;
                }
                else
                {
                    out.push_back(static_cast<char>(ch));
                }
                break;
            }
        }
        out.push_back('"');
    }

    inline std::string JsonString(std::string_view text)
    {
        std::string out;
        out.reserve(text.size() + 4);
        AppendJsonEscaped(out, text);
        return out;
    }

    inline std::string JsonBool(bool value) { return value ? "true" : "false"; }

    inline std::string JsonNumber(double value)
    {
        if (!std::isfinite(value)) return "0";
        char buf[48]{};
        if (value == std::floor(value) && std::fabs(value) < 9.0e15)
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
        else
            std::snprintf(buf, sizeof(buf), "%.6g", value);
        return buf;
    }

    inline std::string ToAsciiLower(std::string_view s)
    {
        std::string out(s);
        for (char& ch : out)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return out;
    }

    struct ScreenshotConfig
    {
        uint32_t width{0};
        uint32_t height{0};
        uint32_t imageFormat{GAKU_IMAGE_PNG};
        uint32_t jpegQuality{kDefaultJpegQuality};
        uint32_t alphaMode{GAKU_SCREENSHOT_ALPHA_AUTO_GREENSCREEN};
        std::string outputDirUtf8;
    };

    inline bool IsValidResolution(uint32_t width, uint32_t height, uint32_t maxDim = kMaxCaptureDimension) noexcept
    {
        if (width == 0 && height == 0) return true;
        return width >= 1u && width <= maxDim && height >= 1u && height <= maxDim;
    }

    inline const char* ImageFormatToString(uint32_t format) noexcept
    {
        return format == GAKU_IMAGE_JPEG ? "jpeg" : "png";
    }

    inline const char* AlphaModeToString(uint32_t mode) noexcept
    {
        switch (mode)
        {
        case GAKU_SCREENSHOT_ALPHA_FORCE_TRANSPARENT: return "force_transparent";
        case GAKU_SCREENSHOT_ALPHA_OPAQUE: return "opaque";
        default: return "auto_greenscreen";
        }
    }

    inline bool ParseImageFormatValue(const GakuMotion::jsonlite::Value* value, uint32_t& out)
    {
        if (!value) return false;
        if (value->IsNumber())
        {
            const double v = value->numberValue;
            if (v == 0.0) { out = GAKU_IMAGE_PNG; return true; }
            if (v == 1.0) { out = GAKU_IMAGE_JPEG; return true; }
            return false;
        }
        if (value->IsString())
        {
            const std::string lower = ToAsciiLower(value->stringValue);
            if (lower == "png") { out = GAKU_IMAGE_PNG; return true; }
            if (lower == "jpeg" || lower == "jpg") { out = GAKU_IMAGE_JPEG; return true; }
        }
        return false;
    }

    inline bool ParseAlphaModeValue(const GakuMotion::jsonlite::Value* value, uint32_t& out)
    {
        if (!value) return false;
        if (value->IsNumber())
        {
            const double v = value->numberValue;
            if (v == 0.0) { out = GAKU_SCREENSHOT_ALPHA_AUTO_GREENSCREEN; return true; }
            if (v == 1.0) { out = GAKU_SCREENSHOT_ALPHA_FORCE_TRANSPARENT; return true; }
            if (v == 2.0) { out = GAKU_SCREENSHOT_ALPHA_OPAQUE; return true; }
            return false;
        }
        if (value->IsString())
        {
            const std::string lower = ToAsciiLower(value->stringValue);
            if (lower == "auto_greenscreen" || lower == "auto")
            {
                out = GAKU_SCREENSHOT_ALPHA_AUTO_GREENSCREEN;
                return true;
            }
            if (lower == "force_transparent" || lower == "transparent")
            {
                out = GAKU_SCREENSHOT_ALPHA_FORCE_TRANSPARENT;
                return true;
            }
            if (lower == "opaque")
            {
                out = GAKU_SCREENSHOT_ALPHA_OPAQUE;
                return true;
            }
        }
        return false;
    }

    inline std::string SerializeConfigJson(const ScreenshotConfig& cfg)
    {
        return std::string("{") +
            "\"width\":" + std::to_string(cfg.width) +
            ",\"height\":" + std::to_string(cfg.height) +
            ",\"imageFormat\":" + JsonString(ImageFormatToString(cfg.imageFormat)) +
            ",\"jpegQuality\":" + std::to_string(cfg.jpegQuality) +
            ",\"alphaMode\":" + JsonString(AlphaModeToString(cfg.alphaMode)) +
            ",\"outputDir\":" + JsonString(cfg.outputDirUtf8) +
            "}";
    }

    inline void LoadConfig(const std::filesystem::path& configFile, ScreenshotConfig& config,
        const std::string& defaultOutputDirUtf8)
    {
        config = ScreenshotConfig{};
        config.outputDirUtf8 = defaultOutputDirUtf8;
        if (configFile.empty()) return;

        std::ifstream in(configFile, std::ios::binary);
        if (!in.is_open()) return;
        std::ostringstream oss;
        oss << in.rdbuf();
        const std::string raw = oss.str();
        if (raw.empty()) return;

        GakuMotion::jsonlite::Value root;
        std::string err;
        if (!GakuMotion::jsonlite::ParseText(raw, root, err) || !root.IsObject()) return;

        const auto* wVal = root.Find("width");
        const auto* hVal = root.Find("height");
        if (wVal && wVal->IsNumber() && hVal && hVal->IsNumber())
        {
            const double w = wVal->numberValue;
            const double h = hVal->numberValue;
            if (w >= 0 && h >= 0 && w == std::floor(w) && h == std::floor(h) &&
                w <= kMaxCaptureDimension && h <= kMaxCaptureDimension)
            {
                const uint32_t uw = static_cast<uint32_t>(w);
                const uint32_t uh = static_cast<uint32_t>(h);
                if (IsValidResolution(uw, uh))
                {
                    config.width = uw;
                    config.height = uh;
                }
            }
        }

        uint32_t fmt = config.imageFormat;
        if (ParseImageFormatValue(root.Find("imageFormat"), fmt))
            config.imageFormat = fmt;

        const auto* qVal = root.Find("jpegQuality");
        if (qVal && qVal->IsNumber() && qVal->numberValue >= 1.0 && qVal->numberValue <= 100.0)
        {
            config.jpegQuality = static_cast<uint32_t>(std::lround(qVal->numberValue));
        }

        uint32_t alpha = config.alphaMode;
        if (ParseAlphaModeValue(root.Find("alphaMode"), alpha))
            config.alphaMode = alpha;

        const auto* dirVal = root.Find("outputDir");
        if (dirVal && dirVal->IsString() && !dirVal->stringValue.empty())
        {
            std::wstring checkWide;
            if (Utf8ToWide(dirVal->stringValue, checkWide))
                config.outputDirUtf8 = dirVal->stringValue;
        }
    }

    inline bool SaveConfigAtomic(const std::filesystem::path& configFile, const ScreenshotConfig& config,
        const std::function<bool()>& ensureRoot = {})
    {
        if (configFile.empty()) return false;
        if (ensureRoot)
        {
            ensureRoot();
        }
        std::error_code ec;
        if (!configFile.parent_path().empty())
        {
            std::filesystem::create_directories(configFile.parent_path(), ec);
            if (ec) return false;
        }

        const std::string payload = "{\n"
            "  \"version\": 1,\n"
            "  \"width\": " + std::to_string(config.width) + ",\n"
            "  \"height\": " + std::to_string(config.height) + ",\n"
            "  \"imageFormat\": " + JsonString(ImageFormatToString(config.imageFormat)) + ",\n"
            "  \"jpegQuality\": " + std::to_string(config.jpegQuality) + ",\n"
            "  \"alphaMode\": " + JsonString(AlphaModeToString(config.alphaMode)) + ",\n"
            "  \"outputDir\": " + JsonString(config.outputDirUtf8) + "\n"
            "}\n";

        static std::atomic<uint64_t> tmpSeq{1};
        const std::filesystem::path tmpFile = configFile.wstring() + L".tmp." +
            std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(tmpSeq.fetch_add(1, std::memory_order_relaxed));

        HANDLE h = CreateFileW(tmpFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;

        DWORD written = 0;
        const BOOL ok = WriteFile(h, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) &&
            written == payload.size() && FlushFileBuffers(h);
        CloseHandle(h);

        if (!ok)
        {
            DeleteFileW(tmpFile.c_str());
            return false;
        }
        if (!MoveFileExW(tmpFile.c_str(), configFile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(tmpFile.c_str());
            return false;
        }
        return true;
    }

    inline std::string FormatTimestampNow()
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return buf;
    }

    inline std::filesystem::path GenerateScreenshotFilePath(const std::filesystem::path& outputDir, uint32_t imageFormat)
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        const wchar_t* ext = (imageFormat == GAKU_IMAGE_JPEG) ? L".jpg" : L".png";
        wchar_t base[64]{};
        std::swprintf(base, std::size(base), L"Gakumas_%04u%02u%02u_%02u%02u%02u_%03u",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        std::error_code ec;
        std::filesystem::path candidate = outputDir / (std::wstring(base) + ext);
        if (!std::filesystem::exists(candidate, ec)) return candidate;

        for (uint32_t suffix = 1; suffix < 1000u; ++suffix)
        {
            candidate = outputDir / (std::wstring(base) + L"_" + std::to_wstring(suffix) + ext);
            if (!std::filesystem::exists(candidate, ec)) return candidate;
        }
        return candidate;
    }

    class ConfigStore
    {
    public:
        void Configure(std::filesystem::path configFile, std::string defaultOutputDirUtf8,
            std::function<bool()> ensureConfigRoot = {})
        {
            std::lock_guard<std::mutex> lock(mutex_);
            configFile_ = std::move(configFile);
            defaultOutputDirUtf8_ = std::move(defaultOutputDirUtf8);
            ensureRoot_ = std::move(ensureConfigRoot);
            LoadConfig(configFile_, config_, defaultOutputDirUtf8_);
        }

        ScreenshotConfig Get() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return config_;
        }

        std::string DefaultOutputDirUtf8() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return defaultOutputDirUtf8_;
        }

        bool Update(const ScreenshotConfig& next, bool persist = true)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            config_ = next;
            if (config_.outputDirUtf8.empty()) config_.outputDirUtf8 = defaultOutputDirUtf8_;
            if (!persist || configFile_.empty()) return true;
            return SaveConfigAtomic(configFile_, config_, ensureRoot_);
        }

    private:
        mutable std::mutex mutex_;
        std::filesystem::path configFile_;
        std::string defaultOutputDirUtf8_;
        std::function<bool()> ensureRoot_;
        ScreenshotConfig config_;
    };
} // namespace ScreenshotManager
