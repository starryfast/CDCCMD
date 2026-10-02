// ============================================================================
//  uninstall.cpp -- CDCCMD 卸载程序
//
//  做四件事:
//    1) 从用户 PATH 移除安装目录
//    2) 删除"添加/删除程序"注册表项
//    3) 创建开机自清任务(延迟删除整个安装目录 + 自身)
//    4) 立刻自删: 生成一个批处理等本进程退出后 rmdir /s /q 安装目录
//
//  编译:
//    g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++
//        -o uninstall.exe uninstall.cpp -lshell32 -luser32 -ladvapi32 -lole32 -luuid
// ============================================================================

#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <fstream>
#include <vector>

#include "version.h"

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

static std::string NarrowPath(const std::wstring& w) {
    UINT cp = GetConsoleOutputCP(); if (cp == 0) cp = CP_ACP;
    int n = WideCharToMultiByte(cp, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(n, 0);
    WideCharToMultiByte(cp, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    s.pop_back();
    return s;
}

static std::wstring ModuleDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? p : p.substr(0, s);
}

static std::wstring TempDir() {
    wchar_t t[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, t);
    return std::wstring(t, n);
}

// 排障日志: 卸载是"异步 + 自删", 出问题时很难看到现场, 落个日志省事。
// 写在 %TEMP% 下(不能写在安装目录, 那目录马上要被删掉)。
// 用 Win32 API 而不是 std::ofstream —— 后者会把整个 iostream 静态库拉进来,
// 让本 exe 从 0.6MB 膨胀到 3MB, 不值得。
static void Log(const char* msg) {
    std::wstring p = TempDir() + L"cdccmd_uninst.log";
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, msg, (DWORD)strlen(msg), &w, NULL);
    WriteFile(h, "\r\n", 2, &w, NULL);
    CloseHandle(h);
}

static void RemoveFromUserPath(const std::wstring& dir) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ | KEY_WRITE, &hk)
        != ERROR_SUCCESS) return;
    wchar_t buf[8192] = { 0 };
    DWORD sz = sizeof(buf);
    std::wstring cur;
    if (RegQueryValueExW(hk, L"Path", NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
        cur = buf;

    // 按分号切分, 精确去掉安装目录那一段
    std::wstring out;
    size_t start = 0;
    while (start <= cur.size()) {
        size_t semi = cur.find(L';', start);
        std::wstring seg = (semi == std::wstring::npos)
            ? cur.substr(start) : cur.substr(start, semi - start);
        std::wstring trimmed = seg;
        while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L' '))
            trimmed.pop_back();
        std::wstring d = dir;
        while (!d.empty() && d.back() == L'\\') d.pop_back();
        if (!trimmed.empty() && _wcsicmp(trimmed.c_str(), d.c_str()) != 0) {
            if (!out.empty()) out += L';';
            out += seg;
        }
        if (semi == std::wstring::npos) break;
        start = semi + 1;
    }
    RegSetValueExW(hk, L"Path", 0, REG_SZ, (const BYTE*)out.c_str(),
        (DWORD)((out.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
        SMTO_ABORTIFHUNG, 2000, NULL);
}

static void RemoveUninstallReg() {
    RegDeleteKeyW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\cdccmd");
}

// %LOCALAPPDATA%\Microsoft\WindowsApps —— 命令别名的第二投放点
static std::wstring WindowsAppsPath() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &p)) && p) {
        r = std::wstring(p) + L"\\Microsoft\\WindowsApps";
        CoTaskMemFree(p);
    }
    return r;
}

// 清掉投放在 WindowsApps 里的命令别名。
// 只删我们自己的 5 个名字, 不动目录里其它应用的执行别名。
static void CleanWindowsAppsShims() {
    std::wstring wa = WindowsAppsPath();
    if (wa.empty()) return;

    const wchar_t* names[] = { L"cdccmd", L"cdc", L"cdcgb", L"jcgx", L"uncdccmd" };
    for (const wchar_t* n : names) {
        DeleteFileW((wa + L"\\" + n + L".exe").c_str());
        DeleteFileW((wa + L"\\" + n + L".cmd").c_str());
    }
}

