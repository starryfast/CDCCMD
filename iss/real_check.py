# -*- coding: utf-8 -*-
"""真机验证: 装 ISS 包 -> 用"干净 PATH"(模拟用户新开 CMD) 敲 5 条命令 -> 卸载"""
import subprocess, os, time, winreg

SETUP = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     'build', 'cdc-setup-iss.exe')
DIR = os.path.expandvars(r'%LOCALAPPDATA%\Programs\cdccmd')
UNINS = os.path.join(DIR, 'unins000.exe')


def dec(b):
    return (b or b'').decode('gbk', 'replace')


def machine_path():
    """读注册表里的用户 PATH, 拼一个"新 CMD 会看到的"环境"""
    k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Environment')
    p = winreg.QueryValueEx(k, 'Path')[0]
    sysp = os.environ.get('SystemRoot', r'C:\Windows')
    syspath = r'%s\system32;%s;%s\System32\Wbem' % (sysp, sysp, sysp)
    return syspath + ';' + p


print('== 1. 静默安装 ==')
subprocess.run([SETUP, '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'],
               capture_output=True, timeout=300)
for _ in range(20):
    if os.path.isfile(os.path.join(DIR, 'cdccmd.exe')):
        break
    time.sleep(1)

env = dict(os.environ)
env['PATH'] = machine_path()
print('   PATH 片段:', env['PATH'][-120:])

print('\n== 2. 敲命令(模拟用户新开 CMD) ==')
# 注意: 这里只验"命令能否被 cmd 识别"; 不真跑 cdc download(123MB, 本机 GitHub 出口极慢)
tests = [
    ('cdc',           '用法: cdc download'),
    ('cdcgb',         '用法: cdcgb'),
    ('cdccmd help',   'cdcgb'),
    ('jcgx',          '检查 CDCCMD 更新'),
]
allok = True
for cmd, expect in tests:
    try:
        p = subprocess.run(['cmd', '/c', cmd], capture_output=True, env=env, timeout=40)
        out = dec(p.stdout) + dec(p.stderr)
    except subprocess.TimeoutExpired:
        out = '<timeout: 命令已启动(可能在联网下载/检查)>'
    bad = '不是内部或外部命令' in out
    first = out.strip().splitlines()[0] if out.strip() else '(空)'
    print('  %-16s -> %s' % (cmd, first))
    if bad:
        allok = False
        print('     [!] 仍然报"不是内部或外部命令"')

print('\n  结论:', '全部命令可被识别' if allok else '仍有命令识别失败')

print('\n== 3. 卸载复原 ==')
subprocess.run([UNINS, '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'],
               capture_output=True, timeout=300)
for _ in range(30):
    if not os.path.isdir(DIR):
        break
    time.sleep(1)
print('   目录已删:', not os.path.isdir(DIR))
