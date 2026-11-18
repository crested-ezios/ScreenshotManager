// SPDX-License-Identifier: MIT
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace ScreenshotManager::WicWriter
{
    using Microsoft::WRL::ComPtr;

    struct ScopedComInit
    {
        HRESULT hr{E_FAIL};
        ScopedComInit() noexcept
        {
            hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        }
        ~ScopedComInit() noexcept
        {
            if (hr == S_OK || hr == S_FALSE) CoUninitialize();
        }
        bool Ok() const noexcept
        {
            return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        }
    };

    inline std::string FormatHResultError(const char* stage, HRESULT hr)
    {
        char buf[192]{};
        std::snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lX)", stage, static_cast<unsigned long>(hr));
        return buf;
    }

    inline std::filesystem::path MakeTempPath(const std::filesystem::path& target)
    {
        static std::atomic<uint64_t> counter{1};
        const uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
        std::wstring name = target.filename().wstring();
        name += L".tmp.";
        name += std::to_wstring(GetCurrentProcessId());
        name += L".";
        name += std::to_wstring(seq);
        return target.parent_path() / name;
    }

    inline bool EnsureParentDir(const std::filesystem::path& target, std::string* errorOut)
    {
        if (target.empty())
        {
            if (errorOut) *errorOut = "Output path is empty";
            return false;
        }
        const auto parent = target.parent_path();
        if (parent.empty()) return true;
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            if (errorOut) *errorOut = "Cannot create output directory: " + ec.message();
            return false;
        }
        return true;
    }

    inline bool ValidateBuffer(uint32_t width, uint32_t height, const uint8_t* topDownRgba, std::string* errorOut)
    {
        if (width == 0 || height == 0 || !topDownRgba)
        {
            if (errorOut) *errorOut = "Invalid image dimensions or null pixel buffer";
            return false;
        }
        const uint64_t w = width;
        const uint64_t h = height;
        if (w * h > static_cast<uint64_t>((std::numeric_limits<UINT>::max)()) / 4ull)
        {
            if (errorOut) *errorOut = "Image byte size exceeds 32-bit WIC stride/buffer limits";
            return false;
        }
        return true;
    }

    inline bool EncodeEncodedContainer(
        const std::filesystem::path& path,
        const GUID& containerFormat,
        uint32_t width,
        uint32_t height,
        const uint8_t* topDownRgba,
        bool preserveAlpha,
        int jpegQuality1To100,
        std::string* errorOut)
    {
        if (!ValidateBuffer(width, height, topDownRgba, errorOut)) return false;
        if (!EnsureParentDir(path, errorOut)) return false;

        ScopedComInit com;
        if (!com.Ok())
        {
            if (errorOut) *errorOut = FormatHResultError("CoInitializeEx", com.hr);
            return false;
        }

        const std::filesystem::path tmpPath = MakeTempPath(path);
        auto cleanupTemp = [&]() {
            std::error_code ec;
            std::filesystem::remove(tmpPath, ec);
        };

        HRESULT hr = S_OK;
        {
            ComPtr<IWICImagingFactory> factory;
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory));
            if (FAILED(hr) || !factory)
            {
                if (errorOut) *errorOut = FormatHResultError("CoCreateInstance(WICImagingFactory)", hr);
                return false;
            }

            ComPtr<IWICStream> stream;
            hr = factory->CreateStream(&stream);
            if (FAILED(hr) || !stream)
            {
                if (errorOut) *errorOut = FormatHResultError("CreateStream", hr);
                return false;
            }

            hr = stream->InitializeFromFilename(tmpPath.c_str(), GENERIC_WRITE);
            if (FAILED(hr))
            {
                if (errorOut) *errorOut = FormatHResultError("InitializeFromFilename", hr);
                cleanupTemp();
                return false;
            }

            ComPtr<IWICBitmapEncoder> encoder;
            hr = factory->CreateEncoder(containerFormat, nullptr, &encoder);
            if (FAILED(hr) || !encoder)
            {
                if (errorOut) *errorOut = FormatHResultError("CreateEncoder", hr);
                cleanupTemp();
                return false;
            }

            hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
            if (FAILED(hr))
            {
                if (errorOut) *errorOut = FormatHResultError("Encoder::Initialize", hr);
                cleanupTemp();
                return false;
            }

            ComPtr<IWICBitmapFrameEncode> frame;
            ComPtr<IPropertyBag2> props;
            hr = encoder->CreateNewFrame(&frame, &props);
            if (FAILED(hr) || !frame)
            {
                if (errorOut) *errorOut = FormatHResultError("CreateNewFrame", hr);
                cleanupTemp();
                return false;
            }

            if (jpegQuality1To100 > 0 && props)
            {
                const int clamped = std::clamp(jpegQuality1To100, 1, 100);
                PROPBAG2 option{};
                option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                VARIANT varValue{};
                varValue.vt = VT_R4;
                varValue.fltVal = static_cast<float>(clamped) / 100.0f;
                hr = props->Write(1, &option, &varValue);
                if (FAILED(hr))
                {
                    if (errorOut) *errorOut = FormatHResultError("IPropertyBag2::Write(ImageQuality)", hr);
                    cleanupTemp();
                    return false;
                }
            }

            hr = frame->Initialize(props.Get());
            if (FAILED(hr))
            {
                if (errorOut) *errorOut = FormatHResultError("Frame::Initialize", hr);
                cleanupTemp();
                return false;
            }

            hr = frame->SetSize(width, height);
            if (FAILED(hr))
            {
                if (errorOut) *errorOut = FormatHResultError("Frame::SetSize", hr);
                cleanupTemp();
                return false;
            }

            WICPixelFormatGUID desiredFmt = preserveAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
            WICPixelFormatGUID actualFmt = desiredFmt;
            hr = frame->SetPixelFormat(&actualFmt);
            if (FAILED(hr) || !IsEqualGUID(actualFmt, desiredFmt))
            {
                if (errorOut) *errorOut = FormatHResultError("Frame::SetPixelFormat", hr);
                cleanupTemp();
                return false;
            }

            const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
            const UINT bytesPerPixel = preserveAlpha ? 4u : 3u;
            const UINT stride = width * bytesPerPixel;
            const UINT bufferSize = stride * height;
            std::vector<uint8_t> encodedPixels(bufferSize);

            if (preserveAlpha)
            {
                for (size_t i = 0; i < pixelCount; ++i)
                {
                    const size_t s = i * 4u;
                    encodedPixels[s + 0] = topDownRgba[s + 2]; // B
                    encodedPixels[s + 1] = topDownRgba[s + 1]; // G
                    encodedPixels[s + 2] = topDownRgba[s + 0]; // R
                    encodedPixels[s + 3] = topDownRgba[s + 3]; // A
                }
            }
            else
            {
                for (size_t i = 0; i < pixelCount; ++i)
                {
                    const size_t s = i * 4u;
                    const size_t d = i * 3u;
                    encodedPixels[d + 0] = topDownRgba[s + 2]; // B
                    encodedPixels[d + 1] = topDownRgba[s + 1]; // G
                    encodedPixels[d + 2] = topDownRgba[s + 0]; // R
                }
            }

            hr = frame->WritePixels(height, stride, bufferSize, encodedPixels.data());
            if (FAILED(hr))
            {
                if (errorOut) *errorOut = FormatHResultError("Frame::WritePixels", hr);
                cleanupTemp();
                return false;
            }

            hr = frame->Commit();
            if (FAILED(hr))
            {
                if (errorOut) *errorOut = FormatHResultError("Frame::Commit", hr);
                cleanupTemp();
                return false;
            }

            hr = encoder->Commit();
            if (FAILED(hr))
            {
                if (errorOut) *errorOut = FormatHResultError("Encoder::Commit", hr);
                cleanupTemp();
                return false;
            }

            hr = stream->Commit(STGC_DEFAULT);
            if (FAILED(hr))
            {
                if (errorOut) *errorOut = FormatHResultError("Stream::Commit", hr);
                cleanupTemp();
                return false;
            }
        } // Release COM pointers so the temp file handle closes before MoveFileExW.

        if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const DWORD err = GetLastError();
            if (errorOut)
            {
                char buf[128]{};
                std::snprintf(buf, sizeof(buf), "MoveFileExW failed (Win32 error %lu)", static_cast<unsigned long>(err));
                *errorOut = buf;
            }
            cleanupTemp();
            return false;
        }
        return true;
    }

    inline bool WritePngRgba(
        const std::filesystem::path& path,
        uint32_t width,
        uint32_t height,
        const uint8_t* topDownRgba,
        bool hasAlpha,
        std::string* errorOut = nullptr)
    {
        return EncodeEncodedContainer(path, GUID_ContainerFormatPng, width, height,
            topDownRgba, hasAlpha, 0, errorOut);
    }

    inline bool WriteJpegRgba(
        const std::filesystem::path& path,
        uint32_t width,
        uint32_t height,
        const uint8_t* topDownRgba,
        uint32_t quality1To100,
        std::string* errorOut = nullptr)
    {
        const int clamped = static_cast<int>(std::clamp(quality1To100, 1u, 100u));
        return EncodeEncodedContainer(path, GUID_ContainerFormatJpeg, width, height,
            topDownRgba, false, clamped, errorOut);
    }

    struct DecodedImage
    {
        uint32_t width{0};
        uint32_t height{0};
        bool hasAlphaChannel{false};
        GUID nativePixelFormat{};
        std::vector<uint8_t> rgbaTopDown;
    };

    inline bool ReadImageForTest(const std::filesystem::path& path, DecodedImage& out, std::string* errorOut = nullptr)
    {
        out = {};
        ScopedComInit com;
        if (!com.Ok())
        {
            if (errorOut) *errorOut = FormatHResultError("CoInitializeEx", com.hr);
            return false;
        }

        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory)
        {
            if (errorOut) *errorOut = FormatHResultError("CoCreateInstance(WICImagingFactory)", hr);
            return false;
        }

        ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(hr) || !decoder)
        {
            if (errorOut) *errorOut = FormatHResultError("CreateDecoderFromFilename", hr);
            return false;
        }

        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr) || !frame)
        {
            if (errorOut) *errorOut = FormatHResultError("Decoder::GetFrame", hr);
            return false;
        }

        UINT w = 0, h = 0;
        hr = frame->GetSize(&w, &h);
        if (FAILED(hr) || w == 0 || h == 0)
        {
            if (errorOut) *errorOut = FormatHResultError("Frame::GetSize", hr);
            return false;
        }

        WICPixelFormatGUID nativeFmt{};
        hr = frame->GetPixelFormat(&nativeFmt);
        if (FAILED(hr))
        {
            if (errorOut) *errorOut = FormatHResultError("Frame::GetPixelFormat", hr);
            return false;
        }

        out.width = w;
        out.height = h;
        out.nativePixelFormat = nativeFmt;
        out.hasAlphaChannel =
            IsEqualGUID(nativeFmt, GUID_WICPixelFormat32bppBGRA) ||
            IsEqualGUID(nativeFmt, GUID_WICPixelFormat32bppRGBA) ||
            IsEqualGUID(nativeFmt, GUID_WICPixelFormat32bppPBGRA) ||
            IsEqualGUID(nativeFmt, GUID_WICPixelFormat32bppPRGBA) ||
            IsEqualGUID(nativeFmt, GUID_WICPixelFormat64bppRGBA) ||
            IsEqualGUID(nativeFmt, GUID_WICPixelFormat64bppBGRA);

        ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(&converter);
        if (FAILED(hr) || !converter)
        {
            if (errorOut) *errorOut = FormatHResultError("CreateFormatConverter", hr);
            return false;
        }

        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
        {
            if (errorOut) *errorOut = FormatHResultError("FormatConverter::Initialize", hr);
            return false;
        }

        const UINT stride = w * 4u;
        const UINT totalBytes = stride * h;
        out.rgbaTopDown.resize(totalBytes);
        hr = converter->CopyPixels(nullptr, stride, totalBytes, out.rgbaTopDown.data());
        if (FAILED(hr))
        {
            if (errorOut) *errorOut = FormatHResultError("FormatConverter::CopyPixels", hr);
            return false;
        }

        // Convert BGRA -> RGBA in place.
        const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const size_t idx = i * 4u;
            std::swap(out.rgbaTopDown[idx + 0], out.rgbaTopDown[idx + 2]);
        }
        return true;
    }
} // namespace ScreenshotManager::WicWriter
