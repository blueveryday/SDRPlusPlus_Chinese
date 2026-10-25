#!/usr/bin/env python3

import os
import re
import sys

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding='utf-8', errors='replace')
    except (AttributeError, ValueError):
        pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8', newline='') as f:
        return f.read()


def write(path, text):
    with open(os.path.join(ROOT, path), 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def sub_once(path, pattern, repl, text):
    new, n = re.subn(pattern, repl, text, count=1, flags=re.M)
    if n != 1:
        print('错误: 在 %s 里找不到版本号位置（%s）' % (path, pattern), file=sys.stderr)
        sys.exit(1)
    return new


def main():
    raw = read('version.txt').strip()
    m = re.match(r'^[vV]?(\d+\.\d+\.\d+)', raw)
    if not m:
        print('错误: version.txt 内容 "%s" 不是 v主.次.修订[后缀] 的格式' % raw, file=sys.stderr)
        return 1
    num = m.group(1)
    print('version.txt = %s  ->  数字版本 %s' % (raw, num))

    changed = []

    def update(path, pattern, repl):
        old = read(path)
        new = sub_once(path, pattern, repl, old)
        if new != old:
            write(path, new)
            changed.append(path)

    update('core/src/version.h', r'^(#define VERSION_STR )"[^"]*"', r'\g<1>"%s"' % num)

    update('make_macos_bundle.sh', r'(bundle_create_plist sdrpp SDR\+\+ org\.sdrpp\.sdrpp )\S+', r'\g<1>%s' % num)

    update('make_debian_package.sh', r'(echo Version: )[0-9][^\s$]*(\$BUILD_NO)', r'\g<1>%s\g<2>' % num)

    print('已更新: %s' % (', '.join(changed) if changed else '无（版本号已经一致）'))
    return 0


if __name__ == '__main__':
    sys.exit(main())