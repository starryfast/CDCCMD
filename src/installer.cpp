// ============================================================================
//  installer.cpp -- CDCCMD 自包含安装包
//
//  自身内嵌 cdccmd.exe (资源 101) 与 uninstall.exe (资源 102)。
//  双击 -> 释放到 %LOCALAPPDATA%\Programs\cdccmd
//        -> 把该目录写入"用户 PATH"(无需管理员)
//        -> 写"添加/删除程序"注册表项
//  分发给用户只需这一个 exe。
//
//  编译:
//    windres payload.rc -O coff -o payload.o
//    g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++
//        -o cdccmd-setup.exe installer.cpp payload.o
//        -lshell32 -luser32 -ladvapi32 -lole32
// ============================================================================

#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <fstream>
#include <vector>

#include "version.h"   // ★ 所有可配置项都在这里

static void Say(const char* utf8) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wn <= 0) return;
    std::wstring w(wn, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], wn);
    UINT cp = GetConsoleOutputCP(); if (cp == 0) cp = CP_ACP;
    int an = WideCharToMultiByte(cp, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (an <= 0) return;
    std::string a(an, 0);
    WideCharToMultiByte(cp, 0, w.c_str(), -1, &a[0], an, nullptr, nullptr);
    a.pop_back();
    fputs(a.c_str(), stdout); fflush(stdout);
}
static void SayLn(const char* u) { Say(u); fputc('\n', stdout); fflush(stdout); }

static std::wstring GetInstallDir() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &p)) && p) {
        r = std::wstring(p) + L"\\Programs\\cdccmd";
        CoTaskMemFree(p);
    } else {
        wchar_t buf[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, buf)))
            r = std::wstring(buf) + L"\\Programs\\cdccmd";
        else
            r = L"C:\\cdccmd";
    }
    return r;
}

static bool ExtractRes(HMODULE hMod, WORD id, const std::wstring& outPath) {
    // RT_RCDATA 是窄字符宏 "RCDATA", FindResourceW 要宽字符 -> 显式转换
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!hRes) { SayLn("  [错误] 找不到内嵌资源"); return false; }
    HGLOBAL hG = LoadResource(hMod, hRes);
    if (!hG) return false;
    DWORD size = SizeofResource(hMod, hRes);
    LPVOID p = LockResource(hG);
    if (!p || size == 0) return false;
    std::ofstream ofs(outPath.c_str(), std::ios::binary);
    if (!ofs) { SayLn("  [错误] 无法写入文件 (被杀软占用?)"); return false; }
    ofs.write((const char*)p, size);
    ofs.close();
    return ofs.good();
}

static bool MakeDirRecursive(const std::wstring& dir) {
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t s = dir.find_last_of(L"\\/");
    if (s != std::wstring::npos && s > 2) MakeDirRecursive(dir.substr(0, s));
    return CreateDirectoryW(dir.c_str(), NULL) != 0;
}

