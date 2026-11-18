import { WebView2BridgeCore, isWebView2Available } from '@/bridge/core'
import { ScreenshotMockBridgeClient } from './mock'
import type { ScreenshotBridgeClient, ScreenshotEvents, ScreenshotMethods } from './protocol'

export { isWebView2Available }

export class ScreenshotWebView2Client
  extends WebView2BridgeCore<ScreenshotMethods, ScreenshotEvents>
  implements ScreenshotBridgeClient
{
  readonly kind = 'webview2' as const
}

let client: ScreenshotBridgeClient | null = null

export function getScreenshotBridge(): ScreenshotBridgeClient | null {
  if (!client) {
    if (isWebView2Available()) client = new ScreenshotWebView2Client()
    else if (import.meta.env.DEV || import.meta.env.MODE === 'preview') client = new ScreenshotMockBridgeClient()
    else return null
  }
  return client
}

export function setScreenshotBridge(next: ScreenshotBridgeClient | null): void {
  client = next
}

export { ScreenshotMockBridgeClient } from './mock'
