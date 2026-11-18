import { useSyncExternalStore } from 'react'
import { getScreenshotBridge, setScreenshotBridge } from './bridge'
import {
  MAX_CAPTURE_DIMENSION,
  type AlphaMode,
  type CaptureCompletedEvent,
  type CaptureStatus,
  type ImageFormat,
  type RecentCapture,
  type ResolutionPreset,
  type ScreenshotAppearance,
  type ScreenshotBridgeClient,
  type ScreenshotCapabilities,
  type ScreenshotConfig,
  type ScreenshotSnapshot,
} from './protocol'

export interface StoreState {
  ready: boolean
  connected: boolean
  bridgeKind: 'webview2' | 'mock' | null
  error: string | null
  actionNotice: { kind: 'info' | 'success' | 'error'; text: string } | null
  busyCapture: boolean
  busyBrowse: boolean
  config: ScreenshotConfig
  capabilities: ScreenshotCapabilities
  captureStatus: CaptureStatus
  recentCaptures: RecentCapture[]
  appearance: ScreenshotAppearance
  supportedMethods: string[]
}

function createInitialState(): StoreState {
  return {
    ready: false,
    connected: false,
    bridgeKind: null,
    error: null,
    actionNotice: null,
    busyCapture: false,
    busyBrowse: false,
    config: {
      width: 0,
      height: 0,
      imageFormat: 'png',
      jpegQuality: 92,
      alphaMode: 'auto_greenscreen',
      outputDir: '',
    },
    capabilities: {
      runtimeReady: false,
      cameraAvailable: false,
      greenScreenDetected: false,
      maxDimension: MAX_CAPTURE_DIMENSION,
      windowWidth: 0,
      windowHeight: 0,
    },
    captureStatus: {
      inFlight: false,
      pendingCount: 0,
      lastError: '',
    },
    recentCaptures: [],
    appearance: {
      theme: 0,
      uiScale: 1,
    },
    supportedMethods: [],
  }
}

class ScreenshotStore {
  private state: StoreState = createInitialState()
  private readonly listeners = new Set<() => void>()
  private unsubscribers: Array<() => void> = []
  private startPromise: Promise<void> | null = null

  getState(): StoreState {
    return this.state
  }

  subscribe = (listener: () => void): (() => void) => {
    this.listeners.add(listener)
    return () => {
      this.listeners.delete(listener)
    }
  }

  reset(): void {
    for (const unsub of this.unsubscribers) unsub()
    this.unsubscribers = []
    this.startPromise = null
    this.state = createInitialState()
    this.notify()
  }

  async start(): Promise<void> {
    if (this.state.ready && this.state.connected) return
    if (this.startPromise) return this.startPromise
    this.startPromise = this.connect()
    try {
      await this.startPromise
    } finally {
      this.startPromise = null
    }
  }

  async restart(): Promise<void> {
    const old = getScreenshotBridge()
    old?.close()
    setScreenshotBridge(null)
    this.reset()
    await this.start()
  }

  async resync(): Promise<void> {
    const bridge = getScreenshotBridge()
    if (!bridge) return
    try {
      const snap = await bridge.request('state.get', {})
      this.applySnapshot(snap)
    } catch (err) {
      this.setError(err instanceof Error ? err.message : String(err))
    }
  }

  async updateConfig(patch: Partial<ScreenshotConfig>): Promise<boolean> {
    const bridge = getScreenshotBridge()
    if (!bridge) return false
    try {
      const res = await bridge.request('config.set', patch)
      this.setState({
        config: res.config,
        actionNotice: null,
      })
      return true
    } catch (err) {
      this.setActionError(err instanceof Error ? err.message : String(err))
      return false
    }
  }

  async applyPreset(preset: ResolutionPreset): Promise<boolean> {
    return this.updateConfig({ width: preset.width, height: preset.height })
  }

  async setCustomResolution(width: number, height: number): Promise<boolean> {
    if (
      !Number.isInteger(width) ||
      !Number.isInteger(height) ||
      width < 1 ||
      height < 1 ||
      width > this.state.capabilities.maxDimension ||
      height > this.state.capabilities.maxDimension
    ) {
      this.setActionError(`自定义分辨率必须在 1 × 1 至 ${this.state.capabilities.maxDimension} × ${this.state.capabilities.maxDimension} 之间`)
      return false
    }
    return this.updateConfig({ width, height })
  }

  async setImageFormat(imageFormat: ImageFormat): Promise<boolean> {
    return this.updateConfig({ imageFormat })
  }

  async setJpegQuality(jpegQuality: number): Promise<boolean> {
    return this.updateConfig({ jpegQuality: Math.min(100, Math.max(1, Math.round(jpegQuality))) })
  }

  async setAlphaMode(alphaMode: AlphaMode): Promise<boolean> {
    return this.updateConfig({ alphaMode })
  }

  async setOutputDir(outputDir: string): Promise<boolean> {
    return this.updateConfig({ outputDir })
  }

