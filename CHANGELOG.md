# CDCCMD 修改清单 (running changelog)

> 每次改动都往这里追加，方便回滚与复查。最新在最上面。

> **关于本仓库**：这是 CDCCMD 的**开源模版版**。所有个人 / 仓库 / 官网信息
> 已经抽成 `src/version.h` 里的配置项（`CFG_OWNER`、`CFG_REPO`、`CFG_MAIN_REPO`、
> `CFG_HOMEPAGE`、`CFG_PUBLISHER`），改那 5 个值就能变成你自己的工具。
> 下面的变更记录保持原样，只把具体标识换成了占位符 ——
> 里面记录的那些坑都是真踩过的，照抄能省不少时间。

## v1.1.0 (2026-10-02) — 下载自动加速 + 命令全机可用

### 新增：加速节点引擎
- **统一节点表**（11 个节点）替代原来散落各处的硬编码镜像
  | 节点 | 代理 API | 下载文件 | git clone |
  | --- | --- | --- | --- |
  | GitHub 官方 | ✔ | ✔ | ✔ |
  | gh-proxy.com | ✔ | ✔ | ✔ |
  | **gh.dpik.top**（akams.cn） | ✘ | ✔ | ✔ |
  | **ghfile.geekertao.top**（akams.cn） | ✘ | ✔ | ✔ |
  | github.tbap.top（akams.cn） | ✘ | ✔ | ✘ |
  | **gh.zwy.one** | ✘ | ✔ | ✔ |
  | gitclone.com | ✘ | ✘ | ✔ |
  | ghproxy.net / kkgithub.com | ✘ | ✔ | ✘ |
  | ghfast.top / ghproxy.cc | ✘ | ✔ | ✘ |
  未接通的节点会在测速阶段被自动跳过，无需手工维护。
- **并发测速**（Win32 线程，不依赖 std::thread）：串行测 10 个节点要几十秒，
  并发后总耗时 ≈ 最慢的那个。
- **吞吐测算法**：扣除首字节等待（TTFB）后按纯传输时间计算。
  否则 96KB 样本里 90% 都是 TTFB，会把速度算成 16 KB/s（实测踩过）。
- **样本完整性判定**：`ghproxy.net` 会提前截断响应（只回几十 KB 就 EOF），
  据此算出的吞吐会虚高到 7 万 KB/s。现在拿不满样本就标记"响应不完整"并排到后面。
- **失败自动切换 + 断点续传**：某节点中途失败会保留已下数据，换下一个节点
  用 `Range` 从断点续传。实测切换后速度提升 6 倍（80 KB/s → 500 KB/s）。
- **错误页识别**：代理返回"HTTP 200 + 一段 HTML"时 `Content-Length` 是与自身自洽的，
  只看长度会把 38KB 错误页当成下载成功。现在用真实文件大小兜底校验，
  大小不符就删掉重来（避免垃圾数据污染后续节点的续传）。
- **`CDC_NO_SPEED=1`**：跳过测速，按顺序尝试节点。

### 新增：`cdc releases [数量]`
列出仓库所有发布版本（对标 github.akams.cn 的"获取 Releases 列表"）：
```
  [1] 7.1.0   正式版   2026-09-25
       安装包: CodeDateCreation7.1.0_Setup.exe  (123.47 MB)
  [2] 7.0.0   正式版   2026-07-11
       安装包: CDC.exceed.7.0.0.exe  (48.38 MB)
```

### 增强：`jcgx` 详细版本检查
- 改用 `/releases` 列表接口（原 `/releases/latest`），信息更全
- 输出：当前版本 / 最新版本 / 版本名称 / 发布时间 / 发布类型 / 历史版本数 /
  发布文件清单（含大小）/ 更新说明（保留原文换行）
- 自动跳过 draft 与预发布版本
- 元数据获取支持多通道回退 + **内容校验**

### 新增：命令"整台电脑到处都能用"
Windows 只按**可执行文件名**解析 PATH，不认 exe 的子命令。除了原有的
argv[0] 分派 + 硬链接别名外，现在把别名**同时投放两个位置**：
1. 安装目录（主）
2. `%LOCALAPPDATA%\Microsoft\WindowsApps` —— Windows 10/11 默认就在 PATH 里的目录

放这里意味着**完全不依赖注册表 PATH**：即使 PATH 被改坏、或用户从
Win+R / PowerShell / Git Bash 启动，都能直接敲。

别名从 4 个增加到 **5 个**（补上主名 `cdccmd`，否则只靠 WindowsApps 时
敲 `cdccmd` 会找不到）。卸载时同步清理两个位置。

