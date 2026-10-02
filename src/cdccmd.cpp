// ============================================================================
//  cdccmd.cpp -- CDCCMD 多功能命令行工具
//
//  命令一览:
//    cdcgb <github仓库URL> [目录]   克隆 GitHub 仓库 (自动测速挑选最快节点)
//    cdc download                   下载并打开主项目最新安装包
//    <有效网址>                     用默认浏览器打开网址
//    jcgx                           检查 CDCCMD 自身是否有新版本, 询问后更新
//    uncdccmd                       打开卸载程序
//    help                           显示帮助
//
//  设计要点:
//    * 纯 Win32 + WinHTTP, 静态链接, 零第三方依赖, 单文件绿色运行
//    * UTF-8 控制台输出: 把中文文案预先转成 ANSI 再打印, 避免 mingw `%s`
//      打印 wchar_t 只输出首字节的老坑 (见项目红线 #4)
//    * 自更新/自卸载不能覆盖正在运行的自身 -> 一律生成 .bat 延迟替换
//
//  编译 (不要加 -municode, 我们要标准 main(argc, argv)):
//    g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++
//        -o cdccmd.exe cdccmd.cpp -lwinhttp -lshell32 -lole32 -ladvapi32 -luuid
//  注意: FOLDERID_* 系列是 GUID 符号, 必须链 uuid (-luuid)
// ============================================================================

#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <algorithm>

#include "version.h"   // ★ 所有可配置项都在这里

// ---------------------------------------------------------------- 控制台 / 编码

// 把 UTF-8 文案按当前控制台代码页输出 (ANSI 中文系统 -> GBK 正常显示)
static void Say(const char* utf8) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wn <= 0) return;
    std::wstring w(wn, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], wn);

    UINT cp = GetConsoleOutputCP();
    if (cp == 0) cp = CP_ACP;
    int an = WideCharToMultiByte(cp, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (an <= 0) return;
    std::string a(an, 0);
    WideCharToMultiByte(cp, 0, w.c_str(), -1, &a[0], an, nullptr, nullptr);
    a.pop_back();
    fputs(a.c_str(), stdout);
    fflush(stdout);
}

static void SayLn(const char* utf8) { Say(utf8); fputc('\n', stdout); fflush(stdout); }

// 宽字符路径 -> ANSI/CP936, 给 system() / ofstream 用
static std::string NarrowPath(const std::wstring& w) {
    UINT cp = GetConsoleOutputCP();
    if (cp == 0) cp = CP_ACP;
    int n = WideCharToMultiByte(cp, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(n, 0);
    WideCharToMultiByte(cp, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    s.pop_back();
    return s;
}

static std::wstring ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.pop_back();
    return w;
}

// ---------------------------------------------------------------- 字符串辅助

static std::string ToLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
static bool StartsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
static bool EndsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
static bool Contains(const std::string& s, const std::string& p) {
    return s.find(p) != std::string::npos;
}

// ---------------------------------------------------------------- WinHTTP 核心

struct UrlParts { std::wstring host, path; unsigned short port = 443; bool https = true; };

static bool CrackUrl(const std::string& url, UrlParts& out) {
    std::wstring w = ToWide(url);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512] = { 0 }, path[4096] = { 0 };
    uc.lpszHostName = host; uc.dwHostNameLength = 511;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 4095;
    if (!WinHttpCrackUrl(w.c_str(), (DWORD)w.size(), 0, &uc)) return false;
    if (uc.dwHostNameLength == 0) return false;
    out.host = host;
    out.path = (uc.dwUrlPathLength > 0) ? path : L"/";
    if (uc.dwUrlPathLength == 0) out.path = L"/";
    // 把查询串接回 path
    out.port = uc.nPort;
    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

struct HttpResponse {
    bool ok = false;
    DWORD status = 0;
    std::string body;
};

// 一次 HTTP GET; 自动跟随重定向; body 可选落盘
static HttpResponse HttpGet(const std::string& url,
    std::vector<std::pair<std::wstring, std::wstring>> headers = {},
    bool progress = false, bool quiet = false) {
    HttpResponse res;
    UrlParts up;
    if (!CrackUrl(url, up)) { if (!quiet) SayLn("[错误] URL 解析失败"); return res; }

    HINTERNET hSess = WinHttpOpen(L"cdccmd/" L"" CDCCMD_VERSION,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) { if (!quiet) SayLn("[错误] WinHttpOpen 失败"); return res; }

    HINTERNET hConn = WinHttpConnect(hSess, up.host.c_str(), up.port, 0);
    if (!hConn) { if (!quiet) SayLn("[错误] 无法连接服务器"); WinHttpCloseHandle(hSess); return res; }

    DWORD flags = up.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", up.path.c_str(), NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return res; }

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hReq, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));
    // 老系统 TLS 根证书不全时的兜底
    DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
    WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
    WinHttpAddRequestHeaders(hReq, L"Accept: application/vnd.github+json\r\n",
        -1, WINHTTP_ADDREQ_FLAG_ADD);
    for (auto& h : headers)
        WinHttpAddRequestHeaders(hReq, (h.first + L": " + h.second + L"\r\n").c_str(),
            -1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, NULL)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        if (!quiet) SayLn("[错误] 请求失败 (网络不通 / 被墙 / DNS 失败)");
        return res;
    }

    DWORD code = 0, csz = sizeof(code);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        NULL, &code, &csz, NULL);
    res.status = code;

    DWORD avail = 0;
    char buf[16384];
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        DWORD toRead = (avail > sizeof(buf)) ? (DWORD)sizeof(buf) : avail;
        DWORD read = 0;
        if (!WinHttpReadData(hReq, buf, toRead, &read) || read == 0) break;
        res.body.append(buf, read);
    }

    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
    res.ok = (code >= 200 && code < 300);
    (void)progress;
    return res;
}

// GitHub API 可选 token (环境变量 CDC_GH_TOKEN / GITHUB_TOKEN), 提升限流额度
static std::vector<std::pair<std::wstring, std::wstring>> GhHeaders() {
    std::vector<std::pair<std::wstring, std::wstring>> h;
    const char* names[] = { "CDC_GH_TOKEN", "GITHUB_TOKEN", "GH_TOKEN" };
    for (const char* n : names) {
        const char* v = getenv(n);
        if (v && *v) {
            h.push_back({ L"Authorization", ToWide(std::string("Bearer ") + v) });
            break;
        }
    }
    return h;
}

// ---------------------------------------------------------------- GitHub JSON 解析
// 说明: 只做 "够用" 的解析 —— 找 assets 数组里的 name / browser_download_url / size。
// 不引第三方 JSON 库, 保持单文件零依赖。

struct Asset {
    std::string name;
    std::string url;
    long long   size = 0;
};

// 从 pos 开始找 "key"\s*:\s*"value" 并返回 value (纯字符串)
static bool JsonStrAfter(const std::string& s, size_t from, const std::string& key,
    std::string& out, size_t* endPos = nullptr) {
    std::string pat = "\"" + key + "\"";
    size_t k = s.find(pat, from);
    if (k == std::string::npos) return false;
    size_t colon = s.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    size_t q1 = s.find('"', colon + 1);
    if (q1 == std::string::npos) return false;
    // 处理转义
    std::string v;
    size_t i = q1 + 1;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) { v += s[i + 1]; i += 2; continue; }
        if (c == '"') break;
        v += c; ++i;
    }
    out = v;
    if (endPos) *endPos = i;
    return true;
}

static long long JsonNumAfter(const std::string& s, size_t from, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t k = s.find(pat, from);
    if (k == std::string::npos) return 0;
    size_t colon = s.find(':', k + pat.size());
    if (colon == std::string::npos) return 0;
    size_t i = colon + 1;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    return _strtoi64(s.c_str() + i, nullptr, 10);
}

