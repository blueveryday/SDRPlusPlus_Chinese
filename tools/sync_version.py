#!/usr/bin/env python3
"""把仓库根目录 version.txt 里的版本号同步到各平台写死版本号的地方。

version.txt 是唯一的版本号来源，例如：  v1.3.0_CE
  - 完整版本  v1.3.0_CE   用于发布文件名
  - 数字版本  1.3.0       用于程序界面、macOS Info.plist、Debian 包
Android 的 versionName / versionCode 已由 android/app/build.gradle 直接读取 version.txt，这里不需要处理。

GitHub Actions 在每个构建任务的开头运行本脚本，这样改了 version.txt 就够了，编译出来的程序不会再显示旧版本号。
也可以在本地手动运行：  python3 tools/sync_version.py
"""
import os
import re
import sys

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