### 修复（本轮踩的坑，全部实测定位）
1. **`ShellExecuteW` 启动卸载程序不可靠**
   - 现象：`uncdccmd` 打印"正在启动卸载程序"但**什么都没发生**（目录/PATH/注册表全在）
   - 根因：`ShellExecuteW` 是异步的，在受限环境（作业对象/沙箱）里会"看着启动了、其实没起来"
   - 修法：改用 `CreateProcessW` + `WaitForSingleObject` **同步等待**，卸载输出直接显示在当前终端

2. **`ping` 在无控制台环境下退化成 30 秒等待**（本轮最大的坑）
   - 现象：卸载后目录要 30+ 秒才消失
   - 时间戳日志实锤：`start 21:33:49.74 → 第一次 rmdir 21:34:22.17`，**32.4 秒全被一条 ping 吃掉**
   - 根因：卸载进程用 `DETACHED_PROCESS` 启动（无控制台）。同一台机器上
     `ping -n 2 127.0.0.1` 有控制台时 1.4 秒，无控制台时 **32.4 秒**；
     `timeout /t 2` 直接挂死；`for /L do @cd.` 45 秒+
   - 修法：**不用 sleep**。`uninstall.exe` 已 `ExitProcess` 秒退，
     而 cmd 从启动到执行第一条 rmdir 也有几十毫秒天然延迟，足够释放文件锁；
     再连续打 40 发 rmdir 覆盖抖动窗口。**实测删除耗时 32.4 秒 → 0.78 秒**

3. **`rem` 会吃掉本行余下所有内容**（包括 `)` 和 `&`）
   - 现象：`for /L ... do @rem & rmdir ...` 里 rmdir 完全不执行
   - 根因：`rem` 在括号内也会吃掉 `)`，导致括号不闭合、后续全成注释
   - 结论：复合命令串里绝不能用 `rem`

4. **`cmd /c "整串"` 外层引号包裹会把命令切坏**
   - 命令串里路径自带引号，再套一层会被 cmd 的引号解析规则（见 `cmd /?`）切散
   - 修法：直接把命令串交给 cmd，不加外层引号

5. **`ExitProcess` 收尾**：卸载做完该做的事后直接硬退出，不走正常 return
   （正常退出要跑完 CRT 收尾，会拖住等待中的 rmdir）

6. **`CreateProcessW` 加 `CREATE_BREAKAWAY_FROM_JOB` 尝试**
   - 进程若在作业对象里，不脱离的话父进程一退出子进程会被连带杀掉
   - 不在 Job 里时该标志会失败（ERROR_ACCESS_DENIED=5），自动回退普通启动

### 其它
- `uninstall.exe` 内置排障日志（`CDCCMD_RM_DEBUG=1` 开启），用 Win32 API 写
  （用 `std::ofstream` 会把整个 iostream 静态库拉进来，exe 从 0.6MB 胀到 3MB）
- 新增 `iss\iss_check.py`：Inno 脚本静态自检，专抓"注释块内嵌花括号"这个反复踩的坑
- `uninstall.exe` 体积 0.61 MB → 1.11 MB（新增日志/WindowsApps 清理逻辑）
- 版本号 1.0.1 → **1.1.0**

### 测试结果
| 测试 | 结果 |
| --- | --- |
| `e2e_test.py`（自包含包全链路） | **43 / 43 通过** |
| `iss_e2e.py`（ISS 包全链路） | **41 / 41 通过** |
| 卸载稳定性（3 轮连跑） | 目录 1~3 秒删除，无残留 |
| 下载加速实测 | 选中最快节点，22 秒 5.23 MB（≈240 KB/s，直连仅 14 KB/s） |

### 实测数据（本机 2026-10-02）
各节点下载同一个 123 MB 安装包的真实速度：
```
gh.dpik.top            661 KB/s   ← akams.cn 节点
ghfile.geekertao.top   524 KB/s   ← akams.cn 节点
gh-proxy.com            58 KB/s
GitHub 官方             14 KB/s   ← 直连
```
**akams 节点比直连快约 47 倍。**

---

## v1.0.1 (2026-10-01) — 修复"命令敲不出来"

> 用户真机反馈：安装后敲 `cdc download` / `jcgx` 都报
> `'xxx' 不是内部或外部命令，也不是可运行的程序或批处理文件。`
> 排查出**两个独立根因**，都已修。

