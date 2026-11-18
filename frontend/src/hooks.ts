import * as React from 'react'
import { getScreenshotBridge } from './bridge'
import type { ScreenshotAppearance } from './protocol'

const THEME_NAMES = ['dark', 'sapphire', 'paper', 'apple-light', 'apple-dark'] as const
const DARK_THEMES = new Set([0, 1, 4])

export function useScreenshotAppearanceEffect(appearance: ScreenshotAppearance | null): void {
  React.useEffect(() => {
    const theme = appearance?.theme ?? 0
    const root = document.documentElement
    root.dataset.theme = THEME_NAMES[theme] ?? 'dark'
    root.classList.toggle('dark', DARK_THEMES.has(theme))
    root.style.zoom = String(appearance?.uiScale ?? 1)
  }, [appearance])
}

function isEditable(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) return false
  if (target.tagName === 'TEXTAREA' || target.tagName === 'SELECT') return true
  if (target.tagName === 'INPUT') {
    const type = (target as HTMLInputElement).type
    return !['button', 'checkbox', 'radio', 'range', 'submit', 'reset', 'file', 'color'].includes(type)
  }
  return target.isContentEditable
}

export function useScreenshotTextInputBridge(enabled: boolean): void {
  React.useEffect(() => {
    if (!enabled) return
    const bridge = getScreenshotBridge()
    if (!bridge) return
    let active = false
    const report = (focused: boolean) => {
      if (focused === active) return
      active = focused
      void bridge.request('shell.setTextInputActive', { active: focused }).catch(() => {})
    }
    const onFocusIn = (event: FocusEvent) => report(isEditable(event.target))
    const onFocusOut = (event: FocusEvent) => report(isEditable(event.relatedTarget))
    const onWindowBlur = () => report(false)
    document.addEventListener('focusin', onFocusIn)
    document.addEventListener('focusout', onFocusOut)
    window.addEventListener('blur', onWindowBlur)
    return () => {
      document.removeEventListener('focusin', onFocusIn)
      document.removeEventListener('focusout', onFocusOut)
      window.removeEventListener('blur', onWindowBlur)
      report(false)
    }
  }, [enabled])
}
