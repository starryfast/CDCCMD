# -*- coding: utf-8 -*-
"""iss_e2e.py -- CDCCMD 的 Inno Setup 安装包全链路验收

覆盖: 静默安装 -> PATH/注册表 -> 命令可用性(模拟新 CMD 的 PATH) ->
      卸载 -> PATH 清理 -> 残留检查
用法: python iss_e2e.py
"""
import os
import subprocess
import sys
import time
import winreg

ROOT = os.path.dirname(os.path.abspath(__file__))
SETUP = os.path.join(ROOT, "build", "cdc-setup-iss.exe")
INSTALL_DIR = os.path.expandvars(r"%LOCALAPPDATA%\Programs\cdccmd")
EXE = os.path.join(INSTALL_DIR, "cdccmd.exe")
UNINS = os.path.join(INSTALL_DIR, "unins000.exe")
WINDOWSAPPS = os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WindowsApps")
ALIASES = ["cdccmd", "cdc", "cdcgb", "jcgx", "uncdccmd"]
UNINS_REG = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\{8F3A1C42-7B6E-4D91-9C25-3E7A5B1D8F04}_is1"

ok = fail = 0


def check(name, cond, extra=""):
    global ok, fail
    if cond:
        ok += 1
        print("  [PASS] %s %s" % (name, extra))
    else:
        fail += 1
        print("  [FAIL] %s %s" % (name, extra))


def dec(b):
    return (b or b"").decode("gbk", "replace")


def user_path():
    try:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Environment")
        return winreg.QueryValueEx(k, "Path")[0]
    except Exception:
        return ""


def enum_uninstall_keys():
    """列出卸载注册表里所有跟 CDCCMD 相关的子键。"""
    out = []
    try:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                           r"Software\Microsoft\Windows\CurrentVersion\Uninstall")
        i = 0
        while True:
            try:
                name = winreg.EnumKey(k, i)
                i += 1
                try:
                    sk = winreg.OpenKey(k, name)
                    try:
                        dn = winreg.QueryValueEx(sk, "DisplayName")[0]
                    except Exception:
                        dn = ""
                    if "CDCCMD" in (dn or "") or "cdccmd" in name.lower():
                        out.append((name, dn))
                except Exception:
                    pass
            except OSError:
                break
    except Exception:
        pass
    return out


print("=" * 64)
print("  CDCCMD Inno Setup 安装包 - 全链路验收")
print("=" * 64)

# ---------- 0. 前置检查 ----------
print("\n[0] 前置检查 ...")
check("安装包已生成", os.path.isfile(SETUP),
      "(%d 字节)" % os.path.getsize(SETUP) if os.path.isfile(SETUP) else "(缺失)")
check("测试前无残留安装目录", not os.path.isdir(INSTALL_DIR))

# ---------- 1. 静默安装 ----------
print("\n[1] 静默安装 (/VERYSILENT) ...")
p = subprocess.run([SETUP, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"],
                   capture_output=True, timeout=300)
check("安装程序退出码 0", p.returncode == 0, "(rc=%d)" % p.returncode)
# 安装器可能异步收尾, 给点时间
for _ in range(20):
    if os.path.isdir(INSTALL_DIR) and os.path.isfile(EXE):
        break
    time.sleep(1)

check("cdccmd.exe 已安装", os.path.isfile(EXE))
check("uninstall 程序已安装", os.path.isfile(UNINS),
      "(unins000.exe)" if os.path.isfile(UNINS) else "(缺失)")
check("文档已附带", os.path.isfile(os.path.join(INSTALL_DIR, "README.md")))

# ---------- 2. PATH ----------
print("\n[2] 用户 PATH ...")
pathv = user_path()
segs = [s for s in pathv.split(";") if s.strip()]
has = any(s.rstrip("\\").lower() == INSTALL_DIR.lower().rstrip("\\") for s in segs)
check("安装目录已在 PATH", has)
check("PATH 无重复项", sum(1 for s in segs
                          if s.rstrip("\\").lower() == INSTALL_DIR.lower().rstrip("\\")) <= 1)

# ---------- 3. 注册表 ----------
print("\n[3] 卸载注册表项 ...")
keys = enum_uninstall_keys()
check("存在卸载注册项", len(keys) >= 1, "-> %s" % keys)
if keys:
    k = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                       r"Software\Microsoft\Windows\CurrentVersion\Uninstall\\" + keys[0][0])
    try:
        us = winreg.QueryValueEx(k, "UninstallString")[0]
        check("UninstallString 正确", "unins" in us.lower(), "-> %s" % us)
    except Exception as e:
        check("UninstallString 可读", False, str(e))
    try:
        dv = winreg.QueryValueEx(k, "DisplayVersion")[0]
        check("DisplayVersion = 1.1.0", dv == "1.1.0", "-> %s" % dv)
    except Exception as e:
        check("DisplayVersion 可读", False, str(e))

