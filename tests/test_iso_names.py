"""ISO-NAMES (Sprint 2 contract r4 §2.2): naming rule for the Sprint 2 HIL-only code.

Why a static test: the release scan (tools/check_release_no_hil.py) can only ban
what it can name. So every function or object the Sprint 2 diff DEFINES in HIL-only
code must contain one of the banned symbol parts (then the ELF symbol scan binds it,
and with it the generic single-token trace labels P/POP/WR/... those functions emit),
and every distinctive string literal the diff adds there must start with SDL_/SLT_
(banned as byte prefixes anywhere in the release image) or be a listed command/marker.

HIL-only code = components/evq_hil/**, components/event_log/evq_hil_* (and its
include/evq_hil_* headers), plus lines that the preprocessor keeps with
CONFIG_AMBYTE_EVQ_HIL=1 but drops with it 0 (object-like #defines such as
event_log.c's EVQ_HIL_ON are followed). Parsed from `git diff 4ec9cef` of the
working tree (whole-file context), so it runs unchanged on the candidate C2.

"Distinctive" string literal (the rule's operative definition here): a literal that
starts - after leading whitespace/CR/LF - with an upper-case console tag of >= 2
characters ([A-Z][A-Z0-9_]+ followed by end, space, '%', ':' or '='), i.e. the shape
of every marker the host tools parse. Lower-case text (error reasons, format
fragments, command words) cannot be a parsed marker. A tag already present anywhere
in the 4ec9cef components tree is not new and passes (e.g. HIL_FAULT, HIL_ERR, EVQ_*).
"""
from __future__ import annotations

import re
import subprocess
import unittest
import warnings
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = "4ec9cef"
SYMBOL_PARTS = ("evq_hil", "evq_arm", "evq_tr_", "hil_sdl", "sdl_hil", "slt_", "sd_logger_hil")
HIL_PATHS = (re.compile(r"^components/evq_hil/"), re.compile(r"^components/event_log/evq_hil_"),
             re.compile(r"^components/event_log/include/evq_hil_"))
LISTED = {"HIL_FAULT", "HILSDLOG", "SDL_BEGIN", "SDL_END", "SDL_FF", "SDL_FL", "SDL_STATE", "SDL_MORE",
          "SDL_INVALID", "SDL_TIMEOUT", "SDL_B64", "SDL_DUMP_END", "SDL_Q", "SLT_HDR", "SLT_DRAIN", "SLT_WM",
          "SLT_ERR", "HIL_ERR", "HIL_OK"}
GENERIC_LABELS = {"P", "POP", "WR", "COMMIT", "RB", "ROT", "OPEN", "CLOSE", "DROP", "QUI"}
TAG_RE = re.compile(r"^(?:\s|\\[rnt])*([A-Z][A-Z0-9_]+)(?=$|[\s%:=]|\\[rnt])")
C_KEYWORDS = {"static", "const", "volatile", "extern", "inline", "unsigned", "signed", "int", "char", "short",
              "long", "float", "double", "void", "bool", "struct", "union", "enum", "typedef", "register",
              "restrict", "_Atomic", "_Thread_local", "__inline", "__inline__", "__restrict"}


# =========================================================================== diff

@dataclass
class FileDiff:
    path: str
    new: list[str] = field(default_factory=list)
    old: list[str] = field(default_factory=list)
    added: set[int] = field(default_factory=set)      # 1-based line numbers in `new`


def parse_diff(text: str) -> list[FileDiff]:
    """Unified diff (ideally whole-file context, -U<big>) -> new/old sides + added lines."""
    files: list[FileDiff] = []
    cur: FileDiff | None = None
    for ln in text.split("\n"):
        if ln.startswith("diff --git "):
            cur = None
            continue
        if ln.startswith("+++ "):
            p = ln[4:].strip()
            if p == "/dev/null":
                cur = None
                continue
            cur = FileDiff(p[2:] if p.startswith("b/") else p)
            files.append(cur)
            continue
        if ln.startswith("--- ") or cur is None or ln.startswith("@@") or ln.startswith("\\"):
            continue
        if ln.startswith("+"):
            cur.new.append(ln[1:])
            cur.added.add(len(cur.new))
        elif ln.startswith("-"):
            cur.old.append(ln[1:])
        elif ln.startswith(" ") or ln == "":
            cur.new.append(ln[1:])
            cur.old.append(ln[1:])
    return files


