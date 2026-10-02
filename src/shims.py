# -*- coding: utf-8 -*-
"""shims.py -- 为 CDCCMD 生成 / 清理"直接可敲"的命令别名

原理:
  Windows 的 CreateProcess / PATHEXT 只认得"可执行文件名", 不认 exe 的子命令。
  所以 `jcgx` 不可能靠 PATH 解析到 `cdccmd.exe jcgx`。
  两种做法:
    A. .cmd 转发脚本   -- 通用, 但多一个 cmd.exe 进程 + 参数转义坑
    B. exe 副本/硬链接 -- cdccmd.exe 内部按 argv[0] 分派, 零开销, 最干净  <-- 采用

本模块给 build.py / ISS 提供同一份别名清单。
"""
import os
import shutil

# 需要暴露成独立命令的别名 -> 子命令
# 注意: 必须含 "cdccmd"(主名) —— 否则只把别名放进 WindowsApps 时,
# 用户敲 cdccmd 仍找不到主程序
ALIASES = ["cdccmd", "cdc", "cdcgb", "jcgx", "uncdccmd"]


def make_alias_exes(app_dir, main_exe="cdccmd.exe", log=print):
    """在 app_dir 里为每个别名生成 main_exe 的副本(用硬链接省空间, 失败退回复制)。

    返回: 已成功创建的别名列表
    """
    src = os.path.join(app_dir, main_exe)
    if not os.path.isfile(src):
        log("  [x] 主程序不存在: %s" % src)
        return []
    made = []
    for a in ALIASES:
        dst = os.path.join(app_dir, a + ".exe")
        # 安装目录里的 cdccmd.exe 就是源文件本身, 不能删掉再建链接
        if os.path.normcase(os.path.abspath(dst)) == os.path.normcase(os.path.abspath(src)):
            made.append(a)
            continue
        try:
            if os.path.exists(dst):
                os.remove(dst)
        except Exception:
            pass
        # 优先硬链接(同卷, 零额外空间)
        ok = False
        try:
            os.link(src, dst)
            ok = True
        except Exception:
            ok = False
        if not ok:
            try:
                shutil.copy2(src, dst)
                ok = True
            except Exception as e:
                log("  [x] %s.exe 创建失败: %s" % (a, e))
        if ok:
            made.append(a)
    return made


if __name__ == "__main__":
    import sys
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    print("aliases:", make_alias_exes(d))
