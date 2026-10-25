#!/usr/bin/env python3
"""SDR++ CE 界面语言文件维护工具。

程序源码里的界面文字都是中文原文，语言文件（root/res/lang/*.json）是“原文 -> 译文”的对照表。
这个工具从源码里提取全部界面原文，用来生成/更新语言文件，并检查语言文件是否有问题。

用法（在仓库根目录运行）：
  python3 tools/i18n_tool.py extract              列出源码里提取到的所有界面原文（按模块分组）
  python3 tools/i18n_tool.py template             重新生成 root/res/lang/zh-CN.json（原文模板）
  python3 tools/i18n_tool.py check [语言文件...]   检查语言文件：缺少的条目、多余的条目、% 格式符是否一致
  python3 tools/i18n_tool.py update en-US         把源码里新增的原文补进 root/res/lang/en-US.json（译文留空，等待翻译）

语言文件格式见 root/res/lang/README.md。
"""
import json
import os
import re
import sys
from collections import OrderedDict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG_DIR = os.path.join(ROOT, 'root', 'res', 'lang')

CJK = re.compile(r'[　-〿㐀-鿿＀-￯]')
SKIP_DIRS = {'.git', 'wiki', 'docker_builds', 'tools', 'vendor'}
SKIP_PREFIXES = ('core/src/imgui', 'android/')
SRC_EXT = ('.cpp', '.h', '.hpp', '.c')

# 这些调用里的字符串是日志或内部错误信息，不是界面文字
LOG_CALLEES = ('flog::',)


# ---------------------------------------------------------------- C++ 词法
def decode_escapes(body):
    """把 C++ 字符串字面量里的转义序列还原成真正的字符。"""
    out = []
    i = 0
    simple = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"', "'": "'", '0': '\0', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v', '?': '?'}
    while i < len(body):
        c = body[i]
        if c != '\\' or i + 1 >= len(body):
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        if n in 'xX':
            m = re.match(r'[0-9a-fA-F]+', body[i + 2:])
            if m:
                out.append(chr(int(m.group(0), 16)))
                i += 2 + len(m.group(0))
                continue
        if n == 'u' and re.match(r'[0-9a-fA-F]{4}', body[i + 2:i + 6]):
            out.append(chr(int(body[i + 2:i + 6], 16)))
            i += 6
            continue
        if n == 'U' and re.match(r'[0-9a-fA-F]{8}', body[i + 2:i + 10]):
            out.append(chr(int(body[i + 2:i + 10], 16)))
            i += 10
            continue
        if n in '01234567':
            m = re.match(r'[0-7]{1,3}', body[i + 1:])
            out.append(chr(int(m.group(0), 8)))
            i += 1 + len(m.group(0))
            continue
        out.append(simple.get(n, n))
        i += 2
    return ''.join(out)


def tokenize(text):
    """极简 C++ 词法：返回 (kind, value, line) 列表。kind: str / id / op。
    字符串、字符字面量、注释都会被正确跳过；相邻的字符串字面量由调用方合并。"""
    toks = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == '\n':
            line += 1
            i += 1
        elif c in ' \t\r':
            i += 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            line += text.count('\n', i, j)
            i = j
        elif c == '#' and (not toks or True) and re.match(r'#\s*(include|define|if|ifdef|ifndef|else|elif|endif|pragma|undef|error)', text[i:i + 12]):
            # 预处理指令：宏定义里的字符串也要保留，所以只跳过 include 行，其余当普通记号处理
            if re.match(r'#\s*include', text[i:i + 12]):
                j = text.find('\n', i)
                i = n if j < 0 else j
            else:
                toks.append(('op', '#', line))
                i += 1
        elif c == '"' or (c in 'uUL' and text.startswith('"', i + 1)) or text.startswith('u8"', i):
            if c != '"':
                i = text.index('"', i)
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == '\\':
                    j += 1
                if j < n and text[j] == '\n':
                    line += 1
                j += 1
            toks.append(('str', decode_escapes(text[i + 1:j]), line))
            i = j + 1
        elif c == 'R' and text.startswith('R"', i):
            m = re.match(r'R"([^(\s]{0,16})\(', text[i:])
            if m:
                end = ')' + m.group(1) + '"'
                j = text.find(end, i + len(m.group(0)))
                j = n if j < 0 else j
                toks.append(('str', text[i + len(m.group(0)):j], line))
                line += text.count('\n', i, j)
                i = j + len(end)
            else:
                toks.append(('id', 'R', line))
                i += 1
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                if text[j] == '\\':
                    j += 1
                j += 1
            i = j + 1
        elif c.isalpha() or c == '_':
            m = re.match(r'[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*', text[i:])
            toks.append(('id', m.group(0), line))
            i += len(m.group(0))
        elif c.isdigit():
            m = re.match(r'[0-9][0-9A-Za-z_.]*', text[i:])
            toks.append(('num', m.group(0), line))
            i += len(m.group(0))
        else:
            toks.append(('op', c, line))
            i += 1
    return toks