// 解析 release JSON -> assets 列表
static std::vector<Asset> ParseAssets(const std::string& json) {
    std::vector<Asset> out;
    size_t p = json.find("\"assets\"");
    if (p == std::string::npos) return out;
    size_t lb = json.find('[', p);
    if (lb == std::string::npos) return out;
    // 逐个 browser_download_url 收集 (一个 asset 恰好一个)
    size_t pos = lb;
    while (true) {
        size_t u = json.find("\"browser_download_url\"", pos);
        if (u == std::string::npos) break;
        Asset a;
        if (!JsonStrAfter(json, u, "browser_download_url", a.url, &pos)) break;
        // name 在该对象的更前面, 往回找最近的 "name"
        size_t back = u;
        std::string nm;
        while (back > lb) {
            size_t cand = json.rfind("\"name\"", back - 1);
            if (cand == std::string::npos || cand < lb) break;
            size_t q1 = json.find('"', json.find(':', cand) + 1);
            size_t q2 = json.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                nm = json.substr(q1 + 1, q2 - q1 - 1);
                break;
            }
            back = cand;
        }
        a.name = nm;
        a.size = JsonNumAfter(json, back > lb ? back : lb, "size");
        if (a.size <= 0) a.size = JsonNumAfter(json, lb, "size");
        out.push_back(a);
    }
    // 兜底: 上一轮没取到 name 的, 用 url 最后一段
    for (auto& a : out) {
        if (a.name.empty()) {
            size_t sl = a.url.find_last_of('/');
            if (sl != std::string::npos) a.name = a.url.substr(sl + 1);
        }
    }
    return out;
}

static std::string JsonTagName(const std::string& json) {
    std::string v;
    JsonStrAfter(json, 0, "tag_name", v);
    return v;
}

// 只看 EXE、且优先 setup/install 命名的
static const Asset* PickInstaller(const std::vector<Asset>& as) {
    for (auto& a : as)
        if (EndsWith(ToLower(a.name), ".exe") &&
            (Contains(ToLower(a.name), "setup") || Contains(ToLower(a.name), "install")))
            return &a;
    for (auto& a : as)
        if (EndsWith(ToLower(a.name), ".exe")) return &a;
    return nullptr;
}

// ---------------------------------------------------------------- 版本比较

static void ParseVer3(const std::string& v, int& a, int& b, int& c) {
    a = b = c = 0;
    std::string s = v;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s = s.substr(1);
    sscanf(s.c_str(), "%d.%d.%d", &a, &b, &c);
}
static bool IsNewer(const std::string& latest, const std::string& cur) {
    int a1, b1, c1, a2, b2, c2;
    ParseVer3(latest, a1, b1, c1);
    ParseVer3(cur, a2, b2, c2);
    if (a1 != a2) return a1 > a2;
    if (b1 != b2) return b1 > b2;
    return c1 > c2;
}

// ---------------------------------------------------------------- 路径辅助

static std::wstring ModulePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}
static std::wstring ModuleDir() {
    std::wstring p = ModulePath();
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? p : p.substr(0, s);
}
static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring TempDir() {
    wchar_t t[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, t);
    return std::wstring(t, n);
}
static std::wstring DownloadsDir() {
    std::wstring r;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &p)) && p) {
        r = p;
        CoTaskMemFree(p);
    }
    // 已知文件夹 API 可能返回一个并不存在的路径(被重定向/精简过) -> 实测存在才用
    if (!r.empty() && DirExists(r)) return r;

    const char* up = getenv("USERPROFILE");
    if (up) {
        std::wstring d = ToWide(up) + L"\\Downloads";
        if (DirExists(d)) return d;
        // 不存在就建一个, 建得成就用
        if (CreateDirectoryW(d.c_str(), NULL)) return d;
    }
    if (!r.empty()) {
        // 用已知文件夹路径兜底(下载时若写不进去会报错)
        return r;
    }
    return TempDir();
}
static bool RunAndWait(const std::string& cmd, DWORD* exitCode = nullptr) {
    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    // 不传自定义环境块 (项目红线 #10: 自定义环境块曾导致 CreateProcessW err=87)
    if (!CreateProcessA(NULL, buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (exitCode) *exitCode = ec;
    return true;
}

// ---------------------------------------------------------------- 功能 1: cdcgb 克隆
// 策略: 不启动一堆 git 进程, 而是先用 WinHTTP 对各镜像的 info/refs 做
// ============================================================================
//  加速节点引擎
//  ---------------------------------------------------------------------------
//  GitHub 在国内直连常常很慢, 各类"前缀代理"节点速度与能力差异极大, 实测(2026-10):
//
//    * 元数据(api.github.com): 只有 gh-proxy.com 返回真 JSON;
//      kkgithub 会返回它自己的假 404 JSON, ghproxy.net 返回一张 HTML 页面
//      -> 所以代理 API 时【必须校验返回内容】, 否则会把垃圾当数据
//    * release 资产下载: gh.dpik.top / ghfile.geekertao.top (akams.cn 节点)
//      首字节 1 秒级, 比直连快得多 -> 最适合做下载加速
//
//  因此策略:
//    元数据 = 多通道回退 + 内容校验
//    下载   = 全节点测速排序 + 失败自动切换 + Range 断点续传
// ============================================================================

static std::string HumanSize(long long b) {
    char t[64];
    if (b >= 1073741824LL)      sprintf(t, "%.2f GB", (double)b / 1073741824.0);
    else if (b >= 1048576LL)    sprintf(t, "%.2f MB", (double)b / 1048576.0);
    else if (b >= 1024LL)       sprintf(t, "%.1f KB", (double)b / 1024.0);
    else                        sprintf(t, "%lld B", b);
    return t;
}

// 目标文件当前大小; 不存在返回 0, 出错返回 -1
static long long LocalFileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    }
    LARGE_INTEGER li;
    li.LowPart = d.nFileSizeLow;
    li.HighPart = (LONG)d.nFileSizeHigh;
    return li.QuadPart;
}

// 节点定义。tpl 里的 {u} = 完整原始 URL, {s} = 去掉协议头的 URL
struct Node {
    const char* name;
    const char* tpl;
    bool api;    // 能可靠代理 api.github.com
    bool file;   // 能代理 release 资产 / archive 下载
    bool git;    // 能代理 git clone
};

static const Node kNODES[] = {
    // --- 直连 ---
    { "GitHub 官方",           "",                                true,  true,  true  },
    // --- 全能代理 (实测能可靠代理 API) ---
    { "gh-proxy.com",          "https://gh-proxy.com/{u}",         true,  true,  true  },
    // --- akams.cn 节点: 文件下载与 clone 都很快 (~1.1s), 但不支持代理 API ---
    { "gh.dpik.top",           "https://gh.dpik.top/{u}",          false, true,  true  },
    { "ghfile.geekertao.top",  "https://ghfile.geekertao.top/{u}", false, true,  true  },
    { "github.tbap.top",       "https://github.tbap.top/{u}",      false, true,  false },
    // --- 其它高速代理: 实测文件下载 ~1s、clone 可用, 同样不支持代理 API ---
    { "gh.zwy.one",            "https://gh.zwy.one/{u}",           false, true,  true  },
    // --- 其它已知镜像 (能力按实测标注; 不可用的会在测速阶段被自动跳过) ---
    { "gitclone.com",          "https://gitclone.com/{s}",         false, false, true  },
    { "ghproxy.net",           "https://ghproxy.net/{u}",          false, true,  false },
    { "kkgithub.com",          "https://kkgithub.com/{u}",         false, true,  false },
    { "ghfast.top",            "https://ghfast.top/{u}",           false, true,  false },
    { "ghproxy.cc",            "https://ghproxy.cc/{u}",           false, true,  false },
};
static const int kNODE_COUNT = (int)(sizeof(kNODES) / sizeof(kNODES[0]));

typedef std::pair<std::string, std::string> NameUrl;   // (节点名, 加速后URL)

static std::string WrapNode(const Node& nd, const std::string& raw) {
    std::string out = nd.tpl;
    if (out.empty()) return raw;                       // 直连
    std::string s = raw;
    if (StartsWith(s, "https://"))      s = s.substr(8);
    else if (StartsWith(s, "http://"))  s = s.substr(7);
    for (size_t p = out.find("{u}"); p != std::string::npos; p = out.find("{u}", p))
        out.replace(p, 3, raw);
    for (size_t p = out.find("{s}"); p != std::string::npos; p = out.find("{s}", p))
        out.replace(p, 3, s);
    return out;
}

