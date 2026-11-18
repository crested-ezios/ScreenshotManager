import { BridgeError, type RequestOptions } from '@/bridge/protocol'
import {
  MAX_CAPTURE_DIMENSION,
  type CaptureCompletedEvent,
  type ScreenshotAppearance,
  type ScreenshotBridgeClient,
  type ScreenshotConfig,
  type ScreenshotEventName,
  type ScreenshotEvents,
  type ScreenshotMethodName,
  type ScreenshotMethods,
  type ScreenshotSnapshot,
} from './protocol'

const SUPPORTED_METHODS: string[] = [
  'bridge.hello',
  'bridge.ping',
  'state.get',
  'capture.take',
  'config.set',
  'folder.open',
  'folder.browse',
  'appearance.get',
  'shell.setTextInputActive',
]

export class ScreenshotMockBridgeClient implements ScreenshotBridgeClient {
  readonly kind = 'mock' as const
  private closed = false
  private nextTicket = 1
  private nextCaptureId = 1
  private readonly handlers = new Map<ScreenshotEventName, Set<(payload: never) => void>>()
  private snapshot: ScreenshotSnapshot = {
    config: {
      width: 3840,
      height: 2160,
      imageFormat: 'png',
      jpegQuality: 92,
      alphaMode: 'auto_greenscreen',
      outputDir: 'C:\\Users\\Preview\\AppData\\Local\\GakuLoader\\UserData\\screenshotmanager\\Screenshots',
    },
    capabilities: {
      runtimeReady: true,
      cameraAvailable: true,
      greenScreenDetected: true,
      maxDimension: MAX_CAPTURE_DIMENSION,
      windowWidth: 1920,
      windowHeight: 1080,
    },
    captureStatus: {
      inFlight: false,
      pendingCount: 0,
      lastError: '',
    },
    recentCaptures: [
      {
        id: 1,
        path: 'C:\\Users\\Preview\\AppData\\Local\\GakuLoader\\UserData\\screenshotmanager\\Screenshots\\Gakumas_20261118_201500_000.png',
        fileName: 'Gakumas_20261118_201500_000.png',
        width: 3840,
        height: 2160,
        imageFormat: 'png',
        hasAlpha: true,
        sizeBytes: 6_420_480,
        timestamp: '2026-11-18 20:15:00',
      },
    ],
    appearance: {
      theme: 0,
      uiScale: 1,
    },
  }

  constructor(initial?: Partial<ScreenshotSnapshot>) {
    if (initial) {
      this.snapshot = {
        ...this.snapshot,
        ...initial,
        config: { ...this.snapshot.config, ...initial.config },
        capabilities: { ...this.snapshot.capabilities, ...initial.capabilities },
        captureStatus: { ...this.snapshot.captureStatus, ...initial.captureStatus },
        appearance: { ...this.snapshot.appearance, ...initial.appearance },
        recentCaptures: initial.recentCaptures ? [...initial.recentCaptures] : [...this.snapshot.recentCaptures],
      }
      this.nextCaptureId = this.snapshot.recentCaptures.length + 1
    }
  }