# ---------- 4. 命令在"新 CMD"里可用 ----------
print("\n[4] 模拟新 CMD 的命令可用性 ...")

# --- 4a. 别名投放 (安装目录 + WindowsApps) ---
for a in ALIASES:
    check("%s.exe 在安装目录" % a, os.path.isfile(os.path.join(INSTALL_DIR, a + ".exe")))
for a in ALIASES:
    check("%s.exe 已投放 WindowsApps" % a,
          os.path.isfile(os.path.join(WINDOWSAPPS, a + ".exe")))

# --- 4b. 干净 PATH(只含系统目录 + WindowsApps, 不含安装目录) ---
sysroot = os.environ.get("SystemRoot", r"C:\Windows")
clean = ";".join([sysroot, os.path.join(sysroot, "System32"),
                  os.path.join(sysroot, "System32", "Wbem"), WINDOWSAPPS])
clean_env = dict(os.environ)
clean_env["PATH"] = clean
for cmd, kw in [("cdc", "cdc download"), ("cdcgb", "cdcgb"), ("cdccmd help", "cdcgb")]:
    try:
        r = subprocess.run(["cmd", "/c", cmd], capture_output=True, env=clean_env,
                           timeout=90, stdin=subprocess.DEVNULL)
        o = dec(r.stdout) + dec(r.stderr)
        check("干净 PATH 下可调用 '%s'" % cmd,
              "不是内部或外部命令" not in o and kw in o)
    except subprocess.TimeoutExpired:
        check("干净 PATH 下可调用 '%s'" % cmd, False, "(超时)")

env = dict(os.environ)
env["PATH"] = INSTALL_DIR + ";" + env.get("PATH", "")

# 调试: 确认走到这里时安装目录与主程序都还在
check("安装目录仍存在", os.path.isdir(INSTALL_DIR),
      "-> %s" % (sorted(os.listdir(INSTALL_DIR)) if os.path.isdir(INSTALL_DIR) else "MISS"))
check("主程序仍存在", os.path.isfile(EXE))

# help
p = subprocess.run([EXE, "help"], capture_output=True, env=env, timeout=60)
h = dec(p.stdout)
check("cdccmd help 可运行", p.returncode == 0 and "cdcgb" in h)
for kw in ["cdc download", "cdc releases", "jcgx", "uncdccmd"]:
    check("help 含 '%s'" % kw, kw in h)

# 用 shell 解析验证 PATH 真生效 (这才是用户敲命令时的走法)。
# 注意: 不要在这里执行 uncdccmd —— 它会真的启动卸载程序, 把安装目录删掉,
#       跟后面 [5] 段的官方卸载器打架。用 where 验证"能被解析到"就够了。
for cmd, kw in [("cdccmd help", "cdcgb"), ("cdc", "cdc download"),
                ("cdcgb", "cdcgb"), ("jcgx", "检查")]:
    try:
        p = subprocess.run(["cmd", "/c", cmd], capture_output=True, env=env,
                           timeout=120, stdin=subprocess.DEVNULL)
        out = dec(p.stdout) + dec(p.stderr)
        check("PATH 里能直接调 '%s'" % cmd,
              "不是内部或外部命令" not in out and kw in out,
              "-> %s" % (out.strip().splitlines()[0][:70] if out.strip() else "(无输出)"))
    except subprocess.TimeoutExpired:
        check("PATH 里能直接调 '%s'" % cmd, False, "(超时)")

p = subprocess.run(["cmd", "/c", "where uncdccmd"], capture_output=True,
                   env=env, timeout=30)
check("PATH 里能解析到 uncdccmd (不执行)",
      p.returncode == 0 and "uncdccmd" in dec(p.stdout).lower())

# ---------- 5. 卸载 ----------
print("\n[5] 静默卸载 ...")
if os.path.isfile(UNINS):
    p = subprocess.run([UNINS, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"],
                       capture_output=True, timeout=300)
    check("卸载程序退出码 0", p.returncode == 0, "(rc=%d)" % p.returncode)
    for _ in range(30):
        if not os.path.isdir(INSTALL_DIR):
            break
        time.sleep(1)

check("安装目录已删除", not os.path.isdir(INSTALL_DIR))

pathv2 = user_path()
segs2 = [s for s in pathv2.split(";") if s.strip()]
still = any(s.rstrip("\\").lower() == INSTALL_DIR.lower().rstrip("\\") for s in segs2)
check("PATH 已移除安装目录", not still)

check("卸载注册项已删除", len(enum_uninstall_keys()) == 0,
      "-> %s" % enum_uninstall_keys())

left_wa = [a for a in ALIASES if os.path.isfile(os.path.join(WINDOWSAPPS, a + ".exe"))]
check("WindowsApps 里的别名已清理", not left_wa, "残留: %s" % left_wa)

print("\n" + "=" * 64)
print("  结果: %d 项通过, %d 项失败" % (ok, fail))
print("=" * 64)
sys.exit(1 if fail else 0)