// 按能力筛出一组候选 (节点名, 加速URL), 顺序 = 表中顺序
static std::vector<NameUrl> CollectNodes(const std::string& raw,
    bool needApi, bool needFile, bool needGit) {
    std::vector<NameUrl> v;
    for (int i = 0; i < kNODE_COUNT; ++i) {
        const Node& nd = kNODES[i];
        if (needApi  && !nd.api)  continue;
        if (needFile && !nd.file) continue;
        if (needGit  && !nd.git)  continue;
        v.push_back(NameUrl(nd.name, WrapNode(nd, raw)));
    }
    return v;
}

// ---------------------------------------------------------------- 测速

struct SpeedInfo {
    std::string name;
    std::string url;
    bool   ok = false;
    bool   complete = false; // 是否拿满了请求的样本量
    int    ms = 0;          // 总耗时
    int    ttfb = 0;        // 首字节耗时(连接+重定向+服务端排队)
    long long bytes = 0;    // 取样实际拿到多少字节
    double kbps = 0.0;      // 估算吞吐(已扣除首字节等待)
};

// 取首块样本测真实吞吐, 比单纯 ping 延迟更能反映下载速度
static SpeedInfo SampleSpeed(const std::string& name, const std::string& url,
    int sampleKB = 96, int timeoutMs = 6000) {
    SpeedInfo si; si.name = name; si.url = url;

    UrlParts up;
    if (!CrackUrl(url, up)) return si;

    HINTERNET hSess = WinHttpOpen(L"cdccmd-spd", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) return si;

    HINTERNET hConn = WinHttpConnect(hSess, up.host.c_str(), up.port, 0);
    if (hConn) {
        DWORD flags = up.https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", up.path.c_str(), NULL,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hReq) {
            WinHttpSetOption(hReq, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
            WinHttpSetOption(hReq, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
            WinHttpSetOption(hReq, WINHTTP_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
            DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(hReq, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));
            DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
            WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));

            char rng[80];
            sprintf(rng, "Range: bytes=0-%d\r\n", sampleKB * 1024 - 1);
            std::wstring wr = ToWide(rng);
            WinHttpAddRequestHeaders(hReq, wr.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

            ULONGLONG t0 = GetTickCount64();
            if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
                WinHttpReceiveResponse(hReq, NULL)) {
                DWORD code = 0, sz = sizeof(code);
                WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    NULL, &code, &sz, NULL);
                if (code == 200 || code == 206) {
                    long long got = 0;
                    DWORD avail = 0;
                    char buf[16384];
                    ULONGLONG tFirst = 0, tEnd = 0;
                    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
                        DWORD toRead = (avail > sizeof(buf)) ? (DWORD)sizeof(buf) : avail;
                        DWORD rd = 0;
                        if (!WinHttpReadData(hReq, buf, toRead, &rd) || rd == 0) break;
                        if (got == 0) tFirst = GetTickCount64();       // 首字节到达
                        got += rd;
                        if (got >= (long long)sampleKB * 1024) { tEnd = GetTickCount64(); break; }
                    }
                    if (!tEnd) tEnd = GetTickCount64();
                    if (!tFirst) tFirst = tEnd;
                    int totalMs = (int)(tEnd - t0);
                    int xferMs  = (int)(tEnd - tFirst);
                    if (totalMs < 1) totalMs = 1;
                    if (got > 0) {
                        si.ok = true;
                        si.ms = totalMs;
                        si.ttfb = (int)(tFirst - t0);
                        si.bytes = got;
                        // 拿满样本才算"可信": 有些节点(实测 ghproxy.net)会提前截断响应,
                        // 只回几十 KB 就 EOF, 这样算出来的吞吐会虚高到荒谬(几十万 KB/s),
                        // 若据此排序就会选到它, 下载时再失败 -> 白折腾一轮。
                        si.complete = (got >= (long long)sampleKB * 1024);
                        // 吞吐按"纯传输时间"算。若只用总耗时, 首字节等待会把速度稀释得很离谱
                        // (实测 48KB 样本: 总耗时 2984ms 中约 2900ms 都是 TTFB)。
                        double useMs = (xferMs >= 40) ? (double)xferMs : (double)totalMs;
                        si.kbps = (double)got / 1024.0 * 1000.0 / useMs;
                    }
                }
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConn);
    }
    WinHttpCloseHandle(hSess);
    return si;
}

// 测速可以关掉(CDC_NO_SPEED=1), 走纯顺序尝试
static bool SpeedDisabled() {
    char b[8] = { 0 };
    DWORD n = GetEnvironmentVariableA("CDC_NO_SPEED", b, sizeof(b) - 1);
    if (n == 0) return false;
    return b[0] == '1' || b[0] == 'y' || b[0] == 'Y' || b[0] == 't' || b[0] == 'T';
}

// 并发测速的工作项 (用纯 Win32 线程, 不引 std::thread 以免多一层运行库依赖)
struct SpeedTask {
    const std::string* name;
    const std::string* url;
    SpeedInfo* out;
};

static DWORD WINAPI SpeedThreadProc(LPVOID p) {
    SpeedTask* t = (SpeedTask*)p;
    *t->out = SampleSpeed(*t->name, *t->url);
    return 0;
}