### 根因 A：没重开 CMD（用户侧）
旧 CMD 窗口的 PATH 是**启动那一刻的快照**，安装器改的是注册表，不会穿透进已开的窗口。

**修法**：
- ISS 包新增任务「安装完成后打开一个新的命令提示符」，**默认勾选**（`[Run]` + `Tasks: opennewcmd`）
- 安装完成提示里加粗写清"请关闭本窗口后重新打开命令提示符"

### 根因 B：`jcgx` 是子命令，不是可执行文件名（程序侧，更关键）
Windows 的 `CreateProcess` / `PATHEXT` 只按**可执行文件名**解析 PATH，
`jcgx` 只是 `cdccmd.exe` 的一个子命令参数，光把目录塞进 PATH 永远调不到它。
（`cdccmd` 能通纯属巧合——它恰好等于 exe 的文件名主干。）

**修法：argv[0] 身份分派 + 硬链接别名**
- `cdccmd.cpp` 入口新增 `NameFromArgv0()`：取 `argv[0]` 主干（去路径、去 `.exe`、转小写），
  命中 `cdc` / `cdcgb` / `jcgx` / `uncdccmd` 就直接分派到对应子命令
  （`cdcgb` 的仓库 URL 参数位相应前移一位）
- 安装时用 `CreateHardLinkW` 给主程序做 4 个同名副本：
  `cdc.exe` / `cdcgb.exe` / `jcgx.exe` / `uncdccmd.exe`
  —— 硬链接与源文件共享同一份磁盘数据，**零额外空间占用**
- 硬链接失败（跨卷、FAT32 等）退化为 `CopyFileW`；再失败写 `.cmd` 转发脚本兜底
- 两处安装器都改了：
  - `src\installer.cpp` 新增 `MakeAliases()`（自包含包）
  - `iss\cdc_setup.iss` 新增 `MakeAliases()` / `MakeAliasScripts()`（ISS 包）
  - 新增 `src\shims.py` 供构建期复用同一份别名清单

### 根因 C：卸载后 TEMP 残留脚本（顺带修掉）
- 现象：卸载完 `%TEMP%\cdccmd_uninstall.bat` / `cdccmd_unin_self.bat` 不消失
- 根因 1：旧脚本用 `tasklist` 轮询"uninstall.exe 是否还在"，
  而脚本正是被 uninstall.exe 启动的 → **永远死等**，走不到 rmdir 和自删
- 根因 2：Windows 下正在执行的 `.bat` **无法被 `del`**；
  就算先 `move` 改名，读取句柄仍锁着同一文件对象，仍会残留 `xxx.bat.del`
- **修法**：不再写任何脚本文件。改用 `CreateProcessW` 起一条**内联 `cmd /c` 命令串**：
  `ping -n 3 >nul & rmdir /s /q "<dir>" & del /f /q "<exe>"`
  内联命令没有磁盘文件，天然零残留。
- 附带收益：`uninstall.exe` 从 2.53 MB 瘦身到 **614 KB**（去掉了没用的代码）

### 新增
- **`iss\cdc_setup.iss`**：Inno Setup 7 打包脚本
  - 免管理员（`PrivilegesRequired=lowest` + 装到 `{localappdata}`）
  - `[Registry]` 写用户 PATH + `Check: NeedsAddPath('{app}')` 防重复
  - `[Code] RemoveFromUserPath()` 卸载时精确摘 PATH
  - 产物 `build\cdc-setup-iss.exe`，约 2.6 MB（自包含包约 6.8 MB）
  - **Inno Pascal Script 无内建 `CreateHardLink`**，必须 `external` 声明
    `CreateHardLinkW@kernel32.dll stdcall`，否则 `Unknown identifier`
- **`iss\make_icon.py`**：把 64x64 单尺寸图标转成 16/24/32/48/64/128 六档多尺寸 `.ico`
  - 手写 ICO 容器 + PNG 载荷（Vista+ 全支持）
  - 双线性重采样**预乘 alpha**，避免半透明边缘发黑
- **`src\shims.py`**：别名清单与硬链接创建逻辑（供 build.py 调用）
- **`iss_e2e.py`**：ISS 包 21 项全链路验收
- **`iss\real_check.py`**：真机 PATH 环境敲命令验证

### 测试结果
| 测试 | 结果 |
| --- | --- |
| `e2e_test.py`（自包含包） | **27 / 27 通过** |
| `iss_e2e.py`（ISS 包） | **21 / 21 通过** |
| `iss\real_check.py`（真机 PATH） | **4 条命令全部识别** |

