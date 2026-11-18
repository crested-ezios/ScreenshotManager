import { render, screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import App from '@/App'
import { getScreenshotBridge, ScreenshotMockBridgeClient, setScreenshotBridge } from '@/bridge'
import { screenshotStore } from '@/store'

let bridge: ScreenshotMockBridgeClient

beforeEach(async () => {
  bridge = new ScreenshotMockBridgeClient()
  setScreenshotBridge(bridge)
  screenshotStore.reset()
  await screenshotStore.start()
})

afterEach(() => {
  getScreenshotBridge()?.close()
  bridge.close()
  screenshotStore.reset()
  setScreenshotBridge(null)
  vi.restoreAllMocks()
})

describe('ScreenshotManager UI', () => {
  it('renders resolution presets, live green screen badge, and recent captures', () => {
    render(<App />)
    expect(screen.getByTestId('runtime-status-badge')).toHaveTextContent('渲染管线就绪')
    expect(screen.getByTestId('greenscreen-badge')).toHaveTextContent('GakuMotion 绿幕已激活')
    expect(screen.getByTestId('active-resolution-badge')).toHaveTextContent('3840×2160')
    expect(screen.getByTestId('effective-alpha-badge')).toHaveTextContent('当前输出: 透明 RGBA')
    expect(screen.getByTestId('recent-captures-list')).toHaveTextContent('Gakumas_20261118_201500_000.png')
  })

  it('switches resolution presets and applies custom WxH dimensions', async () => {
    const user = userEvent.setup()
    const spy = vi.spyOn(bridge, 'request')
    render(<App />)

    await user.click(screen.getByTestId('preset-portrait_4k'))
    await waitFor(() => expect(screen.getByTestId('active-resolution-badge')).toHaveTextContent('2160×3840'))

    await user.click(screen.getByTestId('preset-native'))
    await waitFor(() => expect(screen.getByTestId('active-resolution-badge')).toHaveTextContent('窗口原生 (1920×1080)'))

    await user.click(screen.getByTestId('preset-custom'))
    const wInput = screen.getByLabelText('自定义宽度')
    const hInput = screen.getByLabelText('自定义高度')
    await user.clear(wInput)
    await user.type(wInput, '1600')
    await user.clear(hInput)
    await user.type(hInput, '900')
    await user.click(screen.getByTestId('apply-custom-resolution'))

    await waitFor(() => expect(screen.getByTestId('active-resolution-badge')).toHaveTextContent('1600×900'))
    expect(spy).toHaveBeenCalledWith('config.set', { width: 1600, height: 900 })
  })

  it('reflects live GakuMotion green screen changes and alpha mode selection', async () => {
    render(<App />)
    expect(screen.getByTestId('effective-alpha-badge')).toHaveTextContent('当前输出: 透明 RGBA')

    bridge.setGreenScreen(false)
    await waitFor(() => {
      expect(screen.getByTestId('greenscreen-badge')).toHaveTextContent('GakuMotion 绿幕未开启')
      expect(screen.getByTestId('effective-alpha-badge')).toHaveTextContent('当前输出: 不透明 RGB')
    })

    await screenshotStore.setAlphaMode('force_transparent')
    await waitFor(() => {
      expect(screen.getByTestId('effective-alpha-badge')).toHaveTextContent('当前输出: 透明 RGBA')
    })

    await screenshotStore.setImageFormat('jpeg')
    await waitFor(() => {
      expect(screen.getByTestId('effective-alpha-badge')).toHaveTextContent('当前输出: 不透明 RGB')
    })
  })

  it('triggers capture.take and appends completed screenshot to recent captures', async () => {
    const user = userEvent.setup()
    render(<App />)

    await user.click(screen.getByTestId('capture-now-button'))
    await waitFor(() => {
      expect(screen.getByTestId('recent-captures-list')).toHaveTextContent('Gakumas_mock_0001.png')
      expect(screen.getByTestId('action-notice')).toHaveTextContent('截图已保存')
    })
  })

  it('browses and opens output folder and handles bridge reconnect', async () => {
    const user = userEvent.setup()
    const spy = vi.spyOn(bridge, 'request')
    render(<App />)

    await user.click(screen.getByTestId('browse-folder-button'))
    await waitFor(() => {
      expect(screen.getByLabelText('截图输出目录')).toHaveValue('D:\\Captures\\Gakumas')
    })

    await user.click(screen.getByTestId('open-folder-button'))
    expect(spy).toHaveBeenCalledWith('folder.open', {})

    bridge.push('bridge.closed', { reason: 'detach' })
    await waitFor(() => expect(screen.getByRole('button', { name: '重新连接' })).toBeInTheDocument())
    await user.click(screen.getByRole('button', { name: '重新连接' }))
    await waitFor(() => expect(screen.getByTestId('capture-now-button')).toBeInTheDocument())
  })

  it('rejects out-of-range custom resolution and reacts to appearance.changed theme events', async () => {
    const user = userEvent.setup()
    render(<App />)

    await user.click(screen.getByTestId('preset-custom'))
    const wInput = screen.getByLabelText('自定义宽度')
    await user.clear(wInput)
    await user.type(wInput, '9999')
    await user.click(screen.getByTestId('apply-custom-resolution'))
    await waitFor(() => {
      expect(screen.getByTestId('action-notice')).toHaveTextContent('自定义分辨率必须在')
    })

    bridge.setAppearance(2, 1.25)
    await waitFor(() => {
      expect(document.documentElement.dataset.theme).toBe('paper')
      expect(document.documentElement.style.zoom).toBe('1.25')
    })
  })
})