// 对候选节点测速, 返回按吞吐从快到慢排序的可用列表
static std::vector<NameUrl> RankBySpeed(const std::vector<NameUrl>& cands,
    const std::string& what, bool verbose) {
    if (cands.size() <= 1) return cands;
    if (SpeedDisabled()) {
        if (verbose) SayLn("[信息] 已按 CDC_NO_SPEED 跳过测速, 按顺序尝试节点。");
        return cands;
    }

    if (verbose) Say(("[信息] 正在为" + what + "并发测速 " +
                      std::to_string(cands.size()) + " 个加速节点 ...\n").c_str());

    // 串行测 10 个节点最坏要几十秒; 并发后总耗时 ≈ 最慢的那个
    const size_t n = cands.size();
    std::vector<SpeedInfo> infos(n);
    std::vector<SpeedTask> tasks(n);
    std::vector<HANDLE> hs;
    hs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        tasks[i].name = &cands[i].first;
        tasks[i].url  = &cands[i].second;
        tasks[i].out  = &infos[i];
        HANDLE h = CreateThread(NULL, 0, SpeedThreadProc, &tasks[i], 0, NULL);
        if (h) hs.push_back(h);
        else   infos[i] = SampleSpeed(cands[i].first, cands[i].second);   // 线程起不来就同步做
    }
    if (!hs.empty())
        WaitForMultipleObjects((DWORD)hs.size(), hs.data(), TRUE, 20000);
    for (HANDLE h : hs) CloseHandle(h);

    if (verbose) {
        for (size_t i = 0; i < n; ++i) {
            const SpeedInfo& si = infos[i];
            if (si.ok) {
                char b[192];
                sprintf(b, "  [测速] %-24s 首字节 %5d ms   吞吐 %8.0f KB/s%s\n",
                    si.name.c_str(), si.ttfb, si.kbps,
                    si.complete ? "" : "   (响应不完整)");
                Say(b);
            } else {
                Say(("  [测速] " + si.name + std::string(" 不可用\n")).c_str());
            }
        }
    }

    std::vector<SpeedInfo> alive;
    for (auto& s : infos) if (s.ok) alive.push_back(s);
    std::sort(alive.begin(), alive.end(), [](const SpeedInfo& a, const SpeedInfo& b) {
        if (a.complete != b.complete) return a.complete;   // 样本完整的优先
        return a.kbps > b.kbps;
    });

    std::vector<NameUrl> out;
    for (auto& s : alive) out.push_back(NameUrl(s.name, s.url));
    if (out.empty()) {
        // 全灭 -> 保持原顺序盲试
        if (verbose) SayLn("[警告] 所有节点测速均失败, 改为按顺序逐个尝试 ...");
        return cands;
    }
    if (verbose) {
        char b[192];
        sprintf(b, "[信息] 首选节点: %s (%.0f KB/s)\n", alive[0].name.c_str(), alive[0].kbps);
        Say(b);
    }
    // 未被测出但确实可用的节点也追加在后面(顺序原样), 作为后备
    for (auto& c : cands) {
        bool dup = false;
        for (auto& s : alive) if (s.url == c.second) { dup = true; break; }
        if (!dup) out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------- 可续传下载

// 单节点下载, 支持 Range 断点续传。
// 失败时已写入的数据【保留在磁盘上】, 供下一个节点接着下。
// label 仅用于进度行显示, 让用户看清当前在用哪个节点。
static bool DownloadOnceResume(const std::string& url, const std::wstring& outPath,
    bool showProgress, long long expectSize, const std::string& label, long long& gotOut) {
    gotOut = 0;
    UrlParts up;
    if (!CrackUrl(url, up)) return false;

    long long have = LocalFileSize(outPath);
    if (have < 0) have = 0;

    HINTERNET hSess = WinHttpOpen(L"cdccmd/" L"" CDCCMD_VERSION,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) return false;
    HINTERNET hConn = WinHttpConnect(hSess, up.host.c_str(), up.port, 0);
    if (!hConn) { WinHttpCloseHandle(hSess); return false; }
    DWORD flags = up.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", up.path.c_str(), NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return false; }

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hReq, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));
    DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
    WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));

    if (have > 0) {
        char rng[96];
        sprintf(rng, "Range: bytes=%lld-\r\n", have);
        std::wstring wr = ToWide(rng);
        WinHttpAddRequestHeaders(hReq, wr.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, NULL)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        return false;
    }

    DWORD code = 0, csz = sizeof(code);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        NULL, &code, &csz, NULL);

    // 416: 请求区间越界。若恰好已是完整长度, 视为已下载完成
    if (code == 416) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        if (expectSize > 0 && have >= expectSize) { gotOut = have; return true; }
        return false;
    }
    if (code != 200 && code != 206) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        return false;
    }

    long long total = 0;
    wchar_t cl[32] = { 0 }; DWORD clen = sizeof(cl);
    if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_LENGTH, NULL, cl, &clen, NULL))
        total = _wtoi64(cl);

    // 206 = 服务端接受了 Range, 接着写; 200 = 忽略 Range, 只能从头写
    bool append = (code == 206 && have > 0);
    long long base = append ? have : 0;
    if (!append && have > 0) {
        // 服务端不支持续传, 之前的残缺数据作废
        have = 0;
    }

    std::ios::openmode mode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
    std::ofstream ofs(outPath.c_str(), mode);
    if (!ofs) { WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return false; }

    long long grand = (total > 0) ? (base + total) : 0;   // 预期最终总大小
    long long got = 0;
    DWORD avail = 0;
    char buf[32768];
    ULONGLONG lastTick = 0;
    int lastPct = -2;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        DWORD toRead = (avail > sizeof(buf)) ? (DWORD)sizeof(buf) : avail;
        DWORD rd = 0;
        if (!WinHttpReadData(hReq, buf, toRead, &rd) || rd == 0) break;
        ofs.write(buf, rd);
        if (!ofs.good()) break;
        got += rd;
        if (showProgress) {
            ULONGLONG now = GetTickCount64();
            if (now - lastTick < 200) continue;
            lastTick = now;
            long long cur = base + got;
            int pct = (grand > 0) ? (int)(cur * 100 / grand) : -1;
            if (pct == lastPct) continue;
            lastPct = pct;
            char line[224];
            if (grand > 0) {
                int bars = 24, filled = (int)((__int64)pct * bars / 100);
                std::string b;
                for (int i = 0; i < bars; i++) b += (i < filled ? '#' : '-');
                sprintf(line, "\r[%-22s][%s] %3d%%  %.1f/%.1f MB",
                    label.c_str(), b.c_str(), pct, cur / 1048576.0, grand / 1048576.0);
            } else {
                sprintf(line, "\r[%-22s]已下载 %.1f MB", label.c_str(), cur / 1048576.0);
            }
            fputs(line, stdout); fflush(stdout);
        }
    }
    ofs.flush();
    ofs.close();
    if (showProgress) { fputc('\n', stdout); fflush(stdout); }

    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);

    long long done = base + got;
    gotOut = done;

    if (got <= 0) return false;                      // 一个字节都没拿到

    if (total > 0 && got != total) return false;     // 连接中途断了 -> 保留数据供续传

    // 关键: 代理节点可能返回"错误页"(HTTP 200 + 一段 HTML), 此时 total 与 got 自洽,
    // 只看 Content-Length 会把几十 KB 的错误页当成下载成功 -> 用真实文件大小兜底。
    if (expectSize > 0 && done != expectSize) {
        // 拿到了"完整但大小不对"的响应, 基本可断定不是目标文件。
        // 必须删掉, 否则这堆垃圾会被当成"已下载部分", 污染后续节点的断点续传。
        if (total > 0 || base == 0) {
            DeleteFileW(outPath.c_str());
            gotOut = 0;
        }
        return false;
    }
    return true;
}

// 全节点下载: 按给定顺序逐个尝试, 每个节点都从"当前已下载字节"续传
static bool DownloadMulti(const std::vector<NameUrl>& cands, const std::wstring& outPath,
    bool showProgress, long long expectSize, const std::string& what) {
    if (cands.empty()) return false;
    int i = 0;
    for (auto& c : cands) {
        ++i;
        long long have = LocalFileSize(outPath);
        if (have < 0) have = 0;
        char b[256];
        if (have > 0)
            sprintf(b, "[下载] %s (%d/%d) 续传自 %s\n", c.first.c_str(), i, (int)cands.size(),
                HumanSize(have).c_str());
        else
            sprintf(b, "[下载] %s (%d/%d)\n", c.first.c_str(), i, (int)cands.size());
        Say(b);

        long long got = 0;
        if (DownloadOnceResume(c.second, outPath, showProgress, expectSize, c.first, got))
            return true;

        char b2[192];
        sprintf(b2, "   -> 该节点未完成 (已获 %s), 自动切换下一个 ...\n", HumanSize(got).c_str());
        Say(b2);
    }
    (void)what;
    return false;
}

// 解析 body 字段, 与 JsonStrAfter 的区别: 把 \n \r \t 还原成真实控制字符,
// 这样 release 说明能按原文换行显示(JsonStrAfter 会把 \n 吞成 'n')
static bool JsonBodyAfter(const std::string& s, size_t from, std::string& out) {
    std::string pat = "\"body\"";
    size_t k = s.find(pat, from);
    if (k == std::string::npos) return false;
    size_t colon = s.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    size_t q1 = s.find('"', colon + 1);
    if (q1 == std::string::npos) return false;
    std::string v;
    size_t i = q1 + 1;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == 'n')       v += '\n';
            else if (n == 'r')  v += '\r';
            else if (n == 't')  v += ' ';
            else if (n == '"')  v += '"';
            else if (n == '/')  v += '/';
            else if (n == '\\') v += '\\';
            else if (n == 'u')  { i += 6; continue; }   // \uXXXX 跳过(够用即可)
            else                v += n;
            i += 2;
            continue;
        }
        if (c == '"') break;
        v += c;
        ++i;
    }
    out = v;
    return true;
}

// ---------------------------------------------------------------- Release 列表解析

struct ReleaseInfo {
    std::string tag;
    std::string name;
    std::string published;
    std::string body;
    bool prerelease = false;
    std::vector<Asset> assets;
};

