// Hangar - HTTP(S) via WinHTTP
#pragma once
#include "common.h"
#include <functional>
#include <atomic>

namespace net {

struct Response {
    int status = 0;
    std::string body;
    std::wstring etag;
    std::wstring error;           // set when the request couldn't be made at all
    int rateRemaining = -1;       // GitHub x-ratelimit-remaining
    long long rateReset = 0;      // GitHub x-ratelimit-reset (unix seconds)
};

void Init();                       // call once on the UI thread before any request
void Shutdown();

// GET a URL. `etag` (optional) sends If-None-Match; a 304 comes back with an empty body.
bool Get(const std::wstring& url, Response& r, const std::wstring& etag = L"", bool githubApi = false);

// Download to `path` (written to path.part first). `progress(done, total)` is called often;
// return false from it to cancel. total is 0 when the server didn't say.
bool Download(const std::wstring& url, const std::wstring& path,
              const std::function<bool(uint64_t, uint64_t)>& progress, std::wstring* error);

// Base for GitHub API calls ("https://api.github.com"); HANGAR_API_BASE overrides it for testing.
std::wstring ApiBase();
std::wstring UserAgent();

} // namespace net
