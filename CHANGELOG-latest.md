## Hangar 0.1.5

**No more "GitHub limit reached".** Without a GitHub sign-in, GitHub allows only 60 release checks an hour, and Hangar was using 22 per refresh while sharing that budget with your apps' own updaters. Now:
- A job in the Hangar repo collects every app's releases into one file every 10 minutes.
- Hangar downloads that one file, which isn't counted against the limit, instead of calling GitHub once per app.
- If the file is ever missing or out of date, Hangar falls back to asking GitHub directly, as before.

Downloads are still verified against each release's SHA-256.

**Isle** has joined the library under Windows replacements.