### 踩坑记录（本版新增）
1. **Inno Pascal Script 没有 `CreateHardLink`**
   → 报 `Error on line 133 ... Unknown identifier 'CreateHardLink'`
   → 必须显式 `external 'CreateHardLinkW@kernel32.dll stdcall'`
2. **`cmd` 的内联命令串里，内层引号必须是裸双引号**
   - 写成 `\\\"` （即传入 `\"`）会让 cmd 的 `/c "..."` 提前闭合，静默失败、目录删不掉
   - 反斜杠转义只属于 C 字面量，不是 Win32 的规则
3. **`cmd` 无法删除正在执行的 `.bat`**，`move` 改名也救不了（句柄仍锁同一对象）
   → 最终放弃脚本方案，改内联 `cmd /c`
4. **`for /L %i in (...) do (...)` 的括号体里不能用 `&` 串命令**
   → `&` 是命令分隔符，会被提前切分，循环体被拆散
5. **测试断言要区分"延迟完成"与"真残留"**
   → 自删是延迟动作，断言应加轮询等待，而不是瞬时判定

---

## v1.0.0 (2026-10-01) — 首个版本

### 新增
- **`cdcgb <url> [目录]`**：克隆 GitHub 仓库，自动测速选最快节点
  - 7 个候选镜像：GitHub 官方 / gh-proxy.com / ghfast.top / ghproxy.net / ghproxy.cc / gitclone.com / kkgithub.com
  - 用 `info/refs` + 4 秒超时做轻量延迟探测，`GetTickCount64()` 计时，取最快
  - 最快节点失败自动按延迟顺序回退；全部探测失败则退回逐个盲试
  - 非 GitHub 地址不上镜像，原样克隆
  - 未装 Git 时给出友好提示 + 下载地址
- **`cdc download`**：从 GitHub releases 取 主项目 最新 exe 安装包
  - 优先挑 `setup`/`install` 命名的 exe
  - 下载到「下载」文件夹，带进度条（26 格 + 百分比 + MB）
  - 进度节流：最多每 200ms 刷一次，避免刷屏
  - 完成后 `ShellExecuteW` 自动打开
  - 按 Content-Length 做完整性校验
- **`<有效网址>`**：默认浏览器打开网址
  - 认 `http://` / `https://` / `ftp://` / 裸域名（自动补 `https://`）
- **`jcgx`**：自更新
  - 从 `your-github-name/CDCCMD` releases 取最新 tag，与内置版本号比较
  - 有新版 → `(Y/N)` 询问，Y 则下载并覆盖
  - 优先取 `cdccmd-setup.exe` 附件（直接跑覆盖安装），否则延迟替换自身
  - 404（未建发布页）给出友好提示而非报错
- **`uncdccmd`**：打开卸载程序；找不到本地 uninstall.exe 时回退查注册表
- **自包含安装包**：内嵌 cdccmd.exe(资源101) + uninstall.exe(资源102)
  - 释放到 `%LOCALAPPDATA%\Programs\cdccmd`，**无需管理员**
  - 写用户 PATH（去重、保留原有路径、广播 WM_SETTINGCHANGE）
  - 写「添加/删除程序」项（DisplayName/Publisher/DisplayIcon/URLInfoAbout/QuietUninstallString）
- **卸载程序**：按分号精确移除 PATH 段 → 删注册表 → 清目录 → 延迟批处理删自身
- **`build.py`**：沙箱安全的构建脚本；`python build.py clean` 清理
- **`e2e_test.py`**：27 项全链路验收（安装/权限/注册表/命令冒烟/卸载/残留检查）
- **README.md**：安装、命令、编译、发布流程

### 踩坑与修复记录

1. **`ld.exe` 启动失败（RC=127 / 0xC0000135 STATUS_DLL_NOT_FOUND）**
   - 现象：`g++` 只在链接阶段失败，报 `collect2.exe: error: ld returned 53 exit status`，
     **且没有任何错误正文**；`-fsyntax-only`、`-S`、手动 `as` 全部正常。
   - 根因：`x86_64-w64-mingw32\bin\ld.exe` 依赖 `mingw64\bin\libwinpthread-1.dll`，
     子进程 PATH 没带上 `mingw64\bin` 时加载器直接启动失败，错误码被 collect2 吞掉。
   - 修复：`build.py` 里显式构造 `env['PATH'] = mingw64\bin + os.pathsep + 原PATH`。
     **bash 里 `export PATH=...` 对子进程不生效**，必须用 Python 传 env。