def git_diff() -> str:
    r = subprocess.run(["git", "diff", "--no-color", "-U1000000", BASE, "--", "*.c", "*.h"], cwd=ROOT,
                       capture_output=True, text=True, check=True)
    return r.stdout


def untracked_sources() -> list[FileDiff]:
    """Not-yet-committed new sources (a working tree mid-Sprint): every line is added."""
    r = subprocess.run(["git", "ls-files", "--others", "--exclude-standard", "--", "components", "main"], cwd=ROOT,
                       capture_output=True, text=True, check=True)
    out = []
    for rel in r.stdout.split():
        if rel.endswith((".c", ".h")):
            lines = (ROOT / rel).read_text(errors="replace").split("\n")
            out.append(FileDiff(rel, lines, [], set(range(1, len(lines) + 1))))
    return out


def base_tags() -> set[str]:
    r = subprocess.run(["git", "grep", "-h", "-o", "-E", r'"\s*[A-Z][A-Z0-9_]+', BASE, "--", "components", "main"],
                       cwd=ROOT, capture_output=True, text=True)
    return {m.strip('" ').strip() for m in r.stdout.split()} | {x.strip('"') for x in r.stdout.splitlines()}


# =========================================================================== preprocessor

def _join_continuations(lines: list[str]) -> list[tuple[int, str]]:
    out, i = [], 0
    while i < len(lines):
        start, s = i, lines[i]
        while s.endswith("\\") and i + 1 < len(lines):
            i += 1
            s = s[:-1] + " " + lines[i]
        out.append((start, s))
        i += 1
    return out


def _eval(expr: str, macros: dict[str, str], depth: int = 0) -> int:
    expr = re.sub(r"/\*.*?\*/|//.*$", " ", expr)
    expr = re.sub(r"\bdefined\s*\(\s*(\w+)\s*\)|\bdefined\s+(\w+)",
                  lambda m: "1" if (m.group(1) or m.group(2)) in macros else "0", expr)
    out = []
    for t in re.findall(r"0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*|\w+|&&|\|\||==|!=|<=|>=|<<|>>|[()!<>+\-*/%&|^~]", expr):
        if t[0].isdigit():
            out.append(str(int(t.rstrip("uUlL"), 0)))
        elif re.match(r"\w", t):
            v = macros.get(t)
            out.append(str(_eval(v, macros, depth + 1)) if v and depth < 8 else "0")
        else:
            out.append({"&&": " and ", "||": " or ", "!": " not "}.get(t, t))
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")        # function-like macro residue, e.g. "0(1)"
            return int(bool(eval("".join(out) or "0", {"__builtins__": {}})))  # noqa: S307 - digits/operators only
    except Exception:
        return 0


def active_lines(lines: list[str], defs: dict[str, str]) -> set[int]:
    """1-based lines the preprocessor keeps under `defs` (object-like #defines tracked)."""
    macros = dict(defs)
    active: set[int] = set()
    stack: list[tuple[bool, bool]] = []          # (enclosing active, a branch already taken)
    on = True
    for start, s in _join_continuations(lines):
        m = re.match(r"\s*#\s*(\w+)\s*(.*)$", s)
        d, rest = (m.group(1), m.group(2)) if m else (None, "")
        if d in ("if", "ifdef", "ifndef"):
            if d == "if":
                c = _eval(rest, macros)
            else:
                name = rest.split()[0] if rest.split() else ""
                c = int((name in macros) == (d == "ifdef"))
            stack.append((on, bool(c)))
            on = on and bool(c)
        elif d == "elif" and stack:
            parent, taken = stack[-1]
            c = not taken and bool(_eval(rest, macros))
            stack[-1] = (parent, taken or c)
            on = parent and c
        elif d == "else" and stack:
            parent, taken = stack[-1]
            stack[-1] = (parent, True)
            on = parent and not taken
        elif d == "endif" and stack:
            on, _t = stack.pop()
            on = on
        elif on and d == "define":
            mm = re.match(r"(\w+)(?!\()\s*(.*)$", rest)
            if mm:
                macros[mm.group(1)] = mm.group(2).strip() or "1"
        elif on and d == "undef":
            macros.pop(rest.split()[0] if rest.split() else "", None)
        if on and d is None:
            n_cont = s.count("\n")
            active.add(start + 1 + n_cont)
            active.add(start + 1)
    return active