static void AddToUserPath(const std::wstring& dir) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, NULL, 0,
        KEY_READ | KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS) return;
    wchar_t buf[8192] = { 0 };
    DWORD sz = sizeof(buf);
    std::wstring cur;
    if (RegQueryValueExW(hk, L"Path", NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
        cur = buf;

    std::wstring d = dir;
    while (!d.empty() && d.back() == L'\\') d.pop_back();

    bool found = false;
    size_t start = 0;
    while (start <= cur.size() && !cur.empty()) {
        size_t semi = cur.find(L';', start);
        std::wstring seg = (semi == std::wstring::npos)
            ? cur.substr(start) : cur.substr(start, semi - start);
        std::wstring t = seg;
        while (!t.empty() && (t.back() == L'\\' || t.back() == L' ')) t.pop_back();
        if (!t.empty() && _wcsicmp(t.c_str(), d.c_str()) == 0) { found = true; break; }
        if (semi == std::wstring::npos) break;
        start = semi + 1;
    }
    if (!found) {
        if (!cur.empty() && cur.back() != L';') cur += L';';
        cur += d;
        cur += L';';
        RegSetValueExW(hk, L"Path", 0, REG_SZ, (const BYTE*)cur.c_str(),
            (DWORD)((cur.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(hk);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
        SMTO_ABORTIFHUNG, 2000, NULL);
}

static void WriteUninstallReg(const std::wstring& dir) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\cdccmd",
        0, NULL, 0, KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS) return;
    auto set = [&](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(hk, name, 0, REG_SZ, (const BYTE*)v.c_str(),
            (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    };
    set(L"DisplayName", L"CDCCMD (命令行工具)");
    set(L"Publisher", L"" CDCCMD_PUBLISHER);
    set(L"DisplayVersion", L"" CDCCMD_VERSION);
    set(L"InstallLocation", dir);
    set(L"UninstallString", dir + L"\\uninstall.exe");
    set(L"QuietUninstallString", L"\"" + dir + L"\\uninstall.exe\" /quiet");
    set(L"DisplayIcon", dir + L"\\cdccmd.exe");
    set(L"URLInfoAbout", L"" CDCCMD_HOMEPAGE);
    DWORD one = 1;
    RegSetValueExW(hk, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegCloseKey(hk);
}

// 广播后, 顺便通知资源管理器刷新
static void NotifyShell() {
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
        SMTO_ABORTIFHUNG, 2000, NULL);
}

// ---------------------------------------------------------------------------
//  命令别名
//  Windows 只按"可执行文件名"解析 PATH, 不认 exe 的子命令。
//  cdccmd.exe 内部按 argv[0] 分派, 所以这里给它做硬链接即可(零额外磁盘占用)。
//
//  投放两个位置, 确保"整台电脑到处都能用":
//    1) 安装目录      —— 主位置, 卸载时一起删
//    2) WindowsApps   —— %LOCALAPPDATA%\Microsoft\WindowsApps 是 Windows 10/11
//                        默认就在 PATH 里的目录, 放这里 = 天然全局可用,
//                        且完全不依赖注册表 PATH(即使 PATH 被改坏也照样能敲)
//  硬链接失败(跨卷/文件系统不支持)退回复制; 再失败则写 .cmd 转发脚本兜底。
// ---------------------------------------------------------------------------
static const wchar_t* kAliases[] = { L"cdccmd", L"cdc", L"cdcgb", L"jcgx", L"uncdccmd" };
static const int kAliasCount = (int)(sizeof(kAliases) / sizeof(kAliases[0]));

static bool DirExistsW(const std::wstring& d) {
    DWORD a = GetFileAttributesW(d.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring WindowsAppsDir() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &p)) && p) {
        r = std::wstring(p) + L"\\Microsoft\\WindowsApps";
        CoTaskMemFree(p);
    }
    return r;
}

static int MakeAliases(const std::wstring& dir) {
    std::wstring src = dir + L"\\cdccmd.exe";
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;

    std::vector<std::wstring> targets;
    targets.push_back(dir);
    std::wstring wa = WindowsAppsDir();
    if (!wa.empty() && DirExistsW(wa)) targets.push_back(wa);

    int made = 0;
    for (int i = 0; i < kAliasCount; ++i) {
        const wchar_t* a = kAliases[i];
        bool anyOk = false;
        for (auto& t : targets) {
            std::wstring dst = t + L"\\" + a + L".exe";
            // 安装目录里的 cdccmd.exe 就是源文件本身, 绝不能删掉再建链接
            if (_wcsicmp(dst.c_str(), src.c_str()) == 0) { anyOk = true; continue; }
            DeleteFileW(dst.c_str());
            if (CreateHardLinkW(dst.c_str(), src.c_str(), NULL)) { anyOk = true; continue; }
            if (CopyFileW(src.c_str(), dst.c_str(), FALSE))      { anyOk = true; continue; }
            // 兜底: .cmd 转发脚本(指向安装目录的完整路径, 因为目标目录里没有主程序)
            std::wstring cmdPath = t + L"\\" + a + L".cmd";
            std::wofstream ofs(cmdPath.c_str(), std::ios::binary);
            if (ofs) {
                ofs << L"@echo off\r\n";
                ofs << L"\"" << src << L"\" " << a << L" %*\r\n";
                ofs.close();
                anyOk = true;
            }
        }
        if (anyOk) ++made;
    }
    return made;
}

static std::string NarrowPath(const std::wstring& w) {
    UINT cp = GetConsoleOutputCP(); if (cp == 0) cp = CP_ACP;
    int n = WideCharToMultiByte(cp, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(n, 0);
    WideCharToMultiByte(cp, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    s.pop_back();
    return s;
}

// 打印宽字符路径: 必须先转成 ANSI 再 Say, 绝不能用 wprintf("%s", wchar_t*)
// (mingw 下 VS 风格 %s 对 wchar_t* 只写首字节, 且零警告静默失败)
static void SayPath(const std::wstring& w) { Say(NarrowPath(w).c_str()); }

int main() {
    SetConsoleTitleW(L"CDCCMD 安装程序");
    printf("CDCCMD 安装程序 v%s\n", CDCCMD_VERSION);
    SayLn("=============================");
    SayLn("命令行工具");
    SayLn("");

    std::wstring dir = GetInstallDir();
    Say("[1/4] 创建安装目录: "); SayLn(NarrowPath(dir).c_str());
    if (!MakeDirRecursive(dir)) {
        SayLn("[错误] 无法创建安装目录。");
        SayLn("按回车键退出 ..."); getchar(); return 1;
    }

    HMODULE hMod = GetModuleHandleW(NULL);

    SayLn("[2/4] 释放 cdccmd.exe ...");
    if (!ExtractRes(hMod, 101, dir + L"\\cdccmd.exe")) {
        SayLn("[错误] 安装包可能已损坏或不完整。");
        SayLn("按回车键退出 ..."); getchar(); return 1;
    }
    SayLn("[3/4] 释放 uninstall.exe ...");
    if (!ExtractRes(hMod, 102, dir + L"\\uninstall.exe")) {
        SayLn("[警告] 卸载程序释放失败, 不影响使用(可手动删除目录)。");
    }

    SayLn("[4/5] 写入 PATH 与注册表 ...");
    AddToUserPath(dir);
    WriteUninstallReg(dir);

    SayLn("[5/5] 建立命令别名 cdccmd / cdc / cdcgb / jcgx / uncdccmd ...");
    int nAlias = MakeAliases(dir);
    if (nAlias < 5)
        SayLn("  [警告] 部分别名创建失败, 仍可用 cdccmd <子命令> 调用。");

    NotifyShell();

    SayLn("=============================");
    SayLn("[完成] 安装成功!");
    Say("  安装位置: "); SayPath(dir); SayLn("");
    SayLn("  已加入用户 PATH (无需管理员权限)。");
    SayLn("  命令别名同时放进了 WindowsApps 目录, 保证整台电脑");
    SayLn("  在任何终端(CMD/PowerShell/Git Bash/Win+R)都能直接调用。");
    SayLn("");
    SayLn("请【关闭本窗口后重新打开】命令提示符, 即可直接使用:");
    SayLn("    cdccmd help          查看全部命令");
    SayLn("    cdcgb <仓库URL>      克隆 GitHub 仓库 (自动选最快节点)");
    SayLn("    cdc download         下载主项目最新版 (自动加速)");
    SayLn("    cdc releases [数量]  列出所有发布版本");
    SayLn("    jcgx                 更新 CDCCMD 自身");
    SayLn("    uncdccmd             卸载");
    SayLn("");
    SayLn("提示: 老窗口不认新命令, 必须重开 CMD 窗口。");
    SayLn("");
    SayLn("按回车键退出 ...");
    getchar();
    return 0;
}