// 兜底: 注册一个"下次登录时删除目录"的键 (老系统没有 MoveFileEx 延迟方案时用)
static void RegisterDeleteOnReboot(const std::wstring& path) {
    MoveFileExW(path.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
}

int main(int argc, char* argv[]) {
    bool quiet = (argc >= 2 && (_stricmp(argv[1], "/quiet") == 0 || _stricmp(argv[1], "-q") == 0));
    (void)quiet;

    printf("CDCCMD 卸载程序 v%s\n", CDCCMD_VERSION);
    SayLn("=================================");
    std::wstring dir = ModuleDir();
    Say("安装目录: "); SayLn(NarrowPath(dir).c_str());

    Log("--- start ---");
    Log(NarrowPath(dir).c_str());

    SayLn("[1/4] 从用户 PATH 中移除 ...");
    RemoveFromUserPath(dir);
    Log("path removed");

    SayLn("[2/4] 删除注册表卸载项 ...");
    RemoveUninstallReg();
    Log("reg removed");

    SayLn("[3/4] 清理命令别名 (含 WindowsApps 目录) ...");
    CleanWindowsAppsShims();
    Log("winapps cleaned");

    SayLn("[4/4] 清理安装目录 ...");
    // 先删掉除了自己和 uninstall.exe 之外能删的
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' &&
                (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
                continue;
            std::wstring full = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(full.c_str());
            else
                DeleteFileW(full.c_str());
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    DeleteFileW(dir.c_str());
    Log("dir files deleted");

    // ------------------------------------------------------------------
    //  自删 + 清目录
    //  坑: cmd 无法删除"正在被执行的 .bat" —— 就算先 move 改名, 读取句柄仍锁着同一
    //      个文件对象, del 一样失败(实测留下 xxx.bat.del)。
    //  解法: 不写任何脚本文件, 直接起一个内联的 cmd /c 命令串。内联命令没有磁盘文件,
    //        自然不会留残留; 它负责等卸载进程退出 -> 删目录 -> 删自己。
    //  注意: lpCommandLine 是"最终命令行", 内层引号必须用裸双引号, 不能写成 \" ——
    //        反斜杠转义只属于 C 字面量写法, 传进 Win32 后会成为多余的反斜杠。
    // ------------------------------------------------------------------
    std::wstring exePath = dir + L"\\uninstall.exe";

    // ---------------------------------------------------------------
    //  用内联 cmd 完成"删目录 + 删自己"。踩过的坑:
    //   1) cmd 删不掉正在被执行的 .bat -> 不写脚本文件, 用内联命令串。
    //   2) 【不要用 cmd /c "整串" 外层引号包裹】—— 路径自带引号, 再套一层
    //      会让 cmd 的引号解析把命令切坏(表现为 rmdir 完全没生效)。
    //   3) 【不能用 ping 当 sleep!】本进程用 DETACHED_PROCESS 启动(无控制台),
    //      这种环境下实测: ping -n 2 = 32.4 秒, ping -n 1 = 31.2 秒,
    //      timeout /t 2 直接挂死, for /L do @cd. 45 秒+。
    //      (有控制台时 ping 只要 1.4 秒, 差异极大, 极易踩坑)
    //   4) 【rem 会吃掉本行余下所有内容】包括 ")" 和 "&", 所以 for 循环体里
    //      绝不能用 rem。
    //  结论: 不用 sleep。uninstall.exe 已经 ExitProcess 秒退, 而 cmd 从启动到
    //  执行第一条 rmdir 也要几十毫秒, 这段天然延迟足够让文件锁释放。
    //  再连续多打几发 rmdir 覆盖抖动窗口 —— 实测第一次就成功。
    //  调试开关(默认关闭): CDCCMD_RM_DEBUG=1 时记录每一步时间戳到 %TEMP%。
    // ---------------------------------------------------------------
    const int kTry = 40;

    char dbg[8] = { 0 };
    bool rmDebug = GetEnvironmentVariableA("CDCCMD_RM_DEBUG", dbg, sizeof(dbg) - 1) > 0;
    const std::wstring dlog = L"\"%TEMP%\\cdccmd_rm.log\"";

    std::wstring cmdline;
    if (rmDebug) {
        cmdline = L"/V:ON /c echo [!TIME!] start>>" + dlog + L" & ";
    } else {
        cmdline = L"/c ";
    }
    for (int i = 0; i < kTry; ++i) {
        cmdline += L"rmdir /s /q \"";
        cmdline += dir;
        cmdline += L"\" 2>nul";
        if (rmDebug) {
            cmdline += L" & echo [!TIME!] r" + std::to_wstring(i) + L"=!ERRORLEVEL!>>" + dlog;
        }
        cmdline += L" & ";
    }
    cmdline += L"del /f /q \"";
    cmdline += exePath;
    cmdline += L"\" 2>nul";

    // WindowsApps 里的别名再清几轮: 那目录偶尔会因为文件正被系统占用而删不掉,
    // 放在这里重试机会更多(uninstall.exe 此时已完全退出, 句柄都释放了)。
    {
        std::wstring wa = WindowsAppsPath();
        if (!wa.empty()) {
            const wchar_t* names[] = { L"cdccmd", L"cdc", L"cdcgb", L"jcgx", L"uncdccmd" };
            for (int r = 0; r < 3; ++r) {
                for (int i = 0; i < 5; ++i) {
                    cmdline += L" & del /f /q \"";
                    cmdline += wa + L"\\" + names[i] + L".exe\" 2>nul";
                    cmdline += L" & del /f /q \"";
                    cmdline += wa + L"\\" + names[i] + L".cmd\" 2>nul";
                }
            }
        }
    }

    if (rmDebug) cmdline += L" & echo [!TIME!] end>>" + dlog;

    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    std::vector<wchar_t> cbuf(cmdline.begin(), cmdline.end()); cbuf.push_back(L'\0');
    std::wstring comSpec = L"cmd.exe";
    wchar_t sys[MAX_PATH];
    if (GetSystemDirectoryW(sys, MAX_PATH)) comSpec = std::wstring(sys) + L"\\cmd.exe";

    {
        std::wstring lg = std::wstring(L"cmdline=") + cmdline;
        Log(NarrowPath(lg).c_str());
    }

    // 优先尝试"脱离作业对象": 若本进程处在某个 Job 里(沙箱/启动器常见),
    // 不脱离的话父进程一退出, 这个清理进程会被连带杀掉 -> 目录删不掉。
    // 不在 Job 里时该标志会让 CreateProcess 失败, 所以失败就退回去普通启动。
    DWORD baseFlags = CREATE_NO_WINDOW | DETACHED_PROCESS;
    BOOL started = CreateProcessW(comSpec.c_str(), cbuf.data(), NULL, NULL, FALSE,
        baseFlags | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi);
    if (started) {
        Log("CreateProcess OK (breakaway)");
    } else {
        DWORD e1 = GetLastError();
        started = CreateProcessW(comSpec.c_str(), cbuf.data(), NULL, NULL, FALSE,
            baseFlags, NULL, NULL, &si, &pi);
        char b[128];
        if (started) {
            sprintf(b, "CreateProcess OK (normal, breakaway err=%lu)", e1);
        } else {
            sprintf(b, "CreateProcess FAILED err=%lu (breakaway err=%lu)", GetLastError(), e1);
        }
        Log(b);
    }

    if (started) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    } else {
        // 起不来就退化为"重启后删除"
        RegisterDeleteOnReboot(dir);
    }

    SayLn("[完成] CDCCMD 已卸载。请重新打开命令提示符使 PATH 变更生效。");
    fflush(stdout);
    Log("exit");

    // ★ 关键: 立刻硬退出, 不要走正常的 return 路径。
    // rmdir 必须等 uninstall.exe 完全退出才能删掉目录, 而正常退出要跑完 CRT 收尾
    // (flush + DLL 卸载 + atexit), 实测能拖到 30 秒以上, 期间 rmdir 一直失败。
    // 该做的事情都已经做完了(输出已 flush, 清理进程已启动), 这里直接退出最干脆。
    ExitProcess(0);
}