def hil_only_lines(path: str, lines: list[str]) -> set[int]:
    if any(r.search(path) for r in HIL_PATHS):
        return set(range(1, len(lines) + 1))
    hil = active_lines(lines, {"CONFIG_AMBYTE_EVQ_HIL": "1"})
    rel = active_lines(lines, {"CONFIG_AMBYTE_EVQ_HIL": "0"})
    return hil - rel


# =========================================================================== C scanning

def tokenize(lines: list[str]) -> list[tuple[str, str, int]]:
    """(kind, text, line) with kind in {id, str, punct}; comments and preprocessor lines skipped."""
    src = "\n".join(lines)
    toks: list[tuple[str, str, int]] = []
    i, n, line = 0, len(src), 1
    bol = True
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            bol = True
            continue
        if c in " \t\r\f\v":
            i += 1
            continue
        if bol and c == "#":                     # directive (with continuations)
            while i < n and src[i] != "\n":
                if src[i] == "\\" and i + 1 < n and src[i + 1] == "\n":
                    line += 1
                    i += 2
                    continue
                i += 1
            continue
        bol = False
        if src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += src.count("\n", i, j)
            i = j
            continue
        if c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            if c == '"':
                toks.append(("str", src[i + 1:j], line))
            i = j + 1
            continue
        m = re.match(r"[A-Za-z_]\w*|\d[\w.]*", src[i:i + 256])
        if m:
            toks.append(("id" if not m.group(0)[0].isdigit() else "num", m.group(0), line))
            i += len(m.group(0))
            continue
        toks.append(("punct", c, line))
        i += 1
    return toks


def _strip_attrs(stmt: list[tuple[str, str, int]]) -> list[tuple[str, str, int]]:
    out, i = [], 0
    while i < len(stmt):
        if stmt[i][1] in ("__attribute__", "__declspec", "_Alignas", "__asm__", "asm") and i + 1 < len(stmt) \
                and stmt[i + 1][1] == "(":
            depth, i = 0, i + 1
            while i < len(stmt):
                depth += {"(": 1, ")": -1}.get(stmt[i][1], 0)
                i += 1
                if depth == 0:
                    break
            continue
        out.append(stmt[i])
        i += 1
    return out


def _split_top(stmt, sep=","):
    parts, cur, depth = [], [], 0
    for t in stmt:
        if t[1] in "([{":
            depth += 1
        elif t[1] in ")]}":
            depth -= 1
        if depth == 0 and t[1] == sep and t[0] == "punct":
            parts.append(cur)
            cur = []
        else:
            cur.append(t)
    parts.append(cur)
    return parts


def _declarator_name(decl, first: bool):
    """Name declared by one declarator; None for a prototype."""
    depth, before_eq = 0, []
    for t in decl:
        if t[1] in "([{":
            depth += 1
        elif t[1] in ")]}":
            depth -= 1
        if depth == 0 and t[1] == "=":
            break
        before_eq.append(t)
    has_eq = len(before_eq) < len(decl)
    parens = [i for i, t in enumerate(before_eq) if t[1] == "("]
    if parens and not has_eq:
        p = parens[0]
        if p + 2 < len(before_eq) and before_eq[p + 1][1] == "*" and before_eq[p + 2][0] == "id":
            return before_eq[p + 2]                    # function-pointer object
        return None                                    # prototype / macro call
    if parens and has_eq:
        p = parens[0]
        if p + 2 < len(before_eq) and before_eq[p + 1][1] == "*" and before_eq[p + 2][0] == "id":
            return before_eq[p + 2]
    depth, cand = 0, None
    for t in before_eq:
        if t[1] in "([":
            depth += 1
        elif t[1] in ")]":
            depth -= 1
        elif depth == 0 and t[0] == "id" and t[1] not in C_KEYWORDS:
            cand = t
    return cand


