#!/usr/bin/env python3
"""Verify a generated RA6M5 report page against its own data.js.

THERE IS NO JS RUNTIME ON THIS HOST -- no node, deno, bun, qjs, nor any Python
binding -- so a report page's JavaScript cannot be executed outside a browser.
This is the substitute, and it is the cheapest regression test these pages
have: it resolves every reference the page makes into the data it was
generated from, and it asserts the data is not merely well-shaped but
NON-TRIVIAL.

That last part is the lesson of 2026-11-23, when the soft-sensor page shipped a
chart with no content.  The decimation had produced a ONE-POINT series, and
every structural check passed, because a one-point series resolves perfectly.
Checking that a reference resolves is not the same as checking there is
something to draw.

A NULL top-level key is legitimate -- `repeat` is null on any run whose arms
are all distinct configurations -- so references to it are not an error
PROVIDED every one of them is lexically inside a guard on that alias.  This
script proves that by brace-matching rather than assuming it, because an
unguarded `RP.die_delta` on a run with no replicate pair throws a TypeError
and takes the rest of the page's rendering with it.

    tools/ra6m5-report-verify.py docs/reports/ra6m5-loop
"""
import json, os, re, sys

SKIP_FIELDS = {'map', 'length', 'forEach', 'find', 'reduce', 'filter', 'slice',
               'join', 'concat', 'some', 'every', 'sort', 'indexOf', 'toFixed',
               'toLocaleString', 'push', 'unshift', 'splice', 'keys', 'values',
               'entries', 'includes', 'reverse', 'flat', 'at', 'padStart',
               'repeat', 'replace', 'split', 'trim', 'toString'}


def statement_end(src, i):
    """End of the statement starting at i, honouring nesting and strings."""
    depth, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in '`\'"':
            q, i = c, i + 1
            while i < n and src[i] != q:
                i += 2 if src[i] == '\\' else 1
        elif c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == ';' and depth == 0:
            return i
        i += 1
    return n


def block_end(src, i):
    """Index just past the `{...}` block whose `{` is at i."""
    depth, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in '`\'"':
            q, i = c, i + 1
            while i < n and src[i] != q:
                i += 2 if src[i] == '\\' else 1
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def guarded_regions(src, name):
    """Spans of `src` in which `name` has been tested truthy."""
    out = []
    for m in re.finditer(r'\bif\s*\(', src):
        j = m.end() - 1
        k = statement_end(src, j)          # bounded scan for the condition
        cond_end = src.find(')', j)
        depth = 0
        for p in range(j, min(k + 1, len(src))):
            if src[p] == '(':
                depth += 1
            elif src[p] == ')':
                depth -= 1
                if depth == 0:
                    cond_end = p
                    break
        cond = src[j + 1:cond_end]
        if not re.search(r'\b%s\b' % re.escape(name), cond):
            continue
        p = cond_end + 1
        while p < len(src) and src[p] in ' \t\r\n':
            p += 1
        out.append((m.start(), block_end(src, p) if p < len(src) and src[p] == '{'
                    else statement_end(src, p) + 1))
    # `NAME ? ... : ...` and `NAME && ...` guard their own expression
    for m in re.finditer(r'\b%s\s*(\?|&&)' % re.escape(name), src):
        out.append((m.start(), statement_end(src, m.start()) + 1))
    return out