// 解析 /releases 返回的数组(每项一个 release)。
// 做法: 定位所有 "tag_name", 以相邻两个为界切段, 段内再取其余字段。
static std::vector<ReleaseInfo> ParseReleaseList(const std::string& json) {
    std::vector<ReleaseInfo> out;
    std::vector<size_t> tags;
    for (size_t p = json.find("\"tag_name\""); p != std::string::npos;
         p = json.find("\"tag_name\"", p + 1))
        tags.push_back(p);
    if (tags.empty()) return out;

    for (size_t i = 0; i < tags.size(); ++i) {
        size_t begin = tags[i];
        size_t end = (i + 1 < tags.size()) ? tags[i + 1] : json.size();

        ReleaseInfo ri;
        JsonStrAfter(json, begin, "tag_name", ri.tag);
        {
            std::string nm;
            size_t np = json.find("\"name\"", begin);
            if (np != std::string::npos && np < end) JsonStrAfter(json, np, "name", nm);
            ri.name = nm;
        }
        JsonStrAfter(json, begin, "published_at", ri.published);
        {
            std::string bd;
            size_t bp = json.find("\"body\"", begin);
            if (bp != std::string::npos && bp < end) {
                if (JsonBodyAfter(json, bp, bd) && bd.size() > 600)
                    bd = bd.substr(0, 600) + "...";
            }
            ri.body = bd;
        }
        {
            size_t pp = json.find("\"prerelease\"", begin);
            if (pp != std::string::npos && pp < end) {
                size_t c = json.find(':', pp);
                if (c != std::string::npos) {
                    size_t v = json.find_first_not_of(" \t\r\n", c + 1);
                    if (v != std::string::npos && v < end)
                        ri.prerelease = (json.compare(v, 4, "true") == 0);
                }
            }
        }

        std::string seg = json.substr(begin, end - begin);
        ri.assets = ParseAssets(seg);
        out.push_back(ri);
    }
    return out;
}

// 判断响应体是否"看起来"是 GitHub release 的真 JSON。
// 用来挡掉代理节点返回的垃圾内容(kkgithub 的假 404 JSON / ghproxy.net 的 HTML)。
static bool LooksLikeReleaseJson(const std::string& body) {
    size_t i = body.find_first_not_of(" \t\r\n");
    if (i == std::string::npos) return false;
    if (body[i] != '{' && body[i] != '[') return false;
    if (Contains(body, "<!DOCTYPE") || Contains(body, "<html")) return false;
    if (Contains(body, "\"tag_name\"") || Contains(body, "\"assets\"")) return true;
    // 假 404: {"code":404,...}
    if (Contains(body, "\"code\":404")) return false;
    return false;
}

// 多通道取 release JSON: 直连优先, 再逐个代理, 且校验内容。
// apiPath 形如 "repos/owner/name/releases/latest"
// outStatus: 200=成功, 404=发布页不存在, 0=全部通道失败
static bool FetchReleaseJson(const std::string& apiPath, std::string& outBody,
    std::string& outVia, int* outStatus = nullptr) {
    if (outStatus) *outStatus = 0;
    const std::string base = "https://api.github.com/" + apiPath;

    std::vector<NameUrl> via;
    via.push_back(NameUrl("GitHub 官方", base));
    for (int i = 0; i < kNODE_COUNT; ++i) {
        if (!kNODES[i].api) continue;
        if (!kNODES[i].tpl[0]) continue;               // 跳过直连(已加)
        via.push_back(NameUrl(kNODES[i].name, WrapNode(kNODES[i], base)));
    }

    for (auto& v : via) {
        HttpResponse r = HttpGet(v.second, GhHeaders(), false, true);   // quiet
        if (r.status == 404) {
            // 该通道能连通且明确回答"没有" -> 认定为未建发布页
            outVia = v.first;
            outBody = r.body;
            if (outStatus) *outStatus = 404;
            return false;
        }
        if (!r.ok) continue;
        if (!LooksLikeReleaseJson(r.body)) continue;    // 假响应, 换下一条
        outBody = r.body;
        outVia = v.first;
        if (outStatus) *outStatus = 200;
        return true;
    }
    return false;
}

// ------------------------------- 以下供 git clone 使用

// 轻量探测(带计时), 直接挑出最快的节点再 clone。探测失败则退回顺序尝试。

struct Mirror { std::string name; std::string url; };

// 把用户输入的 GitHub 地址展开成一组候选镜像
static std::vector<Mirror> BuildMirrors(const std::string& raw) {
    std::string u = raw;
    // 补全协议
    if (!StartsWith(u, "http://") && !StartsWith(u, "https://") &&
        !StartsWith(u, "git@")) {
        if (Contains(u, "github.com") || Contains(u, "."))
            u = "https://" + u;
    }
    std::string base = u;
    if (EndsWith(base, ".git")) base = base.substr(0, base.size() - 4);

    std::vector<Mirror> v;
    auto add = [&](const std::string& n, const std::string& x) {
        if (x.empty()) return;
        for (auto& m : v) if (m.url == x) return;
        v.push_back({ n, x });
    };

    // 非 GitHub 地址: 直接原样克隆, 不上镜像
    if (!Contains(base, "github.com")) {
        add("原地址", base + ".git");
        return v;
    }

    std::string owner_repo = base.substr(base.find("github.com") + strlen("github.com"));
    if (!owner_repo.empty() && owner_repo[0] == '/') owner_repo = owner_repo.substr(1);

    add("GitHub 官方",  "https://github.com/" + owner_repo + ".git");
    // 统一走节点表, 新增节点自动对所有命令生效
    std::string rawRepo = "https://github.com/" + owner_repo + ".git";
    for (int i = 0; i < kNODE_COUNT; ++i) {
        if (!kNODES[i].git) continue;
        if (!kNODES[i].tpl[0]) continue;          // 直连已在上面加过
        add(kNODES[i].name, WrapNode(kNODES[i], rawRepo));
    }
    return v;
}

// 用 info/refs 探测延迟(毫秒); 失败返回 -1
static int ProbeMirror(const std::string& gitUrl) {
    std::string probe = gitUrl + "/info/refs?service=git-upload-pack";
    UrlParts up;
    if (!CrackUrl(probe, up)) return -1;

    ULONGLONG t0 = GetTickCount64();
    HINTERNET hSess = WinHttpOpen(L"cdccmd-probe", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) return -1;
    int ms = -1;
    HINTERNET hConn = WinHttpConnect(hSess, up.host.c_str(), up.port, 0);
    if (hConn) {
        DWORD flags = up.https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", up.path.c_str(), NULL,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hReq) {
            int to = 4000;
            WinHttpSetOption(hReq, WINHTTP_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
            WinHttpSetOption(hReq, WINHTTP_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
            WinHttpSetOption(hReq, WINHTTP_OPTION_SEND_TIMEOUT, &to, sizeof(to));
            DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(hReq, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));
            DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
            WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
            if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
                WinHttpReceiveResponse(hReq, NULL)) {
                DWORD code = 0, sz = sizeof(code);
                WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    NULL, &code, &sz, NULL);
                if (code >= 200 && code < 400)
                    ms = (int)(GetTickCount64() - t0);
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConn);
    }
    WinHttpCloseHandle(hSess);
    return ms;
}

static bool HasGit() {
    DWORD ec = 1;
    if (!RunAndWait("git --version >nul 2>&1", &ec)) return false;
    return ec == 0;
}

static void CmdClone(const std::string& repoUrl, const std::string& target) {
    if (!HasGit()) {
        SayLn("[错误] 未检测到 Git。请先安装 Git for Windows 并确保 git 在 PATH 中。");
        SayLn("       下载: https://git-scm.com/download/win");
        return;
    }
    if (!repoUrl.empty())
        Say(("[信息] 目标仓库: " + repoUrl + "\n").c_str());

    std::vector<Mirror> mirrors = BuildMirrors(repoUrl);
    if (mirrors.empty()) { SayLn("[错误] 仓库地址无效"); return; }

    SayLn("[1/2] 正在测速, 寻找最快节点 ...");
    struct Cand { Mirror m; int ms; };
    std::vector<Cand> alive;
    for (auto& m : mirrors) {
        Say(("[  探测] " + m.name + " ... ").c_str());
        int ms = ProbeMirror(m.url);
        if (ms >= 0) {
            char b[64]; sprintf(b, "%d ms\n", ms);
            Say(b);
            alive.push_back({ m, ms });
        } else {
            SayLn("超时/不可用");
        }
    }

    std::sort(alive.begin(), alive.end(),
        [](const Cand& a, const Cand& b) { return a.ms < b.ms; });

    // 探测全灭 -> 顺序盲试
    std::vector<std::string> order;
    if (!alive.empty()) {
        Say(("[信息] 最快节点: " + alive[0].m.name + "\n").c_str());
        for (auto& c : alive) order.push_back(c.m.url);
    } else {
        SayLn("[警告] 所有节点测速均失败, 改为逐个尝试 ...");
        for (auto& m : mirrors) order.push_back(m.url);
    }

    SayLn("[2/2] 开始克隆 ...");
    int idx = 1;
    for (auto& u : order) {
        Say(("[尝试 " + std::to_string(idx) + "/" + std::to_string(order.size()) + "] " + u + "\n").c_str());
        std::string cmd = "git clone --progress \"" + u + "\"";
        if (!target.empty()) cmd += " \"" + target + "\"";
        DWORD ec = 1;
        if (!RunAndWait(cmd, &ec)) { SayLn("[错误] 无法启动 git 进程"); return; }
        if (ec == 0) {
            SayLn("[成功] 克隆完成!");
            return;
        }
        SayLn("  -> 该节点失败, 切换下一个 ...");
        ++idx;
    }
    SayLn("[失败] 所有节点均无法克隆。请检查网络, 或改用 SSH 地址。");
}

