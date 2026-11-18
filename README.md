# ScreenshotManager

Independent GakuLoader v1 native plugin providing multi-resolution offscreen screenshot capture and ACES-calibrated Green Screen alpha extraction.

## Features

- **Multi-Resolution Offscreen Rendering**: Renders `Camera.main` directly to temporary `RenderTexture` targets from native window size up to 4K (`3840x2160`), portrait 4K (`2160x3840`), and custom dimensions (capped at `7680px`), saving/restoring `targetTexture`, `aspect`, `clearFlags`, `backgroundColor`, and URP `renderScale`.
- **D3D11 GPU Readback + Fallback**: Reads back pixels via `Texture.GetNativeTexturePtr()` and a pooled `ID3D11Texture2D` staging buffer, with a `Texture2D.ReadPixels` fallback.
- **ACES-Calibrated Alpha Matte (`src/AlphaMatte.hpp`)**: Dual-pass black/white difference matting that calibrates tonemapped white ceiling delta from background samples and recovers straight unpremultiplied RGB without green/black/white fringes.
- **Atomic WIC Image Encoding (`src/WicImageWriter.hpp`)**: Background-threaded PNG (`32bppBGRA` / `24bppBGR`) and JPEG (`24bppBGR` + `ImageQuality`) encoding with atomic temp-file replacement.
- **Public & Host Services**:
  - `screenshotmanager.capture/v1` (`include/GakuScreenshot.h`) for async ticketed file captures and synchronous `captureFrameImmediate` integration (e.g. `VideoExporter`).
  - `screenshotmanager.web-panel/v1` (`include/GakuWebPanel.h`) WebView2 control panel (`ScreenshotManager.UI`).
  - Optional foreground hotkey `take-screenshot` (`VK_F11`).
  - Lazy integration with `gakumotion.stage-state/v1` (`include/GakuMotionStageState.h`) to detect active GakuMotion Green Screen mode.

## Build & Offline Verification

```powershell
# Build native DLL, stage ScreenshotManager.UI, and run C++ unit & ABI tests
powershell -NoProfile -File build.ps1 -Target All

# Frontend typecheck and Vitest suite
Push-Location frontend
npm run typecheck
npm test
Pop-Location
```

Outputs are written locally under `build/native/`:
- `build/native/ScreenshotManager.dll`
- `build/native/ScreenshotManager.UI/`