  async request<M extends ScreenshotMethodName>(
    method: M,
    params?: ScreenshotMethods[M]['params'],
    _options?: RequestOptions,
  ): Promise<ScreenshotMethods[M]['result']> {
    if (this.closed) throw new BridgeError('closed', 'Mock bridge is closed')

    switch (method) {
      case 'bridge.hello':
        return {
          protocol: 1,
          sessionId: 1,
          pluginVersion: '0.1.0-mock',
          capabilities: [...SUPPORTED_METHODS],
        } as ScreenshotMethods[M]['result']

      case 'bridge.ping':
        return { pong: true } as ScreenshotMethods[M]['result']

      case 'state.get':
        return structuredClone(this.snapshot) as ScreenshotMethods[M]['result']

      case 'appearance.get':
        return { ...this.snapshot.appearance } as ScreenshotMethods[M]['result']

      case 'shell.setTextInputActive':
        return {} as ScreenshotMethods[M]['result']

      case 'config.set': {
        const raw = (params ?? {}) as { config?: Partial<ScreenshotConfig> } & Partial<ScreenshotConfig>
        const patch = raw.config && typeof raw.config === 'object' ? raw.config : raw
        const next: ScreenshotConfig = { ...this.snapshot.config }

        if (typeof patch.width === 'number') next.width = Math.round(patch.width)
        if (typeof patch.height === 'number') next.height = Math.round(patch.height)
        const validRes =
          (next.width === 0 && next.height === 0) ||
          (next.width >= 1 && next.width <= MAX_CAPTURE_DIMENSION && next.height >= 1 && next.height <= MAX_CAPTURE_DIMENSION)
        if (!validRes) throw new BridgeError('invalid_params', '分辨率必须为 0×0（原生）或 1..7680')

        if (patch.imageFormat === 'png' || patch.imageFormat === 'jpeg') next.imageFormat = patch.imageFormat
        if (typeof patch.jpegQuality === 'number') {
          if (patch.jpegQuality < 1 || patch.jpegQuality > 100) {
            throw new BridgeError('invalid_params', 'JPEG 质量必须在 1..100 之间')
          }
          next.jpegQuality = Math.round(patch.jpegQuality)
        }
        if (
          patch.alphaMode === 'auto_greenscreen' ||
          patch.alphaMode === 'force_transparent' ||
          patch.alphaMode === 'opaque'
        ) {
          next.alphaMode = patch.alphaMode
        }
        if (typeof patch.outputDir === 'string' && patch.outputDir.trim().length > 0) {
          next.outputDir = patch.outputDir
        }

        this.snapshot.config = next
        this.emit('state.changed', structuredClone(this.snapshot))
        return { config: { ...next } } as ScreenshotMethods[M]['result']
      }

      case 'capture.take': {
        if (!this.snapshot.capabilities.runtimeReady || !this.snapshot.capabilities.cameraAvailable) {
          throw new BridgeError('not_ready', '游戏渲染尚未就绪')
        }
        const cfg = this.snapshot.config
        const w = cfg.width === 0 ? this.snapshot.capabilities.windowWidth : cfg.width
        const h = cfg.height === 0 ? this.snapshot.capabilities.windowHeight : cfg.height
        const hasAlpha =
          cfg.imageFormat === 'png' &&
          (cfg.alphaMode === 'force_transparent' ||
            (cfg.alphaMode === 'auto_greenscreen' && this.snapshot.capabilities.greenScreenDetected))
        const ext = cfg.imageFormat === 'jpeg' ? 'jpg' : 'png'
        const ticket = this.nextTicket++
        const fileName = `Gakumas_mock_${String(ticket).padStart(4, '0')}.${ext}`
        const outputPath = `${cfg.outputDir.replace(/[\\/]+$/, '')}\\${fileName}`
        const id = ++this.nextCaptureId
        const entry = {
          id,
          path: outputPath,
          fileName,
          width: w,
          height: h,
          imageFormat: cfg.imageFormat,
          hasAlpha,
          sizeBytes: cfg.imageFormat === 'jpeg' ? w * h : w * h * (hasAlpha ? 3 : 2),
          timestamp: '2026-11-18 20:30:00',
        }
        this.snapshot.recentCaptures = [entry, ...this.snapshot.recentCaptures].slice(0, 20)
        const notice: CaptureCompletedEvent = {
          ticket,
          ok: true,
          path: outputPath,
          fileName,
          width: w,
          height: h,
          imageFormat: cfg.imageFormat,
          hasAlpha,
          sizeBytes: entry.sizeBytes,
          timestamp: entry.timestamp,
          error: '',
        }
        this.emit('capture.completed', notice)
        this.emit('state.changed', structuredClone(this.snapshot))
        return { ticket, outputPath } as ScreenshotMethods[M]['result']
      }

      case 'folder.open':
        return { path: this.snapshot.config.outputDir } as ScreenshotMethods[M]['result']

      case 'folder.browse': {
        const nextDir = 'D:\\Captures\\Gakumas'
        this.snapshot.config = { ...this.snapshot.config, outputDir: nextDir }
        this.emit('folder.browsed', { cancelled: false, path: nextDir, error: '' })
        this.emit('state.changed', structuredClone(this.snapshot))
        return { pending: false, cancelled: false, path: nextDir } as ScreenshotMethods[M]['result']
      }

      default:
        throw new BridgeError('unknown_method', `Unknown method: ${method}`)
    }
  }

  on<E extends ScreenshotEventName>(
    event: E,
    handler: (payload: ScreenshotEvents[E]) => void,
  ): () => void {
    let set = this.handlers.get(event)
    if (!set) {
      set = new Set()
      this.handlers.set(event, set)
    }
    set.add(handler as (payload: never) => void)
    return () => {
      set?.delete(handler as (payload: never) => void)
    }
  }

  push<E extends ScreenshotEventName>(event: E, payload: ScreenshotEvents[E]): void {
    this.emit(event, payload)
  }

  setAppearance(theme: number, uiScale: number): void {
    const next: ScreenshotAppearance = { theme, uiScale }
    this.snapshot.appearance = next
    this.emit('appearance.changed', next)
    this.emit('state.changed', structuredClone(this.snapshot))
  }

  setGreenScreen(detected: boolean): void {
    this.snapshot.capabilities = { ...this.snapshot.capabilities, greenScreenDetected: detected }
    this.emit('state.changed', structuredClone(this.snapshot))
  }

  close(): void {
    this.closed = true
    this.handlers.clear()
  }

  private emit<E extends ScreenshotEventName>(event: E, payload: ScreenshotEvents[E]): void {
    const set = this.handlers.get(event)
    if (!set) return
    for (const handler of set) {
      ;(handler as (p: ScreenshotEvents[E]) => void)(payload)
    }
  }
}