// ---------------------------------------------------------------- 功能 2: cdc download

static void CmdDownloadCDC() {
    const char* repo = CDCCMD_MAIN_REPO;
    SayLn("[信息] 正在查询 " CDCCMD_MAIN_NAME " 最新发布 ...");

    std::string body, via;
    int st = 0;
    if (!FetchReleaseJson(std::string("repos/") + repo + "/releases/latest", body, via, &st)) {
        if (st == 404) {
            SayLn("[错误] 未找到 " CDCCMD_MAIN_NAME " 的发布页 (404)。");
        } else {
            SayLn("[错误] 获取发布信息失败 (网络不通 / 被墙 / API 限流)。");
            SayLn("       可设置环境变量 CDC_GH_TOKEN 为你的 GitHub Token 后重试。");
        }
        return;
    }
    Say(("[信息] 数据来源: " + via + "\n").c_str());

    std::vector<Asset> as = ParseAssets(body);
    std::string tag = JsonTagName(body);
    const Asset* a = PickInstaller(as);
    if (!a) {
        SayLn("[错误] 最新发布中未找到 .exe 安装包。");
        return;
    }
    Say(("[信息] 最新版本: " + (tag.empty() ? "(未知)" : tag) + "\n").c_str());
    Say(("[信息] 安装包名: " + a->name + "\n").c_str());
    if (a->size > 0) Say(("[信息] 文件大小: " + HumanSize(a->size) + "\n").c_str());

    std::wstring dest = DownloadsDir() + L"\\" + ToWide(a->name);

    // 本地已有完整包 -> 直接打开, 省一次 100+MB 的下载
    long long have = LocalFileSize(dest);
    if (have > 0 && a->size > 0 && have == a->size) {
        Say("[信息] 本地已有完整安装包: "); SayLn(NarrowPath(dest).c_str());
        SayLn("[完成] 正在打开安装包 ...");
        HINSTANCE h = ShellExecuteW(NULL, L"open", dest.c_str(), NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)h <= 32) SayLn("[错误] 无法自动打开, 请手动到下载文件夹运行。");
        return;
    }
    if (have > 0) {
        char b[128];
        sprintf(b, "[信息] 检测到未完成的旧文件 (%s), 将从断点继续下载。\n", HumanSize(have).c_str());
        Say(b);
    }

    Say("[下载] 保存到: "); SayLn(NarrowPath(dest).c_str());

    // --- 核心: 全节点测速, 挑最快; 失败自动切换并断点续传 ---
    std::vector<NameUrl> cands = CollectNodes(a->url, false /*api*/, true /*file*/, false);
    cands = RankBySpeed(cands, "安装包", true);

    if (!DownloadMulti(cands, dest, true, a->size, a->name)) {
        SayLn("[错误] 所有加速节点均下载失败, 请稍后重试或检查网络。");
        return;
    }
    SayLn("[完成] 下载完成, 正在打开安装包 ...");
    HINSTANCE h = ShellExecuteW(NULL, L"open", dest.c_str(), NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32) SayLn("[错误] 无法自动打开, 请手动到下载文件夹运行。");
}

// ---------------------------------------------------------------- 功能 3: 打开网址

static bool LooksLikeUrl(const std::string& s) {
    if (StartsWith(s, "http://") || StartsWith(s, "https://") ||
        StartsWith(s, "www.") || StartsWith(s, "ftp://")) return true;
    // 形如 example.com / example.com/path 且带点
    if (Contains(s, ".") && !Contains(s, " ") && !Contains(s, "\\")) return true;
    return false;
}

static void OpenUrl(std::string s) {
    // 裸域名(example.com / example.com/path)补上 https://
    if (!StartsWith(s, "http://") && !StartsWith(s, "https://") &&
        !StartsWith(s, "ftp://"))
        s = "https://" + s;
    std::wstring w = ToWide(s);
    HINSTANCE h = ShellExecuteW(NULL, L"open", w.c_str(), NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32) SayLn("[错误] 打开失败, 请确认该网址有效。");
    else Say(("[完成] 已用默认浏览器打开: " + s + "\n").c_str());
}

// ---------------------------------------------------------------- 功能 4: jcgx 自更新

// 写一个延迟执行的批处理: 等本进程退出 -> 覆盖自身 -> 删除自己
static bool SpawnSelfReplace(const std::wstring& newFile, const std::wstring& curFile,
    const std::string& tagMsg) {
    std::wstring bat = TempDir() + L"cdccmd_selfupdate.bat";
    std::ofstream f(NarrowPath(bat).c_str(), std::ios::binary);
    if (!f) { SayLn("[错误] 无法写入更新脚本"); return false; }
    f << "@echo off\r\n"
      << "chcp 65001 >nul\r\n"
      << ":wait\r\n"
      << "tasklist /FI \"IMAGENAME eq cdccmd.exe\" 2>nul | find /I \"cdccmd.exe\" >nul\r\n"
      << "if not errorlevel 1 (ping -n 2 127.0.0.1 >nul & goto wait)\r\n"
      << "ping -n 2 127.0.0.1 >nul\r\n"
      << "copy /y \"" << NarrowPath(newFile) << "\" \"" << NarrowPath(curFile) << "\" >nul\r\n"
      << "del \"" << NarrowPath(newFile) << "\" >nul 2>&1\r\n"
      << "echo " << tagMsg << "\r\n";
    f.close();

    // 自删本脚本: 另起隐藏 cmd 延迟删除 (cmd 删不掉正在执行的 .bat)
    std::wstring killer = TempDir() + L"cdccmd_upd_self.bat";
    std::ofstream kf(NarrowPath(killer).c_str(), std::ios::binary);
    if (kf) {
        kf << "@echo off\r\n"
           << "ping -n 3 127.0.0.1 >nul\r\n"
           << "del /f /q \"" << NarrowPath(bat) << "\"\r\n"
           << "del /f /q \"" << NarrowPath(killer) << "\"\r\n";
        kf.close();
        std::string cl = "cmd /c \"" + NarrowPath(killer) + "\"";
        STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
        std::vector<char> cb(cl.begin(), cl.end()); cb.push_back('\0');
        if (CreateProcessA(NULL, cb.data(), NULL, NULL, FALSE,
                CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
    }
    HINSTANCE h = ShellExecuteW(NULL, L"open", bat.c_str(), NULL, NULL, SW_HIDE);
    return (INT_PTR)h > 32;
}

static bool AskYesNo(const char* prompt) {
    Say(prompt);
    std::string s;
    if (!std::getline(std::cin, s)) return false;
    return !s.empty() && (s[0] == 'Y' || s[0] == 'y');
}

// 逐行缩进打印 release 说明
static void PrintIndented(const std::string& text, const char* indent) {
    std::string cur;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            Say(indent);
            SayLn(cur.c_str());
            cur.clear();
            continue;
        }
        if (text[i] == '\r') continue;
        cur += text[i];
    }
}