def extract_file(path):
    """返回 [(原文, 行号, 是否是拼接表达式的一部分)]"""
    text = open(path, encoding='utf-8', errors='replace').read()
    toks = tokenize(text)
    results = []
    stack = []  # 当前所在括号的“被调用者”名字
    i = 0
    while i < len(toks):
        kind, val, line = toks[i]
        if kind == 'op' and val == '(':
            callee = toks[i - 1][1] if i > 0 and toks[i - 1][0] == 'id' else ''
            stack.append(callee)
        elif kind == 'op' and val == ')':
            if stack:
                stack.pop()
        elif kind == 'str':
            # 合并相邻的字符串字面量（C++ 会在编译期拼成一个）
            j = i
            parts = [val]
            while j + 1 < len(toks) and toks[j + 1][0] == 'str':
                j += 1
                parts.append(toks[j][1])
            s = ''.join(parts)
            prev = toks[i - 1] if i > 0 else None
            nxt = toks[j + 1] if j + 1 < len(toks) else None
            concat = (prev is not None and prev[0] == 'op' and prev[1] == '+') or (nxt is not None and nxt[0] == 'op' and nxt[1] == '+')
            is_log = any(c.startswith(LOG_CALLEES) for c in stack)
            if CJK.search(s) and not is_log:
                results.append((s, line, concat))
            i = j
        i += 1
    return results


def section_of(rel):
    parts = rel.split('/')
    if parts[0] == 'core':
        return 'core'
    if parts[0] in ('misc_modules', 'decoder_modules', 'source_modules', 'sink_modules') and len(parts) > 1:
        name = parts[1]
        return {'radio': 'radio'}.get(name, name)
    return parts[0]


def collect():
    """返回 OrderedDict[分组] -> OrderedDict[原文] -> 备注；以及 拼接类原文 的集合。"""
    sections = {}
    concat_only = {}
    seen = {}
    for root, dirs, files in os.walk(ROOT):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for f in sorted(files):
            if not f.endswith(SRC_EXT):
                continue
            full = os.path.join(root, f)
            rel = os.path.relpath(full, ROOT).replace(os.sep, '/')
            if rel.startswith(SKIP_PREFIXES) or '/vendor/' in rel:
                continue
            for s, line, concat in extract_file(full):
                # "导入##_freq_mgr_imp_" 里 ## 之后是控件 ID，不参与翻译
                # 组合框的选项列表 "甲\0乙\0" 在绘制时是逐项显示的，所以按单项提取
                for key in s.split('\0') if '\0' in s else [s]:
                    key = key.split('##', 1)[0]
                    if not key or not CJK.search(key):
                        continue
                    sec = section_of(rel)
                    only = concat and '##' not in s and '\0' not in s
                    if key not in seen:
                        seen[key] = sec
                        sections.setdefault(sec, OrderedDict())[key] = '%s:%d' % (rel, line)
                        concat_only[key] = only
                    else:
                        concat_only[key] = concat_only[key] and only
    # 资源文件里显示在界面上的名称：主题名、颜色映射名、频段划分（国家）名
    for sub in ('themes', 'colormaps', 'bandplans'):
        d = os.path.join(ROOT, 'root', 'res', sub)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if not f.endswith('.json'):
                continue
            try:
                with open(os.path.join(d, f), encoding='utf-8-sig') as fh:
                    nm = json.load(fh).get('name')
            except Exception:
                continue
            if isinstance(nm, str) and CJK.search(nm) and nm not in seen:
                seen[nm] = 'resources'
                sections.setdefault('resources', OrderedDict())[nm] = 'root/res/%s/%s' % (sub, f)
                concat_only[nm] = False
    ordered = OrderedDict()
    first = ['core'] + sorted(k for k in sections if k != 'core')
    for k in first:
        if k in sections:
            ordered[k] = sections[k]
    return ordered, {k for k, v in concat_only.items() if v}


# ---------------------------------------------------------------- 格式符
def format_signature(text):
    """和 core/src/utils/i18n.cpp 里的 formatSignature 保持一致。"""
    sig, i, n = [], 0, len(text)
    while i < n:
        if text[i] != '%':
            i += 1
            continue
        j = i + 1
        if j < n and text[j] == '%':
            i = j + 1
            continue
        spec = ''
        while j < n and text[j] in "-+ #0'":
            j += 1
        if j < n and text[j] == '*':
            spec += '*'
            j += 1
        else:
            while j < n and text[j].isdigit():
                j += 1
        if j < n and text[j] == '.':
            j += 1
            if j < n and text[j] == '*':
                spec += '*'
                j += 1
            else:
                while j < n and text[j].isdigit():
                    j += 1
        while j < n and text[j] in 'hlLqjzt':
            spec += text[j]
            j += 1
        if j < n and text[j] in 'diouxXeEfFgGaAcspn':
            spec += text[j]
            sig.append(spec)
            i = j + 1
        else:
            i += 1
    return '|'.join(sig)