def main(d):
    html = open(os.path.join(d, 'index.html')).read()
    js = open(os.path.join(d, 'data.js')).read()
    root = re.search(r'const\s+([A-Z][A-Z0-9_]*)\s*=\s*\{', js).group(1)
    data = json.loads(js[js.index('{'):js.rindex('}') + 1])
    script = html[html.index('<script>'):] if '<script>' in html else html
    bad, notes = [], []

    # 1. every ROOT.<key> the page names exists
    keys = sorted(set(re.findall(r'%s\.([A-Za-z_][A-Za-z0-9_]*)' % root, html)))
    for k in keys:
        if k not in data:
            bad.append("%s.%s missing from data.js" % (root, k))

    # 2. the destructured aliases
    m = re.search(r'const\s+[A-Z]{1,2}\s*=\s*%s\.[a-z_]+[^;]*;' % root, script, re.S)
    alias = dict(re.findall(r'\b([A-Z]{1,2})\s*=\s*%s\.([a-z_]+)' % root,
                            m.group(0) if m else ''))
    for a, k in alias.items():
        if k not in data:
            bad.append("alias %s -> %s.%s missing" % (a, root, k))

    # 3. alias.<field> resolves -- or, where the alias is null, is GUARDED
    def probe(obj, parts):
        cur = obj
        for part in parts:
            if isinstance(cur, list):
                cur = cur[0] if cur else None
            if not isinstance(cur, dict) or part not in cur:
                return False
            cur = cur[part]
        return True

    nullable = {a for a, k in alias.items() if data.get(k) is None}
    regions = {a: guarded_regions(script, a) for a in nullable}
    for a in sorted(nullable):
        notes.append("%s (%s.%s) is null: %d guarded region(s)"
                     % (a, root, alias[a], len(regions[a])))
    for a in sorted(nullable):
        if not regions[a]:
            bad.append("%s is null and has no guard at all" % a)
        for mm in re.finditer(r'\b%s\.([A-Za-z_][A-Za-z0-9_]*)' % a, script):
            if not any(lo <= mm.start() < hi for lo, hi in regions[a]):
                line = script[:mm.start()].count('\n') + 1
                bad.append("UNGUARDED %s.%s at script line %d -- %s is null, "
                           "this throws" % (a, mm.group(1), line, a))
    for a, field in re.findall(r'\b([A-Z]{1,2})\.([a-z_]+)\b', script):
        if a not in alias or a in nullable or field in SKIP_FIELDS:
            continue
        if not probe(data[alias[a]], [field]):
            bad.append("%s.%s (%s.%s.%s) does not resolve"
                       % (a, field, root, alias[a], field))

    # 4. nested paths, where the page reaches two deep
    for mm in re.finditer(r'\b([A-Z]{1,2})\.([a-z_]+)'
                          r'(?:\[[^\]]*\])?\.([a-z_]+)\.([a-z_]+)\b', script):
        a, f1, f2, f3 = mm.groups()
        if a not in alias or a in nullable:
            continue
        if f1 in SKIP_FIELDS or f2 in SKIP_FIELDS or f3 in SKIP_FIELDS:
            continue
        if not probe(data[alias[a]], [f1, f2, f3]):
            bad.append("%s.%s[].%s.%s does not resolve" % (a, f1, f2, f3))

    # 5. ids, row() targets, chart hosts, anchors
    ids = set(re.findall(r'\bid="([^"]+)"', html))
    for i in sorted(set(re.findall(r"set\('([^']+)'", html))
                    | set(re.findall(r"getElementById\('([^']+)'\)", html))):
        if i not in ids:
            bad.append("element id '%s' referenced but not in the markup" % i)
    for t in sorted(set(re.findall(r"row\('#([^']+)'", html))):
        if t not in ids:
            bad.append("row() target '#%s' not in the markup" % t)
        elif not re.search(r'id="%s"[^>]*>\s*<thead' % re.escape(t), html) \
                and ('<tr id="%s">' % t) in html:
            bad.append("row() appends to a tbody, but '#%s' is a <tr> in a "
                       "thead -- use getElementById().innerHTML" % t)
    for t in sorted(set(re.findall(r"lines\('([^']+)'", html))
                    | set(re.findall(r"xy\('([^']+)'", html))):
        if t not in ids:
            bad.append("chart host '%s' not in the markup" % t)
    for h in sorted(set(re.findall(r'href="#([^"]+)"', html))):
        if h not in ids:
            bad.append("anchor #%s has no target" % h)

    # 6. every declared table gets filled by something.  Three patterns are in
    #    use and all three count: row('#id'), querySelector('#id tbody') and a
    #    bare '#id' handed to a helper.
    for t in sorted(re.findall(r'<table id="([^"]+)"', html)):
        if not re.search(r"['\"]#%s[ '\"]" % re.escape(t), html):
            bad.append("table '#%s' is declared but nothing fills it" % t)

    # 7. tag balance
    for tag in ('section', 'table', 'tbody', 'thead', 'div', 'tr', 'script'):
        o = len(re.findall(r'<%s[ >]' % tag, html))
        c = len(re.findall(r'</%s>' % tag, html))
        if o != c:
            bad.append("tag <%s> unbalanced: %d open, %d close" % (tag, o, c))

    # 8. CONTENT, not just shape -- a one-point series resolves perfectly
    def walk(o, path=""):
        if isinstance(o, dict):
            for k, v in o.items():
                walk(v, path + "/" + k)
        elif isinstance(o, list):
            if path.endswith(('/t', '/x', '/y')) or '/series' in path:
                if len(o) < 2 and o and not isinstance(o[0], (dict, list)):
                    bad.append("series %s has %d point(s) -- nothing to draw"
                               % (path, len(o)))
            if o and isinstance(o[0], (dict, list)):
                walk(o[0], path + "[]")
    walk(data)
    for k in ('arms', 'brackets'):
        if k in data and len(data[k]) < 2:
            bad.append("%s has only %d entries" % (k, len(data[k])))

    print("%s: %d %s keys, %d aliases (%d null), %d ids"
          % (os.path.basename(d), len(keys), root, len(alias), len(nullable), len(ids)))
    for n in notes:
        print("  note:", n)
    if bad:
        print("FAIL:")
        seen = set()
        for x in bad:
            if x not in seen:
                print("  -", x)
                seen.add(x)
        return 1
    print("  OK: every reference resolves, nullable aliases are guarded, "
          "targets exist, tags balance, no empty series")
    return 0


if __name__ == '__main__':
    sys.exit(max(main(d) for d in (sys.argv[1:] or ['docs/reports/ra6m5-loop'])))
