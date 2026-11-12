#!/usr/bin/env python3
"""Builds releases.json: the recent GitHub releases of every app in manifest.json, plus Hangar.

Hangar downloads this one file (from the `index` branch, via raw.githubusercontent.com) instead
of calling the GitHub API once per app. That keeps it clear of GitHub's 60-requests-an-hour limit
for signed-out users. Runs in .github/workflows/index.yml with the workflow's GITHUB_TOKEN.

Usage: build_index.py <out.json> [previous.json]
Exits 0 with "unchanged" on stdout when only the timestamp would change.
"""
import json, os, sys, time, urllib.request, urllib.error

KEEP_RELEASE = ("tag_name", "name", "draft", "prerelease", "body", "published_at", "html_url")
KEEP_ASSET = ("name", "browser_download_url", "size", "digest")
MAX_BODY = 6000
REFRESH_EVERY = 6 * 3600   # rewrite the file at least this often so clients can tell it's alive


def get(url, token):
    req = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "Hangar-index",
        **({"Authorization": f"Bearer {token}"} if token else {}),
    })
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def slim(rel):
    out = {k: rel.get(k) for k in KEEP_RELEASE}
    if out["body"] and len(out["body"]) > MAX_BODY:
        out["body"] = out["body"][:MAX_BODY] + "\n…"
    out["assets"] = [{k: a.get(k) for k in KEEP_ASSET} for a in rel.get("assets", [])]
    return out


def main():
    out_path = sys.argv[1]
    prev_path = sys.argv[2] if len(sys.argv) > 2 else None
    token = os.environ.get("GITHUB_TOKEN", "")
    manifest = json.load(open("manifest.json", encoding="utf-8"))
    repos = ["larosa023/Hangar"] + [a["repo"] for a in manifest["apps"]]
    seen, index = set(), {}
    for repo in repos:
        key = repo.lower()
        if key in seen:
            continue
        seen.add(key)
        try:
            rels = get(f"https://api.github.com/repos/{repo}/releases?per_page=15", token)
            index[key] = [slim(r) for r in rels if not r.get("draft")]
            print(f"{repo}: {len(index[key])} releases", file=sys.stderr)
        except urllib.error.HTTPError as e:
            print(f"{repo}: HTTP {e.code}, skipped", file=sys.stderr)
        except Exception as e:  # network hiccup: leave the repo out, Hangar falls back to the API for it
            print(f"{repo}: {e}, skipped", file=sys.stderr)

    now = int(time.time())
    if prev_path and os.path.exists(prev_path):
        try:
            prev = json.load(open(prev_path, encoding="utf-8"))
            if prev.get("repos") == index and now - int(prev.get("generated", 0)) < REFRESH_EVERY:
                print("unchanged")
                return
        except Exception:
            pass
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump({"schema": 1, "generated": now, "repos": index}, f, ensure_ascii=False, separators=(",", ":"))
    print("written")


if __name__ == "__main__":
    main()