2. **`FOLDERID_Downloads` / `FOLDERID_LocalAppData` undefined reference**
   - 根因：`FOLDERID_*` 是 GUID 符号，不是宏。
   - 修复：链接加 `-luuid`（三个 exe 都要）。

3. **`FindResourceW(..., RT_RCDATA)` 编译错**
   - 根因：`RT_RCDATA` 是窄字符宏 `"RCDATA"`，宽字符版 API 要 `LPCWSTR`。
   - 修复：改用 `MAKEINTRESOURCEW(10)`（10 = RT_RCDATA 的数值）。

4. **`wprintf(L"%s", wchar_t*)` 只输出首字符**
   - 现象：安装成功的"安装位置: C:\Users\..."只打印出 `C`，**零警告静默失败**。
   - 根因：这是 mingw 下的经典坑——`-mno-wildcard`/UCRT 配置下 `%s` 被当成窄字符串。
   - 修复：**全面弃用 `wprintf`**，改成 `NarrowPath()` 转 ANSI 再 `Say()`。
     （项目红线 #4：mingw 宽字符格式化必须 `%ls`，更稳的是手写转换。）

5. **卸载后 TEMP 残留 `.bat`**
   - 现象：`cdccmd_uninstall.bat` 删不掉，脚本自删失败。
   - 根因：cmd **无法删除正在执行中的 .bat**；且 `start /b cmd /c "...\"path\"..."` 的
     嵌套转义引号会被 cmd 吃掉，命令整体失效。
   - 修复：改成再生成一个 `*_self.bat`，用 `CreateProcessA(CREATE_NO_WINDOW|DETACHED_PROCESS)`
     分离启动它，延迟 3 秒删原脚本再删自己。**不要用嵌套引号拼 cmd 命令。**

6. **裸域名打不开**
   - 现象：`cdccmd example.com/path` 打开失败（`www.` 有补协议，其他裸域名没补）。
   - 修复：`OpenUrl()` 对 `http/https/ftp` 之外的输入一律前置 `https://`。

7. **下载目录可能不存在**
   - 现象：`SHGetKnownFolderPath(FOLDERID_Downloads)` 成功返回但该目录在磁盘上不存在
     （被重定向/精简过的系统），导致写文件失败，且没有任何提示。
   - 修复：`DownloadsDir()` 改为"API 返回值实测 `DirExists` 才算数"，
     否则回退 `%USERPROFILE%\Downloads`，不存在就 `CreateDirectoryW` 建一个。
   - 另外把下载目标**完整路径**打印出来（原来只说"-> 下载文件夹"），方便排查。

8. **进度条刷屏**（早期版本）
   - 现象：重定向到文件时每个 chunk 一行，123MB 下载产生几百行。
   - 修复：按 `GetTickCount64()` 节流，最快每 200ms 刷新一次。

### 编译命令（手工复现用）

```
cdccmd.exe:
  g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++ -Wall
      -o build\cdccmd.exe src\cdccmd.cpp
      -lwinhttp -lshell32 -lole32 -ladvapi32 -luuid

uninstall.exe:
  g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++
      -o build\uninstall.exe src\uninstall.cpp
      -lshell32 -luser32 -ladvapi32 -lole32 -luuid

cdccmd-setup.exe（需先在 build\payload\ 里放好两个 exe + payload.rc）:
  windres payload.rc -O coff -o payload.o          # cwd = build\payload
  g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++
      -I src -o build\cdccmd-setup.exe src\installer.cpp build\payload\payload.o
      -lshell32 -luser32 -ladvapi32 -lole32 -luuid
```

### 验收结果

`python e2e_test.py` → **27 项通过 / 0 项失败**
覆盖：安装释放、PATH 写入、注册表四项、help/无参/未知命令/参数校验、
jcgx 优雅降级、uncdccmd 卸载、PATH 还原、注册表清理、TEMP 无残留。

实测通过的功能：
- `cdcgb` 完整跑通（7 节点测速 → 自动回退 → 克隆成功，`octocat/Hello-World` + `your-github-name/your-repo`）
- `cdc download` 正确识别 7.1.0 / `CodeDateCreation7.1.0_Setup.exe` / 122.99 MB，
  正确创建目标文件并渲染节流后的进度条
  （**注**：本机到 GitHub Release CDN 的出口带宽极慢，123MB 全量下载未能跑完；
  下载引擎另用 `tools/dl_probe.cpp` 单独验证通过 —— 小文件 521 字节一次读完、
  无 Content-Length 的 chunked 响应也能正确处理）
- `jcgx` 正确识别"尚未创建发布页"
- URL 打开三种形态均通过
