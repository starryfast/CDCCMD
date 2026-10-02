# -*- coding: utf-8 -*-
"""e2e_test.py -- CDCCMD 全链路验收测试
安装 -> PATH/注册表检查 -> 命令冒烟 -> 卸载 -> 清理校验
"""
import os
import subprocess
import sys
import time
import winreg

BUILD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build")
SETUP = os.path.join(BUILD, "cdccmd-setup.exe")
INSTALL_DIR = os.path.expandvars(r"%LOCALAPPDATA%\Programs\cdccmd")
EXE = os.path.join(INSTALL_DIR, "cdccmd.exe")
UNINS_REG = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\cdccmd"
# 命令别名同时投放到这两个目录 (WindowsApps 天然在 PATH 里 -> 保证全机可用)
WINDOWSAPPS = os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WindowsApps")
ALIASES = ["cdccmd", "cdc", "cdcgb", "jcgx", "uncdccmd"]

ok_count = 0
fail_count = 0


def check(name, cond, extra=""):
    global ok_count, fail_count
    if cond:
        ok_count += 1
        print("  [PASS] %s %s" % (name, extra))
    else:
        fail_count += 1
        print("  [FAIL] %s %s" % (name, extra))


def run(args, timeout=60, cwd=None, stdin=None, env=None):
    return subprocess.run(args, capture_output=True, timeout=timeout,
                          cwd=cwd, input=stdin, env=env)


def dec(b):
    return (b or b"").decode("gbk", "replace")


def reg_path_value():
    try:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Environment")
        return winreg.QueryValueEx(k, "Path")[0]
    except Exception:
        return ""


def reg_uninstall_exists():
    try:
        winreg.OpenKey(winreg.HKEY_CURRENT_USER, UNINS_REG)
        return True
    except FileNotFoundError:
        return False


print("=" * 64)
print("  CDCCMD 全链路验收测试")
print("=" * 64)

# ---------- 0. 前置清理 ----------
print("\n[0] 前置清理 ...")
subprocess.run(["cmd", "/c", "rmdir", "/s", "/q", INSTALL_DIR], capture_output=True)
for f in os.listdir(os.environ.get("TEMP", ".")):
    if "cdccmd" in f.lower():
        try:
            os.remove(os.path.join(os.environ["TEMP"], f))
        except Exception:
            pass
check("测试前无残留安装目录", not os.path.isdir(INSTALL_DIR))

# ---------- 1. 安装 ----------
print("\n[1] 运行安装包 ...")
p = run([SETUP], timeout=120, stdin=b"\r\n")
out = dec(p.stdout)
check("安装程序退出码 0", p.returncode == 0, "(rc=%d)" % p.returncode)
check("输出含'安装成功'", "安装成功" in out)
check("完整安装路径已打印", INSTALL_DIR in out, "(未截断)")
check("cdccmd.exe 已释放", os.path.isfile(EXE))
check("uninstall.exe 已释放", os.path.isfile(os.path.join(INSTALL_DIR, "uninstall.exe")))

# ---------- 2. PATH / 注册表 ----------
print("\n[2] 检查 PATH 与注册表 ...")
pathv = reg_path_value()
check("安装目录已写入用户 PATH", r"Programs\cdccmd" in pathv)
check("卸载注册表项已创建", reg_uninstall_exists())
if reg_uninstall_exists():
    k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, UNINS_REG)
    dn = winreg.QueryValueEx(k, "DisplayName")[0]
    us = winreg.QueryValueEx(k, "UninstallString")[0]
    check("DisplayName 正确", "CDCCMD" in dn, "-> %s" % dn)
    check("UninstallString 指向 uninstall.exe", us.endswith("uninstall.exe"), "-> %s" % us)

# ---------- 2.5 全机可用 ----------
print("\n[2.5] 命令别名与全机可用 ...")
for a in ALIASES:
    check("%s.exe 在安装目录" % a, os.path.isfile(os.path.join(INSTALL_DIR, a + ".exe")))
for a in ALIASES:
    check("%s.exe 已投放 WindowsApps" % a,
          os.path.isfile(os.path.join(WINDOWSAPPS, a + ".exe")))

