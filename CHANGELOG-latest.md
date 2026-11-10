## Hangar 0.1.1

**Finds your apps wherever they are.** Before, Hangar only looked in each app's standard install folder, so apps you run from somewhere else showed "Install". Now it also checks:
- apps that are running right now
- startup entries (the Windows "Run" list) and Start menu, desktop and taskbar shortcuts
- common folders: `%LOCALAPPDATA%\<App>`, `%APPDATA%\<App>`, `Program Files`
- Apps & features entries whose icon isn't the exe (fixes Gridline)

Hangar checks each exe's version info so it doesn't mistake another program with the same file name for one of yours, and it remembers where each app was found. If an app still isn't found, open its page and click **Already installed? Locate it**.
