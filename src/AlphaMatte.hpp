// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ScreenshotManager::AlphaMatte
{
    inline bool ValidateDimensions(uint32_t width, uint32_t height, size_t& pixelCount) noexcept
    {
        if (width == 0 || height == 0) return false;
        const size_t w = static_cast<size_t>(width);
        const size_t h = static_cast<size_t>(height);
        if (w > (SIZE_MAX / 4u) / h) return false;
        pixelCount = w * h;
        return true;
    }

    inline uint8_t ClampByte(double normalized) noexcept
    {
        const double clamped = std::clamp(normalized, 0.0, 1.0);
        const long rounded = std::lround(clamped * 255.0);
        return static_cast<uint8_t>(std::clamp(rounded, 0L, 255L));
    }

    namespace detail
    {
        template <typename PixelFn>
        inline void TransformRgbaGrid(uint32_t width, uint32_t height,
            bool flipVertical, bool flipHorizontal, PixelFn&& fn) noexcept
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                const uint32_t srcY = flipVertical ? (height - 1u - y) : y;
                const size_t srcRow = static_cast<size_t>(srcY) * width * 4u;
                const size_t dstRow = static_cast<size_t>(y) * width * 4u;
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t srcX = flipHorizontal ? (width - 1u - x) : x;
                    const size_t s = srcRow + static_cast<size_t>(srcX) * 4u;
                    const size_t d = dstRow + static_cast<size_t>(x) * 4u;
                    fn(s, d);
                }
            }
        }
    } // namespace detail

    // Copies RGBA8 pixels into top-down RGBA8 with opaque alpha (A = 255).
    // Safe when srcRgba == outRgba only if !flipVertical && !flipHorizontal.
    inline bool CopyOpaqueToRgba(uint32_t width, uint32_t height, const uint8_t* srcRgba,
        bool flipVertical, uint8_t* outRgba, bool flipHorizontal = false) noexcept
    {
        size_t pixelCount = 0;
        if (!srcRgba || !outRgba || !ValidateDimensions(width, height, pixelCount)) return false;

        detail::TransformRgbaGrid(width, height, flipVertical, flipHorizontal,
            [&](size_t s, size_t d) noexcept {
                outRgba[d + 0] = srcRgba[s + 0];
                outRgba[d + 1] = srcRgba[s + 1];
                outRgba[d + 2] = srcRgba[s + 2];
                outRgba[d + 3] = 255u;
            });
        return true;
    }

    // Converts RGBA8 <-> BGRA8 (safe in-place when src == dst).
    inline bool RgbaToBgra(uint32_t width, uint32_t height, const uint8_t* srcRgba, uint8_t* outBgra) noexcept
    {
        size_t pixelCount = 0;
        if (!srcRgba || !outBgra || !ValidateDimensions(width, height, pixelCount)) return false;
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const size_t idx = i * 4u;
            const uint8_t r = srcRgba[idx + 0];
            const uint8_t g = srcRgba[idx + 1];
            const uint8_t b = srcRgba[idx + 2];
            const uint8_t a = srcRgba[idx + 3];
            outBgra[idx + 0] = b;
            outBgra[idx + 1] = g;
            outBgra[idx + 2] = r;
            outBgra[idx + 3] = a;
        }
        return true;
    }

    // Unpremultiplies hardware-alpha RGBA8 pixels into straight top-down RGBA8.
    // Safe when srcPixels == outRgba only if !flipVertical && !flipHorizontal.
    inline bool UnpremultiplyAndConvertToRgba(uint32_t width, uint32_t height, const uint8_t* srcPixels,
        bool flipVertical, uint8_t* outRgba, bool flipHorizontal = false) noexcept
    {
        size_t pixelCount = 0;
        if (!srcPixels || !outRgba || !ValidateDimensions(width, height, pixelCount)) return false;

        detail::TransformRgbaGrid(width, height, flipVertical, flipHorizontal,
            [&](size_t s, size_t d) noexcept {
                const uint8_t r = srcPixels[s + 0];
                const uint8_t g = srcPixels[s + 1];
                const uint8_t b = srcPixels[s + 2];
                const uint8_t a = srcPixels[s + 3];
                if (a == 0u)
                {
                    outRgba[d + 0] = 0u;
                    outRgba[d + 1] = 0u;
                    outRgba[d + 2] = 0u;
                    outRgba[d + 3] = 0u;
                }
                else if (a == 255u)
                {
                    outRgba[d + 0] = r;
                    outRgba[d + 1] = g;
                    outRgba[d + 2] = b;
                    outRgba[d + 3] = 255u;
                }
                else
                {
                    // (r / 255.0) / (a / 255.0) == r / a in normalized [0..1].
                    const double invA = 1.0 / static_cast<double>(a);
                    outRgba[d + 0] = ClampByte(static_cast<double>(r) * invA);
                    outRgba[d + 1] = ClampByte(static_cast<double>(g) * invA);
                    outRgba[d + 2] = ClampByte(static_cast<double>(b) * invA);
                    outRgba[d + 3] = a;
                }
            });
        return true;
    }

    // Dual-pass black/white difference matting with ACES/Neutral tonemapped white calibration.
    // Calibrates ceilDelta from background pixels where blackRgba is near (0,0,0),
    // then computes per-pixel alpha and recovers straight unpremultiplied foreground RGB from blackRgba.
    inline bool ExtractAlphaDifferenceMatte(uint32_t width, uint32_t height,
        const uint8_t* blackRgba, const uint8_t* whiteRgba,
        uint8_t* outStraightRgba, bool flipVertical, bool flipHorizontal = false) noexcept
    {
        size_t pixelCount = 0;
        if (!blackRgba || !whiteRgba || !outStraightRgba || !ValidateDimensions(width, height, pixelCount))
            return false;

        constexpr uint8_t kNearBlackThreshold = 8u;
        constexpr double kMinCeilDelta = 16.0 / 255.0;

        double maxObservedDelta = 0.0;
        bool foundBackgroundSample = false;

        for (size_t i = 0; i < pixelCount; ++i)
        {
            const size_t idx = i * 4u;
            const uint8_t br = blackRgba[idx + 0];
            const uint8_t bg = blackRgba[idx + 1];
            const uint8_t bb = blackRgba[idx + 2];
            if (br <= kNearBlackThreshold && bg <= kNearBlackThreshold && bb <= kNearBlackThreshold)
            {
                const int dr = static_cast<int>(whiteRgba[idx + 0]) - static_cast<int>(br);
                const int dg = static_cast<int>(whiteRgba[idx + 1]) - static_cast<int>(bg);
                const int db = static_cast<int>(whiteRgba[idx + 2]) - static_cast<int>(bb);
                const int sum = dr + dg + db;
                if (sum > 0)
                {
                    const double meanDelta = static_cast<double>(sum) / (3.0 * 255.0);
                    if (!foundBackgroundSample || meanDelta > maxObservedDelta)
                    {
                        maxObservedDelta = meanDelta;
                        foundBackgroundSample = true;
                    }
                }
            }
        }

        const double ceilDelta = foundBackgroundSample
            ? std::clamp(maxObservedDelta, kMinCeilDelta, 1.0)
            : 1.0;
        const double invThreeCeil = 1.0 / (3.0 * ceilDelta);

        constexpr double kLowSnap = 2.0 / 255.0;
        constexpr double kHighSnap = 253.0 / 255.0;
        constexpr double kMinDenom = 1.0 / 255.0;

        detail::TransformRgbaGrid(width, height, flipVertical, flipHorizontal,
            [&](size_t s, size_t d) noexcept {
                const double br = static_cast<double>(blackRgba[s + 0]) / 255.0;
                const double bg = static_cast<double>(blackRgba[s + 1]) / 255.0;
                const double bb = static_cast<double>(blackRgba[s + 2]) / 255.0;

                const double wr = static_cast<double>(whiteRgba[s + 0]) / 255.0;
                const double wg = static_cast<double>(whiteRgba[s + 1]) / 255.0;
                const double wb = static_cast<double>(whiteRgba[s + 2]) / 255.0;

                const double deltaSum = (wr - br) + (wg - bg) + (wb - bb);
                double alpha = std::clamp(1.0 - deltaSum * invThreeCeil, 0.0, 1.0);

                if (alpha < kLowSnap)
                {
                    outStraightRgba[d + 0] = 0u;
                    outStraightRgba[d + 1] = 0u;
                    outStraightRgba[d + 2] = 0u;
                    outStraightRgba[d + 3] = 0u;
                    return;
                }

                uint8_t outAlpha = 255u;
                if (alpha > kHighSnap)
                {
                    alpha = 1.0;
                    outAlpha = 255u;
                }
                else
                {
                    outAlpha = ClampByte(alpha);
                }

                const double denom = std::max(alpha, kMinDenom);
                outStraightRgba[d + 0] = ClampByte(br / denom);
                outStraightRgba[d + 1] = ClampByte(bg / denom);
                outStraightRgba[d + 2] = ClampByte(bb / denom);
                outStraightRgba[d + 3] = outAlpha;
            });
        return true;
    }
} // namespace ScreenshotManager::AlphaMatte
