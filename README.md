# CDCCMD — Windows 命令行工具模版

一个可以直接 fork 改造的 **Win32 命令行工具模版**：C++17 + WinHTTP 手写，
**零第三方依赖**、静态链接单文件运行，自带：

- 🚀 **多节点自动加速下载引擎**（并发测速 + 失败自动切换 + 断点续传）
- 📦 **免管理员安装包**（写用户 PATH，命令全机可用）
- 🔄 **自更新**（从 GitHub Releases 拉新版）
- 🧪 **开箱即用的验收测试**（安装 / 命令 / 卸载全链路）

**改 5 个字符串就能变成你自己的工具。**

---

## 一、快速开始

### 1. fork / clone 本仓库

### 2. 改配置（只有一个文件）

打开 `src/version.h`，改「配置区」这几个值：

| 宏 | 含义 | 示例 |
| --- | --- | --- |
| `CFG_OWNER` | 你的 GitHub 用户名或组织 | `"octocat"` |
| `CFG_REPO` | **本工具**的仓库名（自更新用） | `"CDCCMD"` |
| `CFG_MAIN_REPO` | 「下载主项目」的目标仓库名 | `"my-awesome-tool"` |
| `CFG_HOMEPAGE` | 项目主页 | `"https://github.com/octocat"` |
| `CFG_PUBLISHER` | 发布者署名 | `"Octo Cat"` |
| `CDCCMD_VERSION` | 版本号 | `"1.0.0"` |

改版本号时记得同步两处（`.rc` / `.iss` 不是 C++，用不了宏）：

- `src/payload.rc` 的 `FILEVERSION` / `PRODUCTVERSION` / `"FileVersion"`
- `iss/cdc_setup.iss` 的 `#define AppVer`

### 3. 编译

```cmd
python build.py
```

依赖：PATH 里有一个 **mingw64 工具链**（没装的话去 <https://www.mingw-w64.org/> 下载，
或改 `build.py` 顶部的 `MINGW_CANDIDATES` 指到你自己的路径）。

产物：

| 文件 | 说明 |
| --- | --- |
| `build\cdccmd.exe` | 主程序 |
| `build\uninstall.exe` | 卸载程序 |
| `build\cdccmd-setup.exe` | **自包含安装包**（内嵌上面两个，双击即装） |

### 4. 打安装包（可选，界面更正式）

