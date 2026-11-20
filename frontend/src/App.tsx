import * as React from 'react'
import {
  RiCameraLensFill,
  RiCheckFill,
  RiErrorWarningFill,
  RiFlaskFill,
  RiFolderOpenFill,
  RiFileSearchFill,
  RiImage2Fill,
  RiLoader4Fill,
  RiPaletteFill,
  RiPlugFill,
  RiRefreshFill,
  RiSparkling2Fill,
} from '@remixicon/react'
import { Alert, AlertDescription } from '@/components/ui/alert'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { Separator } from '@/components/ui/separator'
import { Slider } from '@/components/ui/slider'
import { TooltipProvider } from '@/components/ui/tooltip'
import { cn } from '@/lib/utils'
import { getScreenshotBridge, ScreenshotMockBridgeClient } from './bridge'
import { useScreenshotAppearanceEffect, useScreenshotTextInputBridge } from './hooks'
import {
  ALPHA_MODE_OPTIONS,
  MAX_CAPTURE_DIMENSION,
  RESOLUTION_PRESETS,
  formatFileSize,
  resolvePresetId,
  type AlphaMode,
  type ImageFormat,
} from './protocol'
import { screenshotStore, useScreenshotStore } from './store'

function isStandalonePreview(): boolean {
  if (import.meta.env.MODE === 'preview') return true
  if (typeof window === 'undefined') return false
  return new URLSearchParams(window.location.search).get('preview') === '1'
}

const PREVIEW_THEMES = [
  { value: 0, label: 'Dark' },
  { value: 1, label: 'Sapphire' },
  { value: 2, label: 'Paper & Ink' },
  { value: 3, label: 'Apple Light' },
  { value: 4, label: 'Apple Dark' },
]

function BootScreen({ error, canReconnect }: { error: string | null; canReconnect: boolean }) {
  return (
    <div className="flex h-full min-h-full w-full flex-col items-center justify-center gap-4 bg-background p-6 text-muted-foreground">
      {error ? (
        <div className="flex max-w-md flex-col items-center gap-3 rounded-xl border bg-card/85 p-6 text-center shadow-sm">
          <div className="flex size-11 items-center justify-center rounded-full bg-destructive/15 text-destructive">
            <RiPlugFill className="size-5" />
          </div>
          <div className="space-y-1">
            <p className="text-gaku-md font-medium text-foreground">ScreenshotManager 原生桥接不可用</p>
            <p className="text-gaku-xs leading-relaxed text-muted-foreground">{error}</p>
          </div>
          {canReconnect && (
            <Button type="button" variant="outline" size="sm" onClick={() => void screenshotStore.restart()}>
              <RiRefreshFill className="size-4" />
              重新连接
            </Button>
          )}
        </div>
      ) : (
        <div className="flex flex-col items-center gap-2.5">
          <RiLoader4Fill className="size-7 animate-spin text-primary" />
          <p className="text-gaku-base">正在连接 ScreenshotManager…</p>
        </div>
      )}
    </div>
  )
}

function PreviewToolbar() {
  const appearance = useScreenshotStore((s) => s.appearance)
  const greenScreen = useScreenshotStore((s) => s.capabilities.greenScreenDetected)
  const bridge = getScreenshotBridge()
  const mock = bridge instanceof ScreenshotMockBridgeClient ? bridge : null
  if (!mock) return null
  return (
    <div className="ml-auto flex flex-wrap items-center gap-2" data-testid="preview-toolbar">
      <label className="flex items-center gap-1.5 text-gaku-xs">
        <span className="text-muted-foreground">主题</span>
        <select
          aria-label="预览主题"
          className="h-7 rounded border bg-background px-2 text-gaku-xs text-foreground"
          value={appearance.theme}
          onChange={(e) => mock.setAppearance(Number(e.target.value), appearance.uiScale)}
        >
          {PREVIEW_THEMES.map((t) => (
            <option key={t.value} value={t.value}>
              {t.label}
            </option>
          ))}
        </select>
      </label>
      <Button
        type="button"
        variant="outline"
        size="sm"
        className="h-7 min-h-7 px-2.5 text-gaku-xs"
        onClick={() => mock.setGreenScreen(!greenScreen)}
      >
        {greenScreen ? '模拟关闭绿幕' : '模拟开启绿幕'}
      </Button>
    </div>
  )
}