static void CmdSelfUpdate() {
    SayLn("[信息] 正在检查 CDCCMD 更新 ...");

    std::string body, via;
    int st = 0;
    // 用 /releases 列表而不是 /releases/latest: 能拿到更完整的信息(时间/说明/资产清单)
    if (!FetchReleaseJson(std::string("repos/") + CDCCMD_REPO + "/releases", body, via, &st)) {
        if (st == 404) SayLn("[提示] 尚未创建 CDCCMD 发布页, 无需更新。");
        else {
            SayLn("[错误] 获取更新信息失败 (网络不通 / 被墙 / API 限流)。");
            SayLn("       可设置环境变量 CDC_GH_TOKEN 为你的 GitHub Token 后重试。");
        }
        return;
    }
    Say(("[信息] 数据来源: " + via + "\n").c_str());

    std::vector<ReleaseInfo> rels = ParseReleaseList(body);
    if (rels.empty()) { SayLn("[提示] 发布页中没有可用版本。"); return; }

    // 列表按发布时间倒序; 取第一个"非预发布"作为最新正式版
    ReleaseInfo* best = nullptr;
    for (auto& r : rels) {
        if (r.tag.empty() || r.prerelease) continue;
        best = &r;
        break;
    }
    if (!best) best = &rels[0];

    // ---- 详细检查报告 ----
    SayLn("[信息] ------------------- 版本检查 -------------------");
    Say(("  当前版本 : " + std::string(CDCCMD_VERSION) + "\n").c_str());
    Say(("  最新版本 : " + (best->tag.empty() ? std::string("(未知)") : best->tag) + "\n").c_str());
    if (!best->name.empty() && best->name != best->tag)
        Say(("  版本名称 : " + best->name + "\n").c_str());
    if (!best->published.empty())
        Say(("  发布时间 : " + best->published + "\n").c_str());
    Say(("  发布类型 : " + std::string(best->prerelease ? "预发布" : "正式版") + "\n").c_str());
    Say(("  历史版本 : 共 " + std::to_string(rels.size()) + " 个\n").c_str());

    if (!best->assets.empty()) {
        SayLn("  发布文件 :");
        for (auto& x : best->assets) {
            char b[320];
            sprintf(b, "    - %-36s %10s\n", x.name.c_str(), HumanSize(x.size).c_str());
            Say(b);
        }
    } else {
        SayLn("  发布文件 : (无)");
    }
    if (!best->body.empty()) {
        SayLn("  更新说明 :");
        PrintIndented(best->body, "    ");
    }
    SayLn("[信息] --------------------------------------------------");

    if (!IsNewer(best->tag, CDCCMD_VERSION)) {
        SayLn("[完成] 已是最新版本, 无需更新。");
        return;
    }

    // 挑资产: 优先 cdccmd-setup.exe, 其次任意 exe
    const Asset* a = nullptr;
    for (auto& x : best->assets)
        if (ToLower(x.name) == "cdccmd-setup.exe") { a = &x; break; }
    if (!a)
        for (auto& x : best->assets)
            if (EndsWith(ToLower(x.name), ".exe")) { a = &x; break; }
    if (!a) { SayLn("[提示] 该版本没有可下载的安装包 (仅源码)。"); return; }

    if (!AskYesNo(("[询问] 发现新版本 " + best->tag + ", 是否更新? (Y/N) ").c_str())) {
        SayLn("[取消] 已取消更新。");
        return;
    }

    std::wstring newFile = TempDir() + L"cdccmd_new.exe";
    // 上次残留的临时文件清掉, 避免断点续传带到旧内容
    DeleteFileW(newFile.c_str());

    Say(("[下载] 目标: " + a->name + "  (" + HumanSize(a->size) + ")\n").c_str());
    std::vector<NameUrl> cands = CollectNodes(a->url, false, true, false);
    cands = RankBySpeed(cands, "更新包", true);

    if (!DownloadMulti(cands, newFile, true, a->size, a->name)) {
        SayLn("[错误] 所有加速节点均下载失败。");
        return;
    }

    // 若新版是"自包含安装包", 直接运行它即可完成覆盖安装
    if (ToLower(a->name) == "cdccmd-setup.exe") {
        SayLn("[安装] 正在启动新版安装包完成覆盖安装 ...");
        HINSTANCE h = ShellExecuteW(NULL, L"open", newFile.c_str(), NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)h <= 32) SayLn("[错误] 无法启动安装包, 请手动运行临时目录中的 cdccmd_new.exe");
        return;
    }

    // 否则按"替换当前 exe"处理
    if (SpawnSelfReplace(newFile, ModulePath(), "[完成] CDCCMD 已更新到 " + best->tag)) {
        SayLn("[完成] 更新已就绪, 程序退出后自动替换。");
    } else {
        SayLn("[错误] 无法启动更新脚本。");
    }
}

// ---------------------------------------------------------------- 获取 Releases 列表

// 列出仓库的发布版本 (对标 github.akams.cn 的"获取 Releases 列表")
static void CmdReleases(int limit) {
    if (limit <= 0) limit = 10;
    const char* repo = CDCCMD_MAIN_REPO;
    Say(("[信息] 正在获取 " + std::string(repo) + " 的发布列表 ...\n").c_str());

    std::string body, via;
    int st = 0;
    if (!FetchReleaseJson(std::string("repos/") + repo + "/releases", body, via, &st)) {
        if (st == 404) SayLn("[错误] 未找到该仓库的发布页 (404)。");
        else SayLn("[错误] 获取发布列表失败 (网络不通 / 被墙 / API 限流)。");
        return;
    }
    Say(("[信息] 数据来源: " + via + "\n").c_str());

    std::vector<ReleaseInfo> rels = ParseReleaseList(body);
    if (rels.empty()) { SayLn("[提示] 该仓库还没有任何发布。"); return; }

    SayLn("");
    int shown = 0;
    for (auto& r : rels) {
        if (shown >= limit) break;

        std::string assetName;
        long long assetSize = 0;
        for (auto& x : r.assets) {
            std::string low = ToLower(x.name);
            if (EndsWith(low, ".exe") || EndsWith(low, ".zip") || EndsWith(low, ".msi")) {
                assetName = x.name;
                assetSize = x.size;
                break;
            }
        }

        std::string line = "  [" + std::to_string(shown + 1) + "] " +
                           (r.tag.empty() ? std::string("(无标签)") : r.tag) +
                           "   " + std::string(r.prerelease ? "预发布" : "正式版");
        if (r.published.size() >= 10) line += "   " + r.published.substr(0, 10);
        SayLn(line.c_str());

        if (!assetName.empty()) {
            Say(("       安装包: " + assetName + "  (" + HumanSize(assetSize) + ")\n").c_str());
        } else {
            SayLn("       安装包: (无)");
        }
        ++shown;
    }
    SayLn("");
    Say(("  共 " + std::to_string(rels.size()) + " 个版本, 显示前 " +
         std::to_string(shown) + " 个。\n").c_str());
    SayLn("  提示: 用 cdc download 直接获取最新版安装包。");
}

// ---------------------------------------------------------------- 隐藏: 下载引擎自检

// 用法: cdccmd __dltest <原始URL> <输出文件> [预期大小]
// 只跑"测速 -> 加速下载 -> 断点续传 -> 失败切换"这一条链路, 不打开任何文件。
// 不在帮助里列出, 供开发/排障验证下载引擎用。
static void CmdDlTest(const std::string& url, const std::wstring& outPath, long long expectSize) {
    Say(("[信息] 原始地址: " + url + "\n").c_str());

    std::vector<NameUrl> cands = CollectNodes(url, false /*api*/, true /*file*/, false);
    if (cands.empty()) { SayLn("[错误] 没有可用节点"); return; }

    cands = RankBySpeed(cands, "测试文件", true);

    SayLn("[信息] 测速后尝试顺序:");
    for (size_t i = 0; i < cands.size(); ++i)
        Say(("  " + std::to_string(i + 1) + ". " + cands[i].first + "\n").c_str());

    long long have = LocalFileSize(outPath);
    if (have < 0) have = 0;
    Say(("[信息] 本地已有: " + HumanSize(have) + "\n").c_str());

    bool ok = DownloadMulti(cands, outPath, true, expectSize, "dltest");
    Say(ok ? "[结果] 下载成功\n" : "[结果] 下载失败\n");
    Say(("[信息] 最终文件大小: " + HumanSize(LocalFileSize(outPath)) + "\n").c_str());
}