def definitions(lines: list[str]) -> list[tuple[str, int, str]]:
    """(name, line, kind) of every file-scope function/object definition."""
    toks = tokenize(lines)
    out: list[tuple[str, int, str]] = []
    stmt: list = []
    i = 0

    def skip_block(j: int) -> int:
        depth = 0
        while j < len(toks):
            depth += {"{": 1, "}": -1}.get(toks[j][1], 0) if toks[j][0] == "punct" else 0
            j += 1
            if depth == 0:
                return j
        return j

    def decls(st, after_type=False):
        st = _strip_attrs(st)
        if not st or st[0][1] in ("typedef", "extern", "_Static_assert", "static_assert"):
            return
        if any(t[1] == "typedef" for t in st):
            return
        for k, d in enumerate(_split_top(st)):
            nm = _declarator_name(d, k == 0)
            if nm is not None:
                out.append((nm[1], nm[2], "object"))

    while i < len(toks):
        t = toks[i]
        if t[0] == "punct" and t[1] == ";":
            decls(stmt)
            stmt = []
            i += 1
            continue
        if t[0] == "punct" and t[1] == "{":
            st = _strip_attrs(stmt)
            if len(st) == 2 and st[0][1] == "extern" and st[1][0] == "str":
                stmt = []
                i += 1                                  # extern "C" { ... } is transparent
                continue
            depth0 = [x for x in st]
            has_eq = any(x[1] == "=" for x in _split_top(depth0, ";")[0])
            is_type = any(x[1] == "typedef" for x in st) or (len(st) >= 1 and (
                st[-1][1] in ("struct", "union", "enum") or (len(st) >= 2 and st[-2][1] in ("struct", "union", "enum"))))
            if has_eq:
                j = skip_block(i)                        # initializer; declarators continue to ';'
                while j < len(toks) and toks[j][1] != ";":
                    j += 1
                decls(stmt + [("punct", "{", t[2]), ("punct", "}", t[2])] + toks[skip_block(i):j])
                stmt = []
                i = j + 1
                continue
            if is_type:
                j = skip_block(i)
                k = j
                while k < len(toks) and toks[k][1] != ";":
                    k += 1
                if not any(x[1] == "typedef" for x in st):
                    decls([("id", "int", t[2])] + toks[j:k])     # `struct {..} s_obj;` objects
                stmt = []
                i = k + 1
                continue
            if any(x[1] == "(" for x in st):
                p = next(n for n, x in enumerate(st) if x[1] == "(")
                if p > 0 and st[p - 1][0] == "id" and st[p - 1][1] not in C_KEYWORDS | {"if", "for", "while",
                                                                                          "switch", "return"}:
                    out.append((st[p - 1][1], st[p - 1][2], "function"))
            stmt = []
            i = skip_block(i)
            continue
        stmt.append(t)
        i += 1
    return out


def string_literals(lines: list[str]) -> list[tuple[str, int]]:
    """First piece of every (possibly concatenated) string literal, with its line."""
    toks = tokenize(lines)
    out = []
    for k, t in enumerate(toks):
        if t[0] == "str" and not (k > 0 and toks[k - 1][0] == "str"):
            out.append((t[1], t[2]))
    return out


# =========================================================================== the check

def check(files: list[FileDiff], known_tags: set[str]) -> list[str]:
    bad: list[str] = []
    for f in files:
        if not f.path.endswith((".c", ".h")):
            continue
        hil = hil_only_lines(f.path, f.new)
        scope = f.added & hil
        if not scope:
            continue
        old_names = {n for n, _l, _k in definitions(f.old)}
        for name, ln, kind in definitions(f.new):
            if ln in scope and name not in old_names and not any(p in name for p in SYMBOL_PARTS):
                bad.append(f"{f.path}:{ln}: {kind} `{name}` defined in HIL-only code without a HIL symbol part "
                           f"({'/'.join(SYMBOL_PARTS)})")
        for lit, ln in string_literals(f.new):
            if ln not in scope:
                continue
            m = TAG_RE.match(lit)
            if not m or len(m.group(1)) < 2:
                continue
            tag = m.group(1)
            if tag.startswith(("SDL_", "SLT_")) or tag in LISTED or tag in GENERIC_LABELS or tag in known_tags:
                continue
            bad.append(f"{f.path}:{ln}: marker-like literal \"{lit[:40]}\" does not start with SDL_/SLT_ "
                       f"and is not a listed command/marker")
    return bad


# =========================================================================== tests

