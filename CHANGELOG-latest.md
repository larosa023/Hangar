## Hangar 0.1.3

### Fixed
- **An app could keep showing "Update available" right after updating.** If an app's exe still carried an old version number in its file properties, Hangar believed the old number. Hangar now remembers exactly which release it installed and trusts that as long as the file hasn't changed. (Foyer 0.2.0 was the first app to hit this.)
- **Apps you downloaded but never "installed" now show as installed.** Hangar now also looks in your Downloads folder and on your Desktop, including renamed copies like `Breaker (1).exe`. It only accepts an exe whose file properties name the app or larosa023, so an unrelated file with a similar name is never picked up.

### Also
- Hangar's own version number is now stamped into Hangar.exe automatically on every build.
