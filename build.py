# -*- coding: utf-8 -*-
"""
build.py -- CDCCMD 构建脚本 (沙箱安全版)

为什么不用 build.bat / 直接在 bash 里跑 g++:
  本机沙箱里 g++ 的 collect2 链接阶段会以 ld exit 53 静默失败,
  根因是 ld.exe 依赖 mingw64\\bin\\libwinpthread-1.dll, 而子进程 PATH 没带上它
  (表现为 STATUS_DLL_NOT_FOUND 0xC0000135)。
  这里由 Python 显式构造 env['PATH'], 保证链接器能找到所有依赖 DLL。

用法:
  python build.py            # 完整构建, 产物在 build/
  python build.py clean      # 清理 build/
"""
import os
import shutil
import subprocess
import sys
import glob

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
OUT = os.path.join(ROOT, "build")
PAYLOAD = os.path.join(OUT, "payload")

# ★ 改成你自己的 mingw64 路径（或留空, 脚本会自动找 PATH 里的 g++）
MINGW_CANDIDATES = [
    r"C:\mingw64",
    r"C:\msys64\mingw64",
    r"C:\ProgramData\mingw64",
]

CFLAGS = ["-O2", "-std=c++17", "-static", "-static-libgcc", "-static-libstdc++"]
CXXFLAGS = CFLAGS + ["-Wall"]


def log(msg):
    sys.stdout.write(msg + "\n")
    sys.stdout.flush()


def fail(msg):
    log("\n[失败] " + msg)
    sys.exit(1)


def find_mingw():
    for m in MINGW_CANDIDATES:
        if os.path.isfile(os.path.join(m, "bin", "g++.exe")):
            return m
    which = shutil.which("g++")
    if which:
        return os.path.dirname(os.path.dirname(which))
    fail("找不到 mingw64 工具链。请在 build.py 的 MINGW_CANDIDATES 里补上你的路径。")


def build_env(mingw):
    """关键: 把 mingw64\\bin 放在 PATH 最前面, 否则 ld.exe 会因缺 DLL 启动失败。"""
    env = dict(os.environ)
    env["PATH"] = os.path.join(mingw, "bin") + os.pathsep + env.get("PATH", "")
    return env


def run(cmd, env, desc, cwd=None):
    log("  $ " + " ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)))
    p = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True)
    out = (p.stdout or b"").decode("utf-8", "replace")
    err = (p.stderr or b"").decode("utf-8", "replace")
    if out.strip():
        log(out.rstrip())
    if err.strip():
        log(err.rstrip())
    if p.returncode != 0:
        fail("%s (exit %d)" % (desc, p.returncode))
    return out + err


def clean():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT, ignore_errors=True)
    log("[清理] build/ 已删除")


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "clean":
        return clean()

    mingw = find_mingw()
    gpp = os.path.join(mingw, "bin", "g++.exe")
    windres = os.path.join(mingw, "bin", "windres.exe")
    env = build_env(mingw)

    for d in (OUT, PAYLOAD):
        os.makedirs(d, exist_ok=True)

    log("=" * 60)
    log("  CDCCMD 构建")
    log("  工具链: " + gpp)
    log("=" * 60)

    log("\n[1/5] 编译 cdccmd.exe ...")
    run([gpp] + CXXFLAGS + ["-o", os.path.join(OUT, "cdccmd.exe"),
        os.path.join(SRC, "cdccmd.cpp"),
        "-lwinhttp", "-lshell32", "-lole32", "-ladvapi32", "-luuid"],
        env, "编译 cdccmd.exe")

    log("\n[2/5] 编译 uninstall.exe ...")
    run([gpp] + CXXFLAGS + ["-o", os.path.join(OUT, "uninstall.exe"),
        os.path.join(SRC, "uninstall.cpp"),
        "-lshell32", "-luser32", "-ladvapi32", "-lole32", "-luuid"],
        env, "编译 uninstall.exe")

    log("\n[3/5] 准备 payload 资源 ...")
    for name in ("payload.rc", "version.h"):
        shutil.copyfile(os.path.join(SRC, name), os.path.join(PAYLOAD, name))
    for name in ("cdccmd.exe", "uninstall.exe"):
        shutil.copyfile(os.path.join(OUT, name), os.path.join(PAYLOAD, name))
    log("  已复制 payload.rc / version.h / cdccmd.exe / uninstall.exe")

    log("\n[4/5] 生成 payload.o ...")
    run([windres, "payload.rc", "-O", "coff", "-o", "payload.o"],
        env, "windres 资源编译", cwd=PAYLOAD)

    log("\n[5/5] 生成自包含安装包 cdccmd-setup.exe ...")
    run([gpp] + CXXFLAGS + ["-I" + SRC, "-o", os.path.join(OUT, "cdccmd-setup.exe"),
        os.path.join(SRC, "installer.cpp"), os.path.join(PAYLOAD, "payload.o"),
        "-lshell32", "-luser32", "-ladvapi32", "-lole32", "-luuid"],
        env, "编译 cdccmd-setup.exe")

    # windres 需要在 payload 目录里跑(相对路径引用 exe)
    os.remove(os.path.join(PAYLOAD, "payload.o"))

    log("\n" + "=" * 60)
    log("  构建成功!")
    log("=" * 60)
    for name in ("cdccmd.exe", "uninstall.exe", "cdccmd-setup.exe"):
        p = os.path.join(OUT, name)
        if os.path.isfile(p):
            log("  %-20s %8d 字节" % (name, os.path.getsize(p)))
    log("")
    log("  分发: 只需把 build\\cdccmd-setup.exe 发给别人。")
    log("  安装: 双击 -> 关闭窗口 -> 新开 CMD 输入 cdccmd help")


if __name__ == "__main__":
    main()