# 关键: 构造一个"只有系统目录 + WindowsApps"的干净 PATH (不含安装目录),
# 证明命令不依赖注册表 PATH 也能整机调用
sysroot = os.environ.get("SystemRoot", r"C:\Windows")
clean_env = dict(os.environ)
clean_env["PATH"] = ";".join([
    sysroot,
    os.path.join(sysroot, "System32"),
    os.path.join(sysroot, "System32", "Wbem"),
    WINDOWSAPPS,
])
for cmd, kw in [("cdc", "cdc download"), ("cdcgb", "cdcgb"),
                ("jcgx", "检查"), ("cdccmd help", "cdcgb")]:
    try:
        # stdin 给 DEVNULL: jcgx 若问到 Y/N 会立刻收到 EOF 而取消, 不会挂住
        r = subprocess.run(["cmd", "/c", cmd], capture_output=True,
                           env=clean_env, timeout=90, stdin=subprocess.DEVNULL)
        o = dec(r.stdout) + dec(r.stderr)
        check("干净 PATH 下可调用 '%s'" % cmd,
              "不是内部或外部命令" not in o and kw in o)
    except subprocess.TimeoutExpired:
        check("干净 PATH 下可调用 '%s'" % cmd, False, "(超时)")

# ---------- 3. 命令冒烟 ----------
print("\n[3] 命令冒烟测试 ...")

p = run([EXE, "help"])
h = dec(p.stdout)
check("help 退出码 0", p.returncode == 0)
for kw in ["cdcgb", "cdc download", "cdc releases", "jcgx", "uncdccmd", "帮助"]:
    check("help 含 '%s'" % kw, kw in h)

p = run([EXE])
check("无参数 -> 显示帮助", "用法" in dec(p.stdout))

p = run([EXE, "不存在的命令"])
check("未知命令 -> 提示错误", "未知命令" in dec(p.stdout) and p.returncode != 0)

p = run([EXE, "cdcgb"])
check("cdcgb 缺参数 -> 给用法", "用法" in dec(p.stdout))

p = run([EXE, "cdc"])
check("cdc 缺子命令 -> 给用法", "cdc download" in dec(p.stdout))

# jcgx: 仓库未建发布页时应优雅提示而非崩溃
p = run([EXE, "jcgx"], timeout=90)
j = dec(p.stdout)
check("jcgx 不崩溃", p.returncode == 0 or "错误" in j or "提示" in j,
      "-> %s" % j.strip().splitlines()[0] if j.strip() else "")

# ---------- 4. 卸载 ----------
print("\n[4] 运行 uncdccmd 卸载 ...")
p = run([EXE, "uncdccmd"], timeout=60)
check("uncdccmd 退出码 0", p.returncode == 0)
check("提示已启动卸载程序", "卸载" in dec(p.stdout))

# 等批处理完成
for _ in range(30):
    time.sleep(1)
    if not os.path.isdir(INSTALL_DIR):
        break

check("安装目录已删除", not os.path.isdir(INSTALL_DIR))
pathv2 = reg_path_value()
check("PATH 已移除安装目录", r"Programs\cdccmd" not in pathv2)
check("卸载注册表项已删除", not reg_uninstall_exists())
left_wa = [a for a in ALIASES
           if os.path.isfile(os.path.join(WINDOWSAPPS, a + ".exe"))]
# WindowsApps 的清理由"卸载后的后台 cmd"补刀, 是异步的 -> 给点时间
if left_wa:
    for _ in range(20):
        time.sleep(1)
        left_wa = [a for a in ALIASES
                   if os.path.isfile(os.path.join(WINDOWSAPPS, a + ".exe"))]
        if not left_wa:
            break
check("WindowsApps 里的别名已清理", not left_wa, "残留: %s" % left_wa)

leftover = [f for f in os.listdir(os.environ.get("TEMP", "."))
            if "cdccmd" in f.lower() and not f.lower().endswith(".log")]
# 自删清理是"延迟执行"的: 先等卸载进程退出, 再重试 rmdir, 最后才清自己。
# 给它最多 30 秒, 断言的是"最终不留残留", 而不是"瞬间消失"。
if leftover:
    for _ in range(30):
        time.sleep(1)
        leftover = [f for f in os.listdir(os.environ.get("TEMP", "."))
                    if "cdccmd" in f.lower() and not f.lower().endswith(".log")]
        if not leftover:
            break
check("TEMP 无残留脚本(延迟自删)", len(leftover) == 0, "残留: %s" % leftover)

print("\n" + "=" * 64)
print("  结果: %d 项通过, %d 项失败" % (ok_count, fail_count))
print("=" * 64)
sys.exit(1 if fail_count else 0)