export default function App() {
  const ready = useScreenshotStore((s) => s.ready)
  const connected = useScreenshotStore((s) => s.connected)
  const bridgeKind = useScreenshotStore((s) => s.bridgeKind)
  const error = useScreenshotStore((s) => s.error)
  const actionNotice = useScreenshotStore((s) => s.actionNotice)
  const busyCapture = useScreenshotStore((s) => s.busyCapture)
  const busyBrowse = useScreenshotStore((s) => s.busyBrowse)
  const config = useScreenshotStore((s) => s.config)
  const capabilities = useScreenshotStore((s) => s.capabilities)
  const captureStatus = useScreenshotStore((s) => s.captureStatus)
  const recentCaptures = useScreenshotStore((s) => s.recentCaptures)
  const appearance = useScreenshotStore((s) => s.appearance)
  const supportedMethods = useScreenshotStore((s) => s.supportedMethods)

  const [customActive, setCustomActive] = React.useState(false)
  const [customWidthText, setCustomWidthText] = React.useState(() =>
    String(config.width > 0 ? config.width : 1920),
  )
  const [customHeightText, setCustomHeightText] = React.useState(() =>
    String(config.height > 0 ? config.height : 1080),
  )
  const [dirText, setDirText] = React.useState(config.outputDir)

  React.useEffect(() => {
    void screenshotStore.start()
  }, [])

  React.useEffect(() => {
    if (config.width > 0 && config.height > 0) {
      setCustomWidthText(String(config.width))
      setCustomHeightText(String(config.height))
    }
  }, [config.width, config.height])

  React.useEffect(() => {
    setDirText(config.outputDir)
  }, [config.outputDir])

  useScreenshotAppearanceEffect(appearance)
  useScreenshotTextInputBridge(supportedMethods.includes('shell.setTextInputActive'))

  const detectedPresetId = resolvePresetId(config.width, config.height)
  const activePresetId = customActive && detectedPresetId !== 'custom' ? 'custom' : detectedPresetId

  const effectiveAlphaActive =
    config.imageFormat === 'png' &&
    (config.alphaMode === 'force_transparent' ||
      (config.alphaMode === 'auto_greenscreen' && capabilities.greenScreenDetected))

  const effectiveWidth = config.width === 0 ? capabilities.windowWidth : config.width
  const effectiveHeight = config.height === 0 ? capabilities.windowHeight : config.height
  const highResWarning = Math.max(effectiveWidth, effectiveHeight) >= 3840

  const handleApplyCustom = () => {
    const w = Number.parseInt(customWidthText.trim(), 10)
    const h = Number.parseInt(customHeightText.trim(), 10)
    void screenshotStore.setCustomResolution(w, h)
  }

  const handleApplyOutputDir = () => {
    const trimmed = dirText.trim()
    if (trimmed && trimmed !== config.outputDir) {
      void screenshotStore.setOutputDir(trimmed)
    }
  }

  const standalonePreview = isStandalonePreview()

  return (
    <TooltipProvider delayDuration={250}>
      {!ready || !connected ? (
        <BootScreen error={error ?? (ready ? '原生桥接已断开' : null)} canReconnect={ready} />
      ) : (
        <div className="flex h-full min-h-full w-full flex-col overflow-hidden bg-background text-foreground">
          {bridgeKind === 'mock' && (
            <Alert variant="warning" className="rounded-none border-x-0 border-t-0 py-2" data-testid="mock-banner">
              <RiFlaskFill className="size-4" />
              <AlertDescription className="flex flex-wrap items-center gap-2 text-gaku-xs">
                <span>ScreenshotManager 预览模式（Mock Bridge）：本地模拟，不触发游戏渲染。</span>
                {standalonePreview && <PreviewToolbar />}
              </AlertDescription>
            </Alert>
          )}

          {/* Header */}
          <header className="flex w-full shrink-0 flex-wrap items-center justify-between gap-3 border-b bg-card/70 px-5 py-3.5">
            <div className="flex items-center gap-3">
              <div className="flex size-10 items-center justify-center rounded-lg bg-primary/15 text-primary">
                <RiCameraLensFill className="size-5" />
              </div>
              <div>
                <div className="flex flex-wrap items-center gap-2">
                  <h1 className="text-gaku-title font-semibold leading-tight tracking-tight">ScreenshotManager</h1>
                  <Badge
                    variant={capabilities.cameraAvailable ? 'success' : 'muted'}
                    data-testid="runtime-status-badge"
                  >
                    {capabilities.cameraAvailable
                      ? '渲染管线就绪'
                      : capabilities.runtimeReady
                        ? '等待场景主相机'
                        : '等待 Unity 初始化'}
                  </Badge>
                  <Badge
                    variant={capabilities.greenScreenDetected ? 'success' : 'secondary'}
                    data-testid="greenscreen-badge"
                  >
                    <RiSparkling2Fill className="size-3.5" />
                    {capabilities.greenScreenDetected ? 'GakuMotion 绿幕已激活' : 'GakuMotion 绿幕未开启'}
                  </Badge>
                </div>
                <p className="mt-1 text-gaku-xs text-muted-foreground">
                  离屏多分辨率截图与绿幕透明 Alpha 抠像捕获
                  {capabilities.windowWidth > 0 && capabilities.windowHeight > 0
                    ? ` · 当前窗口 ${capabilities.windowWidth}×${capabilities.windowHeight}`
                    : ''}
                </p>
              </div>
            </div>

            <div className="flex flex-wrap items-center gap-2">
              <Button
                type="button"
                variant="outline"
                size="sm"
                onClick={() => void screenshotStore.openFolder()}
                data-testid="header-open-folder"
              >
                <RiFolderOpenFill className="size-4" />
                打开文件夹
              </Button>
              <Button
                type="button"
                disabled={busyCapture || !capabilities.cameraAvailable}
                onClick={() => void screenshotStore.takeCapture()}
                data-testid="capture-now-button"
              >
                {busyCapture || captureStatus.inFlight ? (
                  <RiLoader4Fill className="size-4 animate-spin" />
                ) : (
                  <RiCameraLensFill className="size-4" />
                )}
                立即截图 (F7)
              </Button>
            </div>
          </header>

          {/* Main scrollable body */}
          <main className="min-h-0 flex-1 overflow-y-auto px-5 py-4">
            <div className="w-full space-y-4">
              {actionNotice && (
                <Alert
                  variant={actionNotice.kind === 'error' ? 'destructive' : actionNotice.kind === 'success' ? 'info' : 'default'}
                  data-testid="action-notice"
                >
                  {actionNotice.kind === 'error' ? (
                    <RiErrorWarningFill className="size-4" />
                  ) : (
                    <RiCheckFill className="size-4" />
                  )}
                  <AlertDescription className="flex items-center justify-between gap-2 text-gaku-base">
                    <span className="break-all">{actionNotice.text}</span>
                    <button
                      type="button"
                      className="shrink-0 text-gaku-sm text-muted-foreground underline hover:text-foreground"
                      onClick={() => screenshotStore.clearNotice()}
                    >
                      关闭
                    </button>
                  </AlertDescription>
                </Alert>
              )}

              <div className="grid gap-4 lg:grid-cols-2">
                {/* Resolution Card */}
                <Card>
                  <CardHeader className="pb-3">
                    <CardTitle className="flex flex-wrap items-center justify-between gap-2 text-gaku-md">
                      <span className="inline-flex items-center gap-1.5">
                        <RiImage2Fill className="size-4 text-primary" />
                        输出分辨率
                      </span>
                      <Badge variant="outline" className="font-mono text-gaku-xs" data-testid="active-resolution-badge">
                        {config.width === 0
                          ? `窗口原生${effectiveWidth > 0 ? ` (${effectiveWidth}×${effectiveHeight})` : ''}`
                          : `${config.width}×${config.height}`}
                      </Badge>
                    </CardTitle>
                    <CardDescription className="text-gaku-xs">
                      独立离屏渲染，支持超分竖屏与 4K/8K 渲染（上限 {capabilities.maxDimension}px）
                    </CardDescription>
                  </CardHeader>
                  <CardContent className="space-y-3">
                    <div className="grid grid-cols-2 gap-2 sm:grid-cols-3">
                      {RESOLUTION_PRESETS.map((preset) => {
                        const selected = activePresetId === preset.id
                        return (
                          <Button
                            key={preset.id}
                            type="button"
                            variant={selected ? 'default' : 'outline'}
                            size="sm"
                            className="justify-center px-2.5"
                            data-testid={`preset-${preset.id}`}
                            onClick={() => {
                              setCustomActive(false)
                              void screenshotStore.applyPreset(preset)
                            }}
                          >
                            {preset.label}
                          </Button>
                        )
                      })}
                      <Button
                        type="button"
                        variant={activePresetId === 'custom' ? 'default' : 'outline'}
                        size="sm"
                        className="justify-center px-2.5 sm:col-span-3"
                        data-testid="preset-custom"
                        onClick={() => setCustomActive(true)}
                      >
                        自定义 W×H
                      </Button>
                    </div>

                    {(activePresetId === 'custom' || customActive) && (
                      <div
                        className="flex flex-wrap items-end gap-2 rounded-md border bg-muted/40 p-3"
                        data-testid="custom-resolution-panel"
                      >
                        <div className="min-w-[96px] flex-1 space-y-1.5">
                          <Label htmlFor="custom-width" className="text-muted-foreground">
                            宽度 (W)
                          </Label>
                          <Input
                            id="custom-width"
                            aria-label="自定义宽度"
                            type="number"
                            min={1}
                            max={MAX_CAPTURE_DIMENSION}
                            className="font-mono"
                            value={customWidthText}
                            onChange={(e) => setCustomWidthText(e.target.value)}
                            onKeyDown={(e) => {
                              if (e.key === 'Enter') handleApplyCustom()
                            }}
                          />
                        </div>
                        <span className="pb-2 text-gaku-base text-muted-foreground">×</span>
                        <div className="min-w-[96px] flex-1 space-y-1.5">
                          <Label htmlFor="custom-height" className="text-muted-foreground">
                            高度 (H)
                          </Label>
                          <Input
                            id="custom-height"
                            aria-label="自定义高度"
                            type="number"
                            min={1}
                            max={MAX_CAPTURE_DIMENSION}
                            className="font-mono"
                            value={customHeightText}
                            onChange={(e) => setCustomHeightText(e.target.value)}
                            onKeyDown={(e) => {
                              if (e.key === 'Enter') handleApplyCustom()
                            }}
                          />
                        </div>
                        <Button
                          type="button"
                          size="sm"
                          data-testid="apply-custom-resolution"
                          onClick={handleApplyCustom}
                        >
                          应用
                        </Button>
                      </div>
                    )}

                    {highResWarning && (
                      <p className="rounded border border-warning/40 bg-warning/10 px-3 py-2 text-gaku-xs leading-relaxed text-warning">
                        高分辨率截图（4K 以上）将临时分配大尺寸离屏 RenderTexture 与 Staging Buffer。
                      </p>
                    )}
                  </CardContent>
                </Card>

                {/* Format & Alpha Card */}
                <Card>
                  <CardHeader className="pb-3">
                    <CardTitle className="flex flex-wrap items-center justify-between gap-2 text-gaku-md">
                      <span className="inline-flex items-center gap-1.5">
                        <RiPaletteFill className="size-4 text-primary" />
                        图像格式与透明通道
                      </span>
                      <Badge variant={effectiveAlphaActive ? 'success' : 'secondary'} data-testid="effective-alpha-badge">
                        {effectiveAlphaActive ? '当前输出: 透明 RGBA' : '当前输出: 不透明 RGB'}
                      </Badge>
                    </CardTitle>
                    <CardDescription className="text-gaku-xs">
                      PNG 支持 32-bit 直通透明通道；双通道差值抠像自动适配 ACES 色调映射
                    </CardDescription>
                  </CardHeader>
                  <CardContent className="space-y-3.5">
                    <div className="space-y-1.5">
                      <Label htmlFor="alpha-mode-select">
                        透明通道策略
                      </Label>
                      <Select
                        value={config.alphaMode}
                        onValueChange={(value) => void screenshotStore.setAlphaMode(value as AlphaMode)}
                      >
                        <SelectTrigger id="alpha-mode-select" aria-label="透明通道策略">
                          <SelectValue />
                        </SelectTrigger>
                        <SelectContent>
                          {ALPHA_MODE_OPTIONS.map((opt) => (
                            <SelectItem key={opt.value} value={opt.value}>
                              {opt.label}
                            </SelectItem>
                          ))}
                        </SelectContent>
                      </Select>
                      <p className="text-gaku-xs leading-relaxed text-muted-foreground">
                        {ALPHA_MODE_OPTIONS.find((o) => o.value === config.alphaMode)?.description}
                      </p>
                    </div>

                    <Separator />

                    <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
                      <div className="space-y-1.5">
                        <Label htmlFor="format-select">
                          文件格式
                        </Label>
                        <Select
                          value={config.imageFormat}
                          onValueChange={(value) => void screenshotStore.setImageFormat(value as ImageFormat)}
                        >
                          <SelectTrigger id="format-select" aria-label="文件格式">
                            <SelectValue />
                          </SelectTrigger>
                          <SelectContent>
                            <SelectItem value="png">
                              PNG（无损 / 支持透明）
                            </SelectItem>
                            <SelectItem value="jpeg">
                              JPEG（体积小 / 不含透明）
                            </SelectItem>
                          </SelectContent>
                        </Select>
                      </div>

                      <div className="space-y-1.5">
                        <div className="flex items-center justify-between">
                          <Label className={cn(config.imageFormat !== 'jpeg' && 'text-muted-foreground')}>
                            JPEG 质量
                          </Label>
                          <span className="font-mono text-gaku-sm tabular-nums text-muted-foreground" data-testid="jpeg-quality-value">
                            {config.jpegQuality}%
                          </span>
                        </div>
                        <div className="flex h-gaku-control items-center">
                          <Slider
                            aria-label="JPEG 质量"
                            min={1}
                            max={100}
                            step={1}
                            disabled={config.imageFormat !== 'jpeg'}
                            value={[config.jpegQuality]}
                            onValueChange={(vals) => {
                              if (vals[0] !== undefined) void screenshotStore.setJpegQuality(vals[0])
                            }}
                          />
                        </div>
                      </div>
                    </div>

                    {config.imageFormat === 'jpeg' && config.alphaMode !== 'opaque' && (
                      <p className="text-gaku-xs leading-relaxed text-warning">
                        JPEG 格式不支持 Alpha 透明通道，导出时将自动输出不透明 RGB 图像。
                      </p>
                    )}
                  </CardContent>
                </Card>
              </div>

              {/* Output Folder Card */}
              <Card>
                <CardHeader className="pb-2.5">
                  <CardTitle className="text-gaku-md">保存路径</CardTitle>
                  <CardDescription className="text-gaku-xs">
                    支持按 F7 快捷键或面板按钮自动命名保存（默认写入 GakuLoader UserData/Screenshots）
                  </CardDescription>
                </CardHeader>
                <CardContent>
                  <div className="flex flex-wrap items-center gap-2">
                    <Input
                      aria-label="截图输出目录"
                      className="min-w-[220px] flex-1 font-mono"
                      value={dirText}
                      onChange={(e) => setDirText(e.target.value)}
                      onBlur={handleApplyOutputDir}
                      onKeyDown={(e) => {
                        if (e.key === 'Enter') handleApplyOutputDir()
                      }}
                    />
                    <Button
                      type="button"
                      variant="outline"
                      size="sm"
                      disabled={busyBrowse}
                      data-testid="browse-folder-button"
                      onClick={() => void screenshotStore.browseFolder()}
                    >
                      <RiFileSearchFill className="size-4" />
                      浏览…
                    </Button>
                    <Button
                      type="button"
                      variant="outline"
                      size="sm"
                      data-testid="open-folder-button"
                      onClick={() => void screenshotStore.openFolder()}
                    >
                      <RiFolderOpenFill className="size-4" />
                      打开目录
                    </Button>
                  </div>
                </CardContent>
              </Card>

              {/* Recent Captures List */}
              <Card>
                <CardHeader className="pb-2.5">
                  <CardTitle className="flex items-center justify-between text-gaku-md">
                    <span>最近截图记录</span>
                    <span className="text-gaku-xs font-normal text-muted-foreground">
                      共 {recentCaptures.length} 条
                    </span>
                  </CardTitle>
                </CardHeader>
                <CardContent>
                  {recentCaptures.length === 0 ? (
                    <div className="rounded-md border border-dashed px-4 py-6 text-center text-gaku-base text-muted-foreground">
                      暂无截图记录。点击右上角“立即截图 (F7)”或在游戏画面中按下 F7 开始捕获。
                    </div>
                  ) : (
                    <div className="divide-y rounded-md border" data-testid="recent-captures-list">
                      {recentCaptures.map((item) => (
                        <div
                          key={item.id}
                          className="flex flex-wrap items-center justify-between gap-3 px-3.5 py-2.5 text-gaku-base"
                        >
                          <div className="min-w-0 flex-1 space-y-0.5">
                            <div className="break-all font-mono text-gaku-base font-medium text-foreground">{item.fileName}</div>
                            <div className="break-all font-mono text-gaku-xs text-muted-foreground">{item.path}</div>
                          </div>
                          <div className="flex shrink-0 flex-wrap items-center gap-2">
                            <Badge variant="outline" className="font-mono text-gaku-micro">
                              {item.width}×{item.height}
                            </Badge>
                            <Badge variant={item.hasAlpha ? 'success' : 'secondary'} className="text-gaku-micro">
                              {item.imageFormat.toUpperCase()} {item.hasAlpha ? 'RGBA' : 'RGB'}
                            </Badge>
                            <span className="min-w-[4.5rem] text-right font-mono text-gaku-xs text-muted-foreground tabular-nums">
                              {formatFileSize(item.sizeBytes)}
                            </span>
                            <span className="hidden text-gaku-xs text-muted-foreground sm:inline">
                              {item.timestamp}
                            </span>
                          </div>
                        </div>
                      ))}
                    </div>
                  )}
                </CardContent>
              </Card>
            </div>
          </main>
        </div>
      )}
    </TooltipProvider>
  )
}