  async takeCapture(): Promise<boolean> {
    const bridge = getScreenshotBridge()
    if (!bridge || this.state.busyCapture) return false
    this.setState({ busyCapture: true, actionNotice: null })
    try {
      const res = await bridge.request('capture.take', {})
      this.setState({
        busyCapture: false,
        actionNotice: this.state.actionNotice ?? { kind: 'info', text: `已提交截图任务 #${res.ticket}` },
      })
      return true
    } catch (err) {
      this.setState({
        busyCapture: false,
        actionNotice: { kind: 'error', text: err instanceof Error ? err.message : String(err) },
      })
      return false
    }
  }

  async openFolder(): Promise<boolean> {
    const bridge = getScreenshotBridge()
    if (!bridge) return false
    try {
      const res = await bridge.request('folder.open', {})
      this.setState({
        actionNotice: { kind: 'info', text: `已打开保存目录: ${res.path}` },
      })
      return true
    } catch (err) {
      this.setActionError(err instanceof Error ? err.message : String(err))
      return false
    }
  }

  async browseFolder(): Promise<boolean> {
    const bridge = getScreenshotBridge()
    if (!bridge || this.state.busyBrowse) return false
    this.setState({ busyBrowse: true, actionNotice: null })
    try {
      const res = await bridge.request('folder.browse', {})
      if (res.pending) {
        return true
      }
      this.setState({
        busyBrowse: false,
        config: !res.cancelled && res.path ? { ...this.state.config, outputDir: res.path } : this.state.config,
        actionNotice:
          !res.cancelled && res.path ? { kind: 'success', text: `保存目录已更新: ${res.path}` } : null,
      })
      return true
    } catch (err) {
      this.setState({
        busyBrowse: false,
        actionNotice: { kind: 'error', text: err instanceof Error ? err.message : String(err) },
      })
      return false
    }
  }

  clearNotice(): void {
    this.setState({ actionNotice: null })
  }

  private async connect(): Promise<void> {
    const bridge = getScreenshotBridge()
    if (!bridge) {
      this.setState({
        ready: true,
        connected: false,
        bridgeKind: null,
        error: 'window.chrome.webview 不可用，请在 GakuLoader 中打开此面板。',
      })
      return
    }

    try {
      this.attachEvents(bridge)
      const hello = await bridge.request('bridge.hello', {})
      const snap = await bridge.request('state.get', {})
      this.setState({
        ready: true,
        connected: true,
        bridgeKind: bridge.kind,
        error: null,
        supportedMethods: hello.capabilities ?? [],
      })
      this.applySnapshot(snap)
    } catch (err) {
      this.setState({
        ready: true,
        connected: false,
        bridgeKind: bridge.kind,
        error: err instanceof Error ? err.message : String(err),
      })
    }
  }

  private attachEvents(bridge: ScreenshotBridgeClient): void {
    for (const unsub of this.unsubscribers) unsub()
    this.unsubscribers = [
      bridge.on('state.changed', (snap) => {
        this.applySnapshot(snap)
      }),
      bridge.on('appearance.changed', (appearance) => {
        this.setState({ appearance })
      }),
      bridge.on('capture.completed', (notice: CaptureCompletedEvent) => {
        this.setState({
          actionNotice: notice.ok
            ? {
                kind: 'success',
                text: `截图已保存 (${notice.width}×${notice.height}${notice.hasAlpha ? ' RGBA' : ''}): ${notice.fileName}`,
              }
            : {
                kind: 'error',
                text: `截图失败: ${notice.error || '未知错误'}`,
              },
        })
      }),
      bridge.on('folder.browsed', (result) => {
        this.setState({
          busyBrowse: false,
          config: !result.cancelled && result.path ? { ...this.state.config, outputDir: result.path } : this.state.config,
          actionNotice: result.error
            ? { kind: 'error', text: result.error }
            : !result.cancelled && result.path
              ? { kind: 'success', text: `保存目录已更新: ${result.path}` }
              : null,
        })
      }),
      bridge.on('bridge.closed', () => {
        this.setState({
          connected: false,
          error: '原生插件会话已断开',
        })
      }),
    ]
  }

  private applySnapshot(snap: ScreenshotSnapshot): void {
    this.setState({
      config: snap.config,
      capabilities: snap.capabilities,
      captureStatus: snap.captureStatus,
      recentCaptures: Array.isArray(snap.recentCaptures) ? snap.recentCaptures : [],
      appearance: snap.appearance ?? this.state.appearance,
    })
  }

  private setActionError(text: string): void {
    this.setState({ actionNotice: { kind: 'error', text } })
  }

  private setError(error: string): void {
    this.setState({ error })
  }

  private setState(partial: Partial<StoreState>): void {
    this.state = { ...this.state, ...partial }
    this.notify()
  }

  private notify(): void {
    for (const listener of this.listeners) listener()
  }
}

export const screenshotStore = new ScreenshotStore()

export function useScreenshotStore<T>(selector: (state: StoreState) => T): T {
  return useSyncExternalStore(
    screenshotStore.subscribe,
    () => selector(screenshotStore.getState()),
    () => selector(screenshotStore.getState()),
  )
}
