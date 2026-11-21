## Hangar 0.1.7

**Tray apps update now.** Some apps (Lumora, for one) only hide to the tray when asked to close, so their updates failed with "didn't close". Hangar now offers to force-close the app and finish the update. It only does that if you say yes.

## Also in this update (0.1.6): smoother and lighter

**Faster drawing**
- Text is laid out once and reused instead of on every frame. That was the most expensive part of drawing, so scrolling, hover effects and page changes do much less work per frame.
- Hangar no longer reads the disk or the registry while drawing. Previous Hangar versions, system colors and cleaned-up release notes are now read once.

**Lighter in the background**
- Background work runs on a few shared worker threads instead of one thread per job: 4 instead of about 50 at startup. Idle workers exit after a minute.
- "Running" detection checks every 2.5 seconds while Hangar is in front, and every 7 seconds when it isn't.
- Minimizing Hangar stops all polling and hands its memory back to Windows.
- Switching back to Hangar no longer re-scans every Start menu shortcut. Refresh (F5) still does a full scan.
- App icons are held only on the GPU instead of in both places.

**Refinements**
- **Crisper text:** solid themes now use ClearType. See-through themes (Mica, Glass) keep grayscale smoothing, because Windows requires it for transparency.
- Animations run at the same speed at any frame rate, and turn off when Windows' **Animation effects** setting is off.
- The **scrollbar** widens when you point at it, can be dragged, and clicking its track jumps a page.
- A soft shadow appears under the app card you're pointing at.
- **Ctrl+A** in search selects everything, so the next key replaces it.
