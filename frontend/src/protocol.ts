import type { RequestOptions } from '@/bridge/protocol'

export type ImageFormat = 'png' | 'jpeg'
export type AlphaMode = 'auto_greenscreen' | 'force_transparent' | 'opaque'

export type ResolutionPresetId =
  | 'native'
  | '1080p'
  | '2k'
  | '4k'
  | 'portrait_1080p'
  | 'portrait_4k'
  | 'custom'

export interface ResolutionPreset {
  id: Exclude<ResolutionPresetId, 'custom'>
  label: string
  width: number
  height: number
}

export const RESOLUTION_PRESETS: readonly ResolutionPreset[] = [
  { id: 'native', label: '窗口原生', width: 0, height: 0 },
  { id: '1080p', label: '1080p 1920×1080', width: 1920, height: 1080 },
  { id: '2k', label: '2K 2560×1440', width: 2560, height: 1440 },
  { id: '4k', label: '4K 3840×2160', width: 3840, height: 2160 },
  { id: 'portrait_1080p', label: '竖屏 1080p', width: 1080, height: 1920 },
  { id: 'portrait_4k', label: '竖屏 4K', width: 2160, height: 3840 },
]

export const ALPHA_MODE_OPTIONS: ReadonlyArray<{
  value: AlphaMode
  label: string
  description: string
}> = [
  {
    value: 'auto_greenscreen',
    label: '绿幕自动透明 RGBA',
    description: '检测到 GakuMotion 绿幕开启时自动执行双通道抠像输出透明通道',
  },
  {
    value: 'force_transparent',
    label: '强制透明 RGBA',
    description: '始终执行黑白双通道差值抠像，保留发丝与半透明光效 Alpha',
  },
  {
    value: 'opaque',
    label: '不透明 RGB',
    description: '单次离屏渲染，输出不透明图像（性能最高）',
  },
]

export const MAX_CAPTURE_DIMENSION = 7680

export interface ScreenshotConfig {
  width: number
  height: number
  imageFormat: ImageFormat
  jpegQuality: number
  alphaMode: AlphaMode
  outputDir: string
}

export interface ScreenshotCapabilities {
  runtimeReady: boolean
  cameraAvailable: boolean
  greenScreenDetected: boolean
  maxDimension: number
  windowWidth: number
  windowHeight: number
}

export interface CaptureStatus {
  inFlight: boolean
  pendingCount: number
  lastError: string
}

export interface RecentCapture {
  id: number
  path: string
  fileName: string
  width: number
  height: number
  imageFormat: ImageFormat
  hasAlpha: boolean
  sizeBytes: number
  timestamp: string
}

export interface ScreenshotAppearance {
  theme: number
  uiScale: number
}

export interface ScreenshotSnapshot {
  config: ScreenshotConfig
  capabilities: ScreenshotCapabilities
  captureStatus: CaptureStatus
  recentCaptures: RecentCapture[]
  appearance: ScreenshotAppearance
}

export interface CaptureCompletedEvent {
  ticket: number
  ok: boolean
  path: string
  fileName: string
  width: number
  height: number
  imageFormat: ImageFormat
  hasAlpha: boolean
  sizeBytes: number
  timestamp: string
  error: string
}

export interface FolderBrowsedEvent {
  cancelled: boolean
  path: string
  error: string
}

export type ScreenshotMethods = {
  'bridge.hello': {
    params: Record<string, never>
    result: { protocol: number; sessionId: number; pluginVersion: string; capabilities: string[] }
  }
  'bridge.ping': {
    params: Record<string, never>
    result: { pong: boolean }
  }
  'state.get': {
    params: Record<string, never>
    result: ScreenshotSnapshot
  }
  'capture.take': {
    params: Partial<ScreenshotConfig>
    result: { ticket: number; outputPath: string }
  }
  'config.set': {
    params: Partial<ScreenshotConfig> | { config: Partial<ScreenshotConfig> }
    result: { config: ScreenshotConfig }
  }
  'folder.open': {
    params: Record<string, never>
    result: { path: string }
  }
  'folder.browse': {
    params: Record<string, never>
    result: { pending: boolean; cancelled: boolean; path: string }
  }
  'appearance.get': {
    params: Record<string, never>
    result: ScreenshotAppearance
  }
  'shell.setTextInputActive': {
    params: { active: boolean }
    result: Record<string, never>
  }
}

export type ScreenshotEvents = {
  'state.changed': ScreenshotSnapshot
  'capture.completed': CaptureCompletedEvent
  'folder.browsed': FolderBrowsedEvent
  'appearance.changed': ScreenshotAppearance
  'bridge.closed': { reason: string }
}

export type ScreenshotMethodName = keyof ScreenshotMethods & string
export type ScreenshotEventName = keyof ScreenshotEvents & string

export interface ScreenshotBridgeClient {
  readonly kind: 'webview2' | 'mock'
  request<M extends ScreenshotMethodName>(
    method: M,
    params?: ScreenshotMethods[M]['params'],
    options?: RequestOptions,
  ): Promise<ScreenshotMethods[M]['result']>
  on<E extends ScreenshotEventName>(
    event: E,
    handler: (payload: ScreenshotEvents[E]) => void,
  ): () => void
  close(): void
}

export function resolvePresetId(width: number, height: number): ResolutionPresetId {
  if (width === 0 && height === 0) return 'native'
  for (const preset of RESOLUTION_PRESETS) {
    if (preset.width === width && preset.height === height) return preset.id
  }
  return 'custom'
}

export function formatFileSize(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0 B'
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`
}