// ---------------------------------------------------------------- 功能 5: uncdccmd

// 启动卸载程序并等它跑完。
// 为什么不用 ShellExecuteW: 它是异步的, 在受限环境(作业对象/沙箱)里经常
// "看起来启动了、其实进程没起来", 表现为卸载完全没发生(目录/PATH 都还在)。
// 用 CreateProcessW + WaitForSingleObject 可以确认它真的跑完, 而且卸载输出
// 直接显示在当前终端里, 用户能看到进度。
static bool LaunchAndWait(const std::wstring& exe, DWORD timeoutMs) {
    std::wstring cmd = L"\"" + exe + L"\"";
    std::vector<wchar_t> cb(cmd.begin(), cmd.end());
    cb.push_back(L'\0');

    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(NULL, cb.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, timeoutMs);
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return ec == 0;
    }

    // 兜底: 退回 ShellExecuteW (某些环境不让直接 CreateProcess)
    HINSTANCE h = ShellExecuteW(NULL, L"open", exe.c_str(), NULL, NULL, SW_SHOWNORMAL);
    return (INT_PTR)h > 32;
}

static void CmdUninstall() {
    std::wstring dir = ModuleDir();
    std::wstring un = dir + L"\\uninstall.exe";
    if (!FileExists(un)) {
        // 注册表里找
        HKEY hk;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\cdccmd",
            0, KEY_READ, &hk) == ERROR_SUCCESS) {
            wchar_t v[1024] = { 0 }; DWORD sz = sizeof(v);
            if (RegQueryValueExW(hk, L"UninstallString", NULL, NULL, (LPBYTE)v, &sz) == ERROR_SUCCESS) {
                un = v; RegCloseKey(hk);
                SayLn("[信息] 正在启动卸载程序 ...");
                if (!LaunchAndWait(un, 120000))
                    SayLn("[警告] 卸载程序未正常结束, 请检查是否已卸载。");
                return;
            }
            RegCloseKey(hk);
        }
        SayLn("[错误] 未找到卸载程序。若为绿色版, 直接删除安装目录即可。");
        Say("       安装目录: "); SayLn(NarrowPath(dir).c_str());
        return;
    }
    SayLn("[信息] 正在启动卸载程序 ...");
    if (!LaunchAndWait(un, 120000))
        SayLn("[警告] 卸载程序未正常结束, 请检查是否已卸载。");
}

// ---------------------------------------------------------------- 帮助

static void PrintHelp() {
    printf("CDCCMD - 多功能命令行工具 v%s\n", CDCCMD_VERSION);
    SayLn("=========================================================");
    SayLn("用法:  cdccmd <命令> [参数]");
    SayLn("");
    SayLn("  cdcgb <仓库URL> [目录]   克隆 GitHub 仓库 (自动测速选最快节点)");
    SayLn("  cdc download             下载并打开主项目最新安装包");
    SayLn("  cdc releases [数量]      列出所有发布版本 (默认 10 个)");
    SayLn("  jcgx                     检查 CDCCMD 自身更新 (输入 Y 确认更新)");
    SayLn("  uncdccmd                 打开卸载程序");
    SayLn("  <有效网址>               用默认浏览器打开网址");
    SayLn("  help                     显示本帮助");
    SayLn("=========================================================");
    SayLn("下载加速:");
    SayLn("  所有下载都会先给加速节点测速, 自动挑最快的那个;");
    SayLn("  若某节点中途失败, 会自动切换到下一个节点并从断点续传。");
    SayLn("");
    SayLn("示例:");
    SayLn("  cdcgb https://github.com/your-github-name/your-repo");
    SayLn("  cdcgb https://github.com/your-github-name/your-repo D:\\codes");
    SayLn("  cdc download");
    SayLn("  cdc releases 20");
    SayLn("  cdccmd https://github.com/your-github-name");
    SayLn("  jcgx");
    SayLn("  uncdccmd");
    SayLn("");
    SayLn("环境变量(可选):");
    SayLn("  CDC_GH_TOKEN   设置 GitHub Token 可提高 API 限流额度");
    SayLn("  CDC_NO_SPEED   设为 1 时跳过节点测速, 直接按顺序尝试");
}

// ---------------------------------------------------------------- 入口

// 从 argv[0] 的文件名主干推断身份: 支持 cdc.cmd / cdcgb.cmd 等转发脚本
// 场景: shim 用 copy /H 硬链接或副本, 以 "cdc.exe" 调用本程序, 此时按 argv[0] 分派
static std::string NameFromArgv0(const char* a0) {
    std::string s = a0 ? a0 : "";
    // 取最后一个路径分隔符之后
    size_t p = s.find_last_of("\\/");
    if (p != std::string::npos) s = s.substr(p + 1);
    // 去掉 .exe 后缀
    if (s.size() > 4) {
        std::string ext = ToLower(s.substr(s.size() - 4));
        if (ext == ".exe") s = s.substr(0, s.size() - 4);
    }
    return ToLower(s);
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(GetConsoleOutputCP());

    // ---- 按 argv[0] 分派: 裸敲 "cdc" / "jcgx" / "cdcgb" / "uncdccmd" 时走这里 ----
    const std::string me = NameFromArgv0(argc > 0 ? argv[0] : "");
    if (me == "cdc") {
        if (argc >= 2) {
            std::string sub = ToLower(argv[1]);
            if (sub == "download") { CmdDownloadCDC(); return 0; }
            if (sub == "releases" || sub == "list") {
                CmdReleases(argc >= 3 ? atoi(argv[2]) : 10);
                return 0;
            }
        }
        SayLn("用法: cdc download            下载并打开最新安装包");
        SayLn("      cdc releases [数量]    列出所有发布版本");
        return 1;
    }
    if (me == "jcgx")    { CmdSelfUpdate();  return 0; }
    if (me == "uncdccmd"){ CmdUninstall();   return 0; }
    if (me == "cdcgb")   {
        if (argc < 2) {
            SayLn("用法: cdcgb <github仓库URL> [本地存放目录]");
            SayLn("示例: cdcgb https://github.com/your-github-name/your-repo D:\\codes");
            return 1;
        }
        CmdClone(argv[1], argc >= 3 ? argv[2] : "");
        return 0;
    }

    if (argc < 2) { PrintHelp(); return 0; }

    std::string c1 = ToLower(argv[1]);

    // 隐藏自检命令(不列在 help 里)
    if (c1 == "__dltest" && argc >= 4) {
        CmdDlTest(argv[2], ToWide(argv[3]), argc >= 5 ? _strtoi64(argv[4], nullptr, 10) : 0);
        return 0;
    }

    if (c1 == "help" || c1 == "-h" || c1 == "--help" || c1 == "/?" || c1 == "-help") {
        PrintHelp();
    } else if (c1 == "cdcgb") {
        if (argc < 3) {
            SayLn("用法: cdcgb <github仓库URL> [本地存放目录]");
            SayLn("示例: cdcgb https://github.com/your-github-name/your-repo D:\\codes");
            return 1;
        }
        CmdClone(argv[2], argc >= 4 ? argv[3] : "");
    } else if (c1 == "cdc") {
        if (argc >= 3 && ToLower(argv[2]) == "download") {
            CmdDownloadCDC();
        } else if (argc >= 3 && (ToLower(argv[2]) == "releases" || ToLower(argv[2]) == "list")) {
            CmdReleases(argc >= 4 ? atoi(argv[3]) : 10);
        } else {
            SayLn("用法: cdc download            下载并打开最新安装包");
            SayLn("      cdc releases [数量]    列出所有发布版本");
            return 1;
        }
    } else if (c1 == "releases" || c1 == "list") {
        CmdReleases(argc >= 3 ? atoi(argv[2]) : 10);
    } else if (c1 == "jcgx") {
        CmdSelfUpdate();
    } else if (c1 == "uncdccmd") {
        CmdUninstall();
    } else if (LooksLikeUrl(argv[1])) {
        OpenUrl(argv[1]);
    } else {
        Say(("[错误] 未知命令: " + std::string(argv[1]) + "\n").c_str());
        PrintHelp();
        return 1;
    }
    return 0;
}