需要 [Inno Setup 7](https://jrsoftware.org/isdl.php)：

```cmd
cd iss
"%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe" cdc_setup.iss
```

产物 `build\cdc-setup-iss.exe`：装完自动开一个新的 CMD 窗口。

> ⚠️ **记得改 `iss\cdc_setup.iss` 里的 `AppId` GUID**，别和别人的安装包撞车。

---

## 二、命令一览

装好后这 5 条命令在全机任意终端都能直接用：

| 命令 | 作用 |
| --- | --- |
| `cdcgb <仓库URL> [目录]` | 克隆 GitHub 仓库，**自动测速挑最快节点** |
| `cdc download` | 下载并打开主项目最新安装包（**自动加速**） |
| `cdc releases [数量]` | 列出主项目的所有发布版本 |
| `jcgx` | 检查本工具自身更新，问 Y/N |
| `uncdccmd` | 卸载 |
| `<有效网址>` | 用默认浏览器打开网址 |

```cmd
cdccmd help                                  # 看完整帮助
cdcgb https://github.com/octocat/Hello-World # 克隆，自动挑最快镜像
cdc download                                 # 下主项目最新版
cdc releases 20                              # 列最近 20 个版本
jcgx                                         # 自更新
```

---

## 三、加速下载引擎（本模版的核心）

GitHub 在国内直连经常只有十几 KB/s。本模版内置一个**节点表 + 并发测速 + 自动切换**的下载引擎。

### 它是怎么工作的

1. **并发测速**：给全部节点同时发一个 96KB 的 `Range` 取样请求
2. **算真实吞吐**：扣除首字节等待时间（TTFB）后按纯传输时间算
   （不扣的话，样本里 90% 都是等待时间，速度会被算成十几 KB/s）
3. **可信度判定**：拿不满样本的节点标记「响应不完整」，排到最后
   （有些代理会提前截断响应，据此算出的吞吐会虚高到几万 KB/s）
4. **从最快的开始下**，中途失败自动换下一个，并用 `Range` **从断点续传**
5. **错误页识别**：代理返回 "HTTP 200 + 一段 HTML" 时长度是自洽的，
   只看 `Content-Length` 会把错误页当成下载成功 —— 所以用真实文件大小兜底校验

### 内置节点

节点表在 `src/cdccmd.cpp` 的 `kNODES[]`，每个节点标注了能力：

```cpp
struct Node {
    const char* name;
    const char* tpl;   // "{u}" = 完整URL, "{s}" = 去掉协议头的URL
    bool api;          // 能否可靠代理 api.github.com
    bool file;         // 能否代理 release 资产下载
    bool git;          // 能否代理 git clone
};
```

默认包含 `gh-proxy.com` / `ghproxy.net` / `kkgithub.com` / `ghfast.top` /
`ghproxy.cc` / `gitclone.com` 以及若干专用加速节点。
**接不通的会在测速阶段自动跳过**，不用手工维护 —— 加节点只要加一行。

> 环境变量 `CDC_NO_SPEED=1` 可跳过测速，按顺序尝试。

---

## 四、命令为什么"全机可用"

Windows 的 `CreateProcess` / `PATHEXT` **只按可执行文件名解析 PATH**，
不认 exe 的子命令 —— 所以光把目录塞进 PATH，`jcgx` 是调不到的。

本模版的解法：

1. **`argv[0]` 身份分派**：主程序读自己的文件名，判断「我是谁」
2. 安装时用**硬链接**做出同名副本（零额外磁盘占用）：
   `cdccmd.exe` `cdc.exe` `cdcgb.exe` `jcgx.exe` `uncdccmd.exe`
3. 别名**同时投放两个位置**：
   - 安装目录（主）
   - `%LOCALAPPDATA%\Microsoft\WindowsApps` —— Windows 10/11 默认就在 PATH 里，
     **完全不依赖注册表**，即使 PATH 被改坏也能用

硬链接不可用时退回复制，再不行写 `.cmd` 转发脚本兜底。

---

## 五、目录结构

```
├─ src/
│   ├─ cdccmd.cpp       主程序（命令分派 + 加速引擎 + 各功能）
│   ├─ installer.cpp    自包含安装包
│   ├─ uninstall.cpp    卸载程序
│   ├─ payload.rc       把两个 exe 嵌进安装包的资源脚本
│   ├─ shims.py         别名清单与硬链接创建
│   └─ version.h        ★ 唯一配置入口
├─ iss/
│   ├─ cdc_setup.iss    Inno Setup 打包脚本
│   ├─ make_icon.py     多尺寸 ico 生成
│   ├─ iss_check.py     .iss 静态自检（抓注释里的花括号等坑）
│   ├─ real_check.py    真机 PATH 环境验证
│   └─ assets/          图标
├─ build.py             构建脚本（Python，自动处理 PATH）
├─ e2e_test.py          自包含包全链路验收
├─ iss_e2e.py           Inno 包全链路验收
└─ CHANGELOG.md         修改清单
```

---

## 六、验收测试

```cmd
python e2e_test.py        # 安装 / PATH / 命令 / 卸载 全链路
python iss_e2e.py         # Inno 包同上，另验证别名投放
python iss\real_check.py  # 真机 PATH 环境敲命令
```

测试会真的安装到 `%LOCALAPPDATA%\Programs\cdccmd` 再卸载，**建议在虚拟机或干净环境跑**。

---

## 七、二次开发提示

- **改命令名**：`src/cdccmd.cpp` 的 `main()` 里按 `argv[0]` 分派那段 +
  `src/shims.py` / `src/installer.cpp` / `iss\cdc_setup.iss` 的别名清单（三处要一起改）
- **加加速节点**：`src/cdccmd.cpp` 的 `kNODES[]` 加一行即可，全命令生效
- **编译路径别含中文**：mingw 的部分工具在中文路径下会找不到文件
- **换图标**：`iss\assets\*.ico` 只是示例，用 `iss\make_icon.py` 重新生成你自己的
- **卸载是异步的**：`uninstall.exe` 退出后才由后台 cmd 删掉自己的目录，
  正常 1~3 秒内消失（细节与踩过的坑见 `CHANGELOG.md`）

---

## 许可证

MIT，见 [LICENSE](LICENSE)。随便用，改完记得把 `version.h` 里的署名换成你自己。
