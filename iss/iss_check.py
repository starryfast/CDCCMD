# -*- coding: utf-8 -*-
"""iss_check.py -- Inno Setup 脚本静态自检

专治几个已经反复踩到的坑:
  1. 注释块 { ... } 内部再出现花括号 -> 注释被提前闭合, 后面的中文被当代码
     (报错信息往往是 "Syntax error" 且行号指向注释之后, 很难直接看出来)
  2. 字符串里的 #13#10 出现在续行行首
  3. 文件缺少 UTF-8 BOM / 不是 CRLF (Inno 对中文脚本有要求)

用法: python iss_check.py cdc_setup.iss
退出码: 0 = 通过, 1 = 发现问题
"""
import sys
import os


def check_braces_in_comments(text):
    """扫描 [Code] 段的 Pascal 代码, 找出注释块内部的裸花括号。

    只扫 [Code] 段: 其它段的 {xxx} 是 Inno 的常量引用语法(如 {app}),
    而 AppId={{GUID} 里的 {{ 是"转义出一个字面花括号", 都是合法的。
    Pascal 脚本里 { } 才是注释, 嵌套花括号会把注释提前闭合。
    """
    # 定位 [Code] 段
    idx = text.find('[Code]')
    if idx < 0:
        return []
    # 行号基准: 数一下 [Code] 之前有多少换行
    base_line = text[:idx].count('\n')

    body = text[idx:]
    problems = []
    i = 0
    n = len(body)
    line = base_line + 1
    while i < n:
        c = body[i]
        if c == '\n':
            line += 1
            i += 1
            continue
        if c == '/' and i + 1 < n and body[i + 1] == '/':    # 行注释
            while i < n and body[i] != '\n':
                i += 1
            continue
        if c == "'":                     # 字符串字面量: 跳过, '' 是转义
            i += 1
            while i < n:
                if body[i] == "'":
                    if i + 1 < n and body[i + 1] == "'":
                        i += 2
                        continue
                    i += 1
                    break
                if body[i] == '\n':
                    line += 1
                i += 1
            continue
        if c == '{':                     # 注释块开始
            start_line = line
            i += 1
            while i < n:
                if body[i] == '\n':
                    line += 1
                    i += 1
                    continue
                if body[i] == '}':
                    i += 1
                    break
                if body[i] == '{':
                    problems.append(
                        '  第 %d 行注释块(起始于第 %d 行)内出现 "{" -> 注释会提前闭合'
                        % (line, start_line))
                i += 1
            continue
        i += 1
    return problems


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'cdc_setup.iss'
    if not os.path.isfile(path):
        print('找不到文件: %s' % path)
        return 1

    raw = open(path, 'rb').read()
    ok = True

    print('=' * 60)
    print('  Inno Setup 脚本自检: %s' % os.path.basename(path))
    print('=' * 60)

    # 1. BOM / 换行
    has_bom = raw.startswith(b'\xef\xbb\xbf')
    crlf = raw.count(b'\r\n')
    lf = raw.count(b'\n')
    print('[编码] UTF-8 BOM: %s   行数(CRLF): %d / %d' % ('有' if has_bom else '无', crlf, lf))
    if not has_bom:
        print('  [警告] 建议给含中文的 .iss 加 UTF-8 BOM')

    try:
        text = raw.decode('utf-8-sig')
    except UnicodeDecodeError as e:
        print('  [错误] 不是合法 UTF-8: %s' % e)
        return 1

    # 2. 注释内花括号
    problems = check_braces_in_comments(text)
    print('[注释] 注释块内嵌花括号: %d 处' % len(problems))
    for p in problems:
        print(p)
        ok = False

    # 3. 续行行首的 #13#10
    bad_cont = []
    lines = text.split('\n')
    for idx, ln in enumerate(lines):
        if ln.strip().startswith('#13#10'):
            bad_cont.append('  第 %d 行: 续行行首不能是 #13#10' % (idx + 1))
    print('[续行] 行首 #13#10: %d 处' % len(bad_cont))
    for b in bad_cont:
        print(b)
        ok = False

    print('=' * 60)
    print('  结果: %s' % ('通过' if ok else '发现问题'))
    print('=' * 60)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