FIXTURE_NEW_FILE = """\
diff --git a/components/evq_hil/evq_hil_sdl_extra.c b/components/evq_hil/evq_hil_sdl_extra.c
new file mode 100644
--- /dev/null
+++ b/components/evq_hil/evq_hil_sdl_extra.c
@@ -0,0 +1,14 @@
+#include <stdio.h>
+static int s_hil_sdl_count;
+static const char *const k_hil_sdl_names[] = { "a", "b" };
+static void hil_sdl_drain(void)
+{
+    printf("SLT_DRAIN %u %u\\r\\n", 1u, 2u);
+    printf("POP %u\\n", 3u);
+}
+int evq_hil_sdl_status(void) { return s_hil_sdl_count; }
+static int helper(int x)
+{
+    return x + 1;
+}
+typedef struct { int a; } hil_local_t;
"""

FIXTURE_IF_BLOCK = """\
diff --git a/components/sd_logger/sd_logger.c b/components/sd_logger/sd_logger.c
--- a/components/sd_logger/sd_logger.c
+++ b/components/sd_logger/sd_logger.c
@@ -1,6 +1,20 @@
 #include <stdio.h>
+#if CONFIG_AMBYTE_EVQ_HIL || defined(EVQ_HIL_HOST)
+#define SDL_HOOKS 1
+#else
+#define SDL_HOOKS 0
+#endif
 static int s_prod;
+static int s_new_production_counter;
+#if SDL_HOOKS
+static void sdl_hil_note(int n)
+{
+    printf("SDL_FF %d\\n", n);
+    printf("\\r\\nHIL_BADMARK %d\\n", n);
+}
+static unsigned s_trace_count = 0, s_other;
+#endif
 void production(void)
 {
     s_prod++;
 }
"""


class IsoNamesParser(unittest.TestCase):
    def test_new_hil_file_unprefixed_helper_fails(self):
        bad = check(parse_diff(FIXTURE_NEW_FILE), set())
        self.assertEqual(len(bad), 1, bad)
        self.assertIn("`helper`", bad[0])

    def test_if_block_in_production_file(self):
        bad = check(parse_diff(FIXTURE_IF_BLOCK), set())
        names = sorted(re.findall(r"`(\w+)`", " ".join(bad)))
        self.assertEqual(names, ["s_other", "s_trace_count"], bad)      # production counter outside #if ignored
        self.assertTrue(any("HIL_BADMARK" in b for b in bad), bad)
        self.assertFalse(any("SDL_FF" in b for b in bad))
        self.assertEqual(len(bad), 3, bad)

    def test_prefixed_fixture_passes(self):
        fixed = FIXTURE_NEW_FILE.replace("helper", "hil_sdl_helper")
        self.assertEqual(check(parse_diff(fixed), set()), [])

    def test_definition_scanner(self):
        src = ["static int a = 1, b, c[3] = {1, 2, 3};", "int f(void);", "static void (*cb)(int);",
               "struct s { int x; } obj1;", "typedef int t_t;", "extern int e;",
               "void g(int x) { if (x) { } }", "_Static_assert(1, \"x\");", "RTC_NOINIT_ATTR static uint32_t r;",
               "static const k_t k[] = { { 1 }, { 2 } };"]
        got = [(n, k) for n, _l, k in definitions(src)]
        self.assertEqual(got, [("a", "object"), ("b", "object"), ("c", "object"), ("cb", "object"),
                               ("obj1", "object"), ("g", "function"), ("r", "object"), ("k", "object")])

    def test_preprocessor_follows_defines(self):
        lines = ["#if !defined(X) && CONFIG_AMBYTE_EVQ_HIL", "a", "#endif", "#if CONFIG_AMBYTE_EVQ_HIL",
                 "#define ON 1", "#else", "#define ON 0", "#endif", "#if ON", "b", "#else", "c", "#endif", "d"]
        self.assertEqual(hil_only_lines("components/x/y.c", lines), {2, 10})


class IsoNamesRepo(unittest.TestCase):
    def test_sprint2_diff_names(self):
        try:
            text = git_diff()
        except (OSError, subprocess.CalledProcessError) as e:
            self.skipTest(f"git unavailable: {e}")
        bad = check(parse_diff(text) + untracked_sources(), base_tags())
        self.assertEqual(bad, [], "\n" + "\n".join(bad))


if __name__ == "__main__":
    unittest.main()