# ---------------------------------------------------------------- 语言文件
def load_lang(path):
    with open(path, encoding='utf-8-sig') as f:
        doc = json.load(f, object_pairs_hook=OrderedDict)
    flat = OrderedDict()

    def walk(node):
        for k, v in node.items():
            if k.startswith('//') or k.startswith('_'):
                continue
            if isinstance(v, dict):
                walk(v)
            else:
                flat[k] = v
    walk(doc.get('strings', {}))
    return doc, flat


def write_lang(path, meta, sections_values, header_comment):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    doc = OrderedDict()
    doc['format'] = 1
    doc['meta'] = meta
    strings = OrderedDict()
    strings['// 说明'] = header_comment
    for sec, items in sections_values.items():
        strings[sec] = items
    doc['strings'] = strings
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
        f.write('\n')


ZH_META = OrderedDict([
    ('code', 'zh-CN'), ('name', 'Chinese (Simplified)'), ('native_name', '简体中文'),
    ('author', 'SDR++ CE'), ('version', '1.0'),
])
ZH_COMMENT = '这是原文模板：左边是程序里的中文原文（请不要修改），右边是显示的文字。要做新的语言，复制本文件改名为语言标签（如 ja-JP.json），修改 meta 并翻译右边的文字即可。详见 README.md。'


def cmd_extract():
    sections, concat = collect()
    total = 0
    for sec, items in sections.items():
        print('== %s (%d)' % (sec, len(items)))
        for k, where in items.items():
            mark = '  [拼接]' if k in concat else ''
            print('  %-40s %s%s' % (where, json.dumps(k, ensure_ascii=False), mark))
        total += len(items)
    print('\n共 %d 条原文，其中 %d 条只出现在字符串拼接里（运行时拼成的整句不会被翻译）' % (total, len(concat)))


def cmd_template():
    sections, _ = collect()
    values = OrderedDict((sec, OrderedDict((k, k) for k in items)) for sec, items in sections.items())
    write_lang(os.path.join(LANG_DIR, 'zh-CN.json'), ZH_META, values, ZH_COMMENT)
    print('已生成 root/res/lang/zh-CN.json（%d 条）' % sum(len(v) for v in values.values()))


def cmd_check(paths):
    sections, _ = collect()
    keys = OrderedDict()
    for sec, items in sections.items():
        for k in items:
            keys[k] = sec
    if not paths:
        paths = sorted(os.path.join(LANG_DIR, f) for f in os.listdir(LANG_DIR) if f.endswith('.json'))
    bad = 0
    for p in paths:
        try:
            doc, flat = load_lang(p)
        except Exception as e:
            print('%s: 无法读取: %s' % (os.path.basename(p), e))
            bad += 1
            continue
        missing = [k for k in keys if k not in flat or flat[k] == '']
        extra = [k for k in flat if k not in keys]
        fmt = [k for k, v in flat.items() if isinstance(v, str) and v and format_signature(k) != format_signature(v)]
        nonstr = [k for k, v in flat.items() if not isinstance(v, str)]
        print('%s: %d 条译文；缺少 %d；源码里已不存在 %d；格式符不一致 %d；非字符串 %d' % (
            os.path.basename(p), len(flat), len(missing), len(extra), len(fmt), len(nonstr)))
        for k in fmt:
            print('   格式符不一致: %s -> %s' % (json.dumps(k, ensure_ascii=False), json.dumps(flat[k], ensure_ascii=False)))
            bad += 1
        for k in nonstr:
            bad += 1
        if os.path.basename(p) != 'zh-CN.json':
            for k in missing[:20]:
                print('   缺少: %s' % json.dumps(k, ensure_ascii=False))
            if len(missing) > 20:
                print('   ... 还有 %d 条' % (len(missing) - 20))
    return 1 if bad else 0


def cmd_update(code):
    path = os.path.join(LANG_DIR, code + '.json')
    sections, _ = collect()
    doc, flat = load_lang(path)
    values = OrderedDict()
    added = 0
    for sec, items in sections.items():
        values[sec] = OrderedDict()
        for k in items:
            if k in flat:
                values[sec][k] = flat[k]
            else:
                values[sec][k] = ''
                added += 1
    comment = doc.get('strings', {}).get('// 说明', '')
    write_lang(path, doc.get('meta', OrderedDict()), values, comment)
    print('%s: 新增 %d 条待翻译（译文为空，加载时会被忽略，显示原文）' % (code, added))


def main(argv):
    if len(argv) < 2 or argv[1] in ('-h', '--help'):
        print(__doc__)
        return 0
    cmd = argv[1]
    if cmd == 'extract':
        cmd_extract()
    elif cmd == 'template':
        cmd_template()
    elif cmd == 'check':
        return cmd_check(argv[2:])
    elif cmd == 'update' and len(argv) == 3:
        cmd_update(argv[2])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
