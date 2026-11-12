# Themes

Hangar ships with 14 themes, and you can add your own as small JSON files. The same format will theme every app in the suite once the Hangar SDK supports live theming (Phase 4).

## Built-in themes

| Theme | Look |
|---|---|
| Dark | The default. Mica backdrop on Windows 11, Windows accent color |
| Light | Bright and clean |
| Windows | Light or Dark to match Windows, and switches when Windows does |
| Midnight | Pure black for OLED screens, with a black title bar |
| Glass | Acrylic: see-through over your wallpaper, with translucent cards |
| Frost | Cool arctic blues |
| Twilight | Deep violet |
| Forest | Evergreen |
| Sunset | Warm browns with orange |
| Ocean | Deep teal |
| Rose | Pink on plum |
| Graphite | Monochrome |
| Paper | Warm light |
| Mist | Cool light gray-blue |

**Accent color** (Themes → Accent color) can be:
- the theme's own accent
- your Windows accent color
- one of 9 swatches
- any color from **Custom…**

Hangar keeps every accent readable: it lightens dark accents on dark themes and darkens bright accents on light themes.

## Theme files

Put `.json` files in `%LOCALAPPDATA%\Hangar\themes\` (Themes → **Open folder**) and press **Reload**. **Save as file** writes the current theme there, so you can start from one you like.

```json
{
  "schema": 1,
  "name": "My Theme",
  "description": "Shown under the name",
  "mode": "dark",              // dark | light
  "backdrop": "mica",          // mica | acrylic | none (Windows 11 22H2+; ignored elsewhere)
  "transparency": 0.15,        // 0-0.9: how much of the backdrop shows through
  "surfaceOpacity": 1.0,       // below 1 for see-through cards (Glass uses 0.07)
  "colors": {
    "background": "#2E3440",
    "sidebar": "#272C36",
    "surface": "#3B4252",       // cards, panels
    "surfaceHover": "#434C5E",
    "surfaceStrong": "#4C566A", // pressed, track, selected
    "text": "#ECEFF4",
    "subtext": "#C0C8D6",
    "faint": "#7B8698",
    "accent": "#88C0D0"         // or "windows"
  }
}
```

Any color you leave out comes from Dark or Light (whichever `mode` says). Status colors (good, warning, error) are picked automatically for the mode. The file name doesn't matter; `name` is what's shown.
