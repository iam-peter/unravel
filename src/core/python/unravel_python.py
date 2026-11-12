#!/usr/bin/env python3
"""Python sources as unravel's code model: modules, classes, functions,
methods, attributes and variables, with the calls, uses, imports,
inheritance and overrides between them, and measurements of every body.

Only the standard library's ast and tokenize read the code; nothing of it
is imported or run. Names are resolved statically: definitions and
imports per module, `self` and `cls` in methods, locals assigned from a
class, annotations, super(), and for an attribute of something of unknown
type, the one project class that defines it (marked dynamic).

    python3 -I unravel_python.py --root DIR --out FILE.json  < file list

Writes JSON: {"files": [...], "symbols": [...], "refs": [...],
"diagnostics": [...]}, with the numbers of unravel's model (src/core/model.h).
"""

import argparse
import ast
import io
import json
import os
import sys
import tokenize

# unravel's SymbolKind, RefKind, flags and access (src/core/model.h)
NAMESPACE, CLASS, FUNCTION, METHOD, CONSTRUCTOR, OPERATOR, FIELD, VARIABLE = 1, 2, 8, 9, 10, 12, 13, 14
CALL, TYPE_USE, VAR_USE, INHERIT, OVERRIDE, DECLARATION = 0, 1, 2, 3, 4, 7
DEFINED, VIRTUAL, PURE_VIRTUAL, STATIC, CONST, ABSTRACT, ENTRY_POINT, META_CALLABLE = (
    1, 4, 8, 16, 32, 2048, 8192, 1 << 18)
READ, WRITE, IMPLICIT, ADDRESS_OF, DYNAMIC = 1, 2, 4, 16, 32
PUBLIC, PROTECTED, PRIVATE = 1, 2, 3
WARNING, ERROR = 1, 2

# Decorators that don't hand a function to someone who calls it
PLAIN_DECORATORS = {"staticmethod", "classmethod", "property", "abstractmethod", "abc.abstractmethod",
                    "functools.wraps", "wraps", "functools.lru_cache", "lru_cache", "functools.cache", "cache",
                    "cached_property", "functools.cached_property", "override", "typing.override",
                    "overload", "typing.overload", "dataclass", "dataclasses.dataclass", "contextmanager",
                    "contextlib.contextmanager", "setter", "getter", "deleter", "final", "typing.final"}


def u32(x):
    return x & 0xFFFFFFFF


def fnv32(text):
    h = 2166136261
    for b in text.encode("utf-8"):
        h = u32((h ^ b) * 16777619)
    return h


def mix32(x):
    x ^= x >> 16
    x = u32(x * 0x7feb352d)
    x ^= x >> 15
    x = u32(x * 0x846ca68b)
    x ^= x >> 16
    return x


def dotted(node):
    """a.b.c for Name/Attribute chains, else None"""
    parts = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
        return ".".join(reversed(parts))
    return None


class Symbol:
    __slots__ = ("id", "usr", "name", "kind", "parent", "file", "line", "col", "end_line", "end_col",
                 "def_line", "flags", "access", "signature", "type", "metrics", "members", "bases", "module",
                 "node", "type_symbol")

    def __init__(self, sid, usr, name, kind, parent, file):
        self.id = sid
        self.usr = usr
        self.name = name
        self.kind = kind
        self.parent = parent
        self.file = file
        self.line = self.col = self.end_line = self.end_col = 0
        self.def_line = 0
        self.flags = 0
        self.access = 0
        self.signature = ""
        self.type = ""
        self.metrics = None
        self.members = {}     # name -> Symbol, for modules and classes
        self.bases = []       # Symbols, for classes
        self.module = None
        self.node = None
        self.type_symbol = None   # for a field or variable: the project class its values are


class Module:
    def __init__(self, path, rel, name, text, tree, file_index):
        self.path = path
        self.rel = rel
        self.name = name
        self.text = text
        self.lines = text.split("\n")
        self.tree = tree
        self.file = file_index
        self.symbol = None
        self.imports = {}     # local name -> ("module", dotted) or ("symbol", dotted)
        self.includes = []    # (file index, line)


class Index:
    def __init__(self, root):
        self.root = root
        self.files = []
        self.symbols = []
        self.refs = []
        self.diagnostics = []
        self.modules = {}       # dotted name -> Module
        self.by_usr = {}
        self.members_by_name = {}  # member name -> [class member Symbols], for the dynamic fallback

    # --- Files and modules -------------------------------------------------

    def module_name(self, path):
        """Dotted name from the top of its package, for absolute imports"""
        directory, base = os.path.split(path)
        parts = [] if base == "__init__.py" else [os.path.splitext(base)[0]]
        while os.path.exists(os.path.join(directory, "__init__.py")):
            directory, package = os.path.split(directory)
            parts.insert(0, package)
            if not directory or directory == "/":
                break
        return ".".join(parts) if parts else os.path.splitext(base)[0]

    def load(self, path):
        rel = os.path.relpath(path, self.root)
        index = len(self.files)
        self.files.append({"path": path, "includes": []})
        try:
            with open(path, "rb") as f:
                raw = f.read()
            text = raw.decode("utf-8", errors="replace")
            tree = ast.parse(raw, filename=path)
        except SyntaxError as e:
            self.diagnostics.append({"severity": ERROR, "message": "Python: " + (e.msg or "syntax error"),
                                     "file": index, "line": e.lineno or 1, "col": (e.offset or 1)})
            return
        except (OSError, ValueError) as e:
            self.diagnostics.append({"severity": ERROR, "message": "Python: " + str(e), "file": index, "line": 1, "col": 1})
            return
        name = self.module_name(path)
        if name in self.modules:   # the same dotted name twice (scripts in several folders)
            name = rel.replace(os.sep, ".")[:-3]
        module = Module(path, rel, name, text, tree, index)
        self.modules[name] = module

    # --- Symbols ---------------------------------------------------------------

    def add(self, usr, name, kind, parent, module, node=None, at=None):
        if usr in self.by_usr:
            return self.by_usr[usr]
        s = Symbol(len(self.symbols), usr, name, kind, parent.id if parent else -1, module.file)
        s.module = module
        s.node = node
        s.flags = DEFINED
        if at:
            s.line, s.col = at
        if node is not None and hasattr(node, "lineno"):
            s.def_line = node.lineno
            s.end_line = getattr(node, "end_lineno", node.lineno) or node.lineno
            s.end_col = (getattr(node, "end_col_offset", 0) or 0) + 1
            if not at:
                s.line, s.col = node.lineno, node.col_offset + 1
        self.symbols.append(s)
        self.by_usr[usr] = s
        if parent is not None:
            parent.members.setdefault(name, s)
        return s

    def name_position(self, module, node, keyword):
        """Where the name of a def or class is written: after the keyword"""
        line = module.lines[node.lineno - 1] if node.lineno - 1 < len(module.lines) else ""
        raw = line.encode("utf-8")
        start = raw.find(keyword.encode() + b" ", node.col_offset)
        if start >= 0:
            at = raw.find(node.name.encode(), start)
            if at >= 0:
                return node.lineno, at + 1
        return node.lineno, node.col_offset + 1

    def access_of(self, name):
        if name.startswith("__") and name.endswith("__"):
            return PUBLIC
        if name.startswith("__"):
            return PRIVATE
        if name.startswith("_"):
            return PROTECTED
        return PUBLIC

    def declare_module(self, module):
        usr = "py:" + module.name
        m = self.add(usr, module.name, NAMESPACE, None, module)
        m.line, m.col = 1, 1
        m.def_line, m.end_line = 1, max(1, len(module.lines))
        m.flags = DEFINED
        module.symbol = m
        for node in module.tree.body:
            self.declare_statement(module, m, node, top=True)

    def decorator_names(self, node):
        out = []
        for d in getattr(node, "decorator_list", []):
            target = d.func if isinstance(d, ast.Call) else d
            name = dotted(target)
            if name:
                out.append(name)
        return out

    def declare_function(self, module, parent, node):
        in_class = parent.kind == CLASS
        name = node.name
        if in_class:
            kind = CONSTRUCTOR if name == "__init__" else OPERATOR if (name.startswith("__") and name.endswith("__")) else METHOD
        else:
            kind = FUNCTION
        s = self.add(parent.usr + "." + name, name, kind, parent, module, node, self.name_position(module, node, "def"))
        decorators = self.decorator_names(node)
        args = node.args
        params = [a.arg for a in args.posonlyargs + args.args + args.kwonlyargs]
        if args.vararg:
            params.append("*" + args.vararg.arg)
        if args.kwarg:
            params.append("**" + args.kwarg.arg)
        if in_class:
            if "staticmethod" in decorators:
                s.flags |= STATIC
            elif params:
                params = params[1:]   # self, cls
            if any(d.endswith("abstractmethod") for d in decorators):
                s.flags |= PURE_VIRTUAL | VIRTUAL
            s.access = self.access_of(name)
        # Registered by a decorator (a route, a command, a fixture): called from outside
        if any(d not in PLAIN_DECORATORS and not d.endswith((".setter", ".getter", ".deleter")) for d in decorators):
            s.flags |= META_CALLABLE
        returns = ast.unparse(node.returns) if getattr(node, "returns", None) is not None else ""
        prefix = "async def " if isinstance(node, ast.AsyncFunctionDef) else "def "
        s.signature = prefix + name + "(" + ", ".join(params) + ")" + (" -> " + returns if returns else "")
        s.metrics = self.measure(module, node, len([p for p in params]))
        if in_class:
            self.members_by_name.setdefault(name, []).append(s)
        return s

    def declare_statement(self, module, parent, node, top=False):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            s = self.declare_function(module, parent, node)
            if parent.kind == CLASS:
                # self.x = … in a method declares a field of the class
                for sub in ast.walk(node):
                    targets = []
                    if isinstance(sub, ast.Assign):
                        targets = sub.targets
                    elif isinstance(sub, (ast.AnnAssign, ast.AugAssign)):
                        targets = [sub.target]
                    for t in targets:
                        for el in (t.elts if isinstance(t, (ast.Tuple, ast.List)) else [t]):
                            if (isinstance(el, ast.Attribute) and isinstance(el.value, ast.Name)
                                    and el.value.id == "self" and el.attr not in parent.members):
                                f = self.add(parent.usr + "." + el.attr, el.attr, FIELD, parent, module, None,
                                             (el.lineno, el.end_col_offset - len(el.attr) + 1))
                                f.def_line = f.end_line = el.lineno
                                f.access = self.access_of(el.attr)
                                self.members_by_name.setdefault(el.attr, []).append(f)
            return
        if isinstance(node, ast.ClassDef):
            c = self.add(parent.usr + "." + node.name, node.name, CLASS, parent, module, node,
                         self.name_position(module, node, "class"))
            c.signature = "class " + node.name + ("(" + ", ".join(ast.unparse(b) for b in node.bases) + ")" if node.bases else "")
            c.access = self.access_of(node.name) if parent.kind == CLASS else 0
            decorators = self.decorator_names(node)
            if any(d not in PLAIN_DECORATORS for d in decorators):
                c.flags |= META_CALLABLE
            c.metrics = {"lines": c.end_line - c.def_line + 1}
            for sub in node.body:
                self.declare_statement(module, c, sub)
            return
        if isinstance(node, (ast.Assign, ast.AnnAssign)) and parent.kind in (NAMESPACE, CLASS):
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            for t in targets:
                for el in (t.elts if isinstance(t, (ast.Tuple, ast.List)) else [t]):
                    if isinstance(el, ast.Name) and el.id not in parent.members:
                        kind = VARIABLE if parent.kind == NAMESPACE else FIELD
                        v = self.add(parent.usr + "." + el.id, el.id, kind, parent, module, None, (el.lineno, el.col_offset + 1))
                        v.def_line = v.end_line = el.lineno
                        if el.id.isupper():
                            v.flags |= CONST
                        if parent.kind == CLASS:
                            v.flags |= STATIC
                            v.access = self.access_of(el.id)
                            self.members_by_name.setdefault(el.id, []).append(v)
                        if isinstance(node, ast.AnnAssign) and node.annotation is not None:
                            v.type = ast.unparse(node.annotation)
            return
        # Definitions inside if/try at module level (platform checks, optional imports)
        if top and isinstance(node, (ast.If, ast.Try)):
            for part in (node.body, node.orelse, getattr(node, "finalbody", []),
                         [h for handler in getattr(node, "handlers", []) for h in handler.body]):
                for sub in part:
                    self.declare_statement(module, parent, sub, top=True)

    # --- Imports and bases -----------------------------------------------------

    def resolve_module(self, module, name, level):
        """The dotted name of an import's module, relative ones made absolute"""
        if level:
            package = module.name.split(".")
            if not module.path.endswith("__init__.py"):
                package = package[:-1]
            package = package[:len(package) - (level - 1)] if level > 1 else package
            return ".".join(package + ([name] if name else []))
        return name

    def read_imports(self, module):
        for node in ast.walk(module.tree):
            if isinstance(node, ast.Import):
                for alias in node.names:
                    target = alias.name
                    local = alias.asname or alias.name.split(".")[0]
                    module.imports[local] = ("module", target if alias.asname else alias.name.split(".")[0])
                    self.include(module, target, node.lineno)
            elif isinstance(node, ast.ImportFrom):
                base = self.resolve_module(module, node.module or "", node.level)
                for alias in node.names:
                    if alias.name == "*":
                        imported = self.modules.get(base)
                        if imported:
                            for name in imported.symbol.members:
                                if not name.startswith("_"):
                                    module.imports.setdefault(name, ("symbol", base + "." + name))
                        continue
                    full = base + "." + alias.name if base else alias.name
                    module.imports[alias.asname or alias.name] = ("module", full) if full in self.modules else ("symbol", full)
                    if full in self.modules:
                        self.include(module, full, node.lineno)
                self.include(module, base, node.lineno)

    def include(self, module, target, line):
        imported = self.modules.get(target)
        if imported is None and "." in target:
            imported = self.modules.get(target.rsplit(".", 1)[0])
        if imported is not None and imported is not module:
            module.includes.append((imported.file, line))

    def lookup(self, full):
        """A symbol by its dotted name: a module, or something in one"""
        if full in self.modules:
            return self.modules[full].symbol
        if "." in full:
            head, tail = full.rsplit(".", 1)
            owner = self.lookup(head)
            if owner is not None:
                return self.member(owner, tail)
        return None

    def member(self, owner, name):
        """A member of a module or a class, through the class's bases"""
        if owner.kind == NAMESPACE:
            found = owner.members.get(name)
            if found is not None:
                return found
            # An imported name re-exported by the module
            module = owner.module
            if module is not None and name in module.imports:
                how, full = module.imports[name]
                return self.lookup(full) if how == "symbol" else (self.modules[full].symbol if full in self.modules else None)
            return None
        for c in self.mro(owner):
            found = c.members.get(name)
            if found is not None:
                return found
        return None

    def mro(self, cls):
        out, todo, seen = [], [cls], set()
        while todo:
            c = todo.pop(0)
            if c.id in seen:
                continue
            seen.add(c.id)
            out.append(c)
            todo.extend(c.bases)
        return out

    def module_level(self, module, name):
        """What a name means at module level: a definition or an import"""
        found = module.symbol.members.get(name)
        if found is not None:
            return found
        if name in module.imports:
            how, full = module.imports[name]
            if how == "module":
                return self.modules[full].symbol if full in self.modules else None
            return self.lookup(full)
        return None

    def read_bases(self, module):
        for s in list(self.symbols):
            if s.module is not module or s.kind != CLASS or s.node is None:
                continue
            for b in s.node.bases:
                name = dotted(b)
                if not name:
                    continue
                head, _, rest = name.partition(".")
                target = self.module_level(module, head)
                for part in rest.split(".") if rest else []:
                    target = self.member(target, part) if target is not None else None
                if target is not None and target.kind == CLASS:
                    s.bases.append(target)
                    self.ref(s, target, INHERIT, module, b.lineno, b.col_offset + 1, b.end_col_offset + 1)
            if any(m.flags & PURE_VIRTUAL for m in s.members.values()) or any(dotted(b) in ("ABC", "abc.ABC") for b in s.node.bases):
                s.flags |= ABSTRACT

    def read_overrides(self):
        for s in self.symbols:
            if s.kind != CLASS:
                continue
            for name, m in s.members.items():
                if m.kind not in (METHOD, OPERATOR, CONSTRUCTOR):
                    continue
                for base in self.mro(s)[1:]:
                    other = base.members.get(name)
                    if other is not None and other.kind in (METHOD, OPERATOR, CONSTRUCTOR):
                        self.ref(m, other, OVERRIDE, m.module, m.line, m.col, m.col + len(name))
                        m.flags |= VIRTUAL
                        other.flags |= VIRTUAL
                        break

    # --- References ------------------------------------------------------------

    def ref(self, source, target, kind, module, line, col, end_col, flags=0):
        self.refs.append({"from": source.id if source is not None else -1, "to": target.id, "kind": kind,
                          "flags": flags, "file": module.file, "line": line, "col": col,
                          "endCol": end_col})

    def read_references(self, module):
        Walker(self, module).run()

    # --- Measuring -------------------------------------------------------------

    def measure(self, module, node, params):
        m = {"lines": (getattr(node, "end_lineno", node.lineno) or node.lineno) - node.lineno + 1,
             "params": params, "complexity": 1, "cognitive": 0, "nesting": 0, "statements": 0, "returns": 0}

        def walk(n, depth, parent_if=False):
            for child in ast.iter_child_nodes(n):
                if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.Lambda)):
                    walk(child, depth + 1)   # nested definitions count as nesting, not branches
                    continue
                if isinstance(child, ast.stmt):
                    m["statements"] += 1
                nests = False
                if isinstance(child, ast.If):
                    m["complexity"] += 1
                    is_elif = parent_if and len(n.orelse) == 1 and n.orelse[0] is child
                    m["cognitive"] += 1 if is_elif else 1 + depth
                    if is_elif:
                        # An elif: its branches as deep as the if's
                        walk(child, depth, True)
                        continue
                    nests = True
                elif isinstance(child, (ast.For, ast.AsyncFor, ast.While)):
                    m["complexity"] += 1
                    m["cognitive"] += 1 + depth
                    nests = True
                elif isinstance(child, ast.ExceptHandler):
                    m["complexity"] += 1
                    m["cognitive"] += 1 + depth
                    nests = True
                elif isinstance(child, (ast.Try, ast.With, ast.AsyncWith)) or type(child).__name__ in ("TryStar", "Match"):
                    nests = True
                    if type(child).__name__ == "Match":
                        m["cognitive"] += 1 + depth
                elif type(child).__name__ == "match_case":
                    m["complexity"] += 1
                elif isinstance(child, ast.IfExp):
                    m["complexity"] += 1
                    m["cognitive"] += 1 + depth
                elif isinstance(child, ast.BoolOp):
                    m["complexity"] += len(child.values) - 1
                    m["cognitive"] += 1
                elif isinstance(child, ast.comprehension):
                    m["complexity"] += len(child.ifs)
                elif isinstance(child, ast.Return):
                    m["returns"] += 1
                if nests:
                    m["nesting"] = max(m["nesting"], depth + 1)
                    walk(child, depth + 1, isinstance(child, ast.If))
                else:
                    walk(child, depth, False)

        walk(node, 0)
        self.signature_of_body(module, node, m)
        return m

    def signature_of_body(self, module, node, m):
        """Tokens of the body, names and literals made alike, hashed: the same
        hash is the same code with other names"""
        if not node.body:
            return
        first = node.body[0].lineno
        last = getattr(node, "end_lineno", None) or first
        source = "\n".join(module.lines[first - 1:last]) + "\n"
        norm = []
        try:
            for tok in tokenize.generate_tokens(io.StringIO(source).readline):
                t = tok.type
                if t in (tokenize.COMMENT, tokenize.NL, tokenize.ENDMARKER, tokenize.ENCODING):
                    continue
                if t == tokenize.NAME:
                    norm.append(fnv32(tok.string) | 0x80000000 if tok.string in KEYWORDS else 1)
                elif t in (tokenize.NUMBER, tokenize.STRING):
                    norm.append(2)
                elif t == tokenize.INDENT:
                    norm.append(3)
                elif t == tokenize.DEDENT:
                    norm.append(4)
                elif t == tokenize.NEWLINE:
                    norm.append(5)
                else:
                    norm.append(fnv32(tok.string) | 0x80000000)
        except (tokenize.TokenError, IndentationError, SyntaxError):
            return
        m["tokens"] = len(norm)
        h = 1469598103934665603
        for v in norm:
            h = ((h ^ v) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        m["cloneHash"] = str(h)   # 64 bits: as a string, JSON readers lose them in doubles
        if len(norm) < 40:
            return
        mins = [0xFFFFFFFF] * 32
        for i in range(len(norm) - 5):
            sh = 2166136261
            for j in range(6):
                sh = u32((sh ^ norm[i + j]) * 16777619)
            for k in range(32):
                v = mix32(sh ^ u32(0x9e3779b9 * (k + 1)))
                if v < mins[k]:
                    mins[k] = v
        m["minHash"] = mins

    # --- Output ------------------------------------------------------------------

    def run(self, paths):
        for p in paths:
            self.load(p)
        modules = list(self.modules.values())
        for module in modules:
            self.declare_module(module)
        for module in modules:
            self.read_imports(module)
            for target, line in module.includes:
                self.files[module.file]["includes"].append({"file": target, "line": line})
        for module in modules:
            self.read_bases(module)
        self.read_overrides()
        for module in modules:
            self.read_references(module)
        # The scripts' entry points
        for module in modules:
            for node in module.tree.body:
                if (isinstance(node, ast.If) and isinstance(node.test, ast.Compare)
                        and dotted(node.test.left) == "__name__"):
                    module.symbol.flags |= ENTRY_POINT
            main = module.symbol.members.get("main")
            if main is not None and main.kind == FUNCTION and module.symbol.flags & ENTRY_POINT:
                main.flags |= ENTRY_POINT

    def output(self):
        symbols = []
        for s in self.symbols:
            e = {"usr": s.usr, "name": s.name, "kind": s.kind, "parent": s.parent, "file": s.file,
                 "line": s.line, "col": s.col, "defLine": s.def_line or s.line, "endLine": s.end_line or s.line,
                 "endCol": s.end_col, "flags": s.flags, "access": s.access, "signature": s.signature,
                 "type": s.type}
            if s.metrics:
                e["metrics"] = s.metrics
            symbols.append(e)
        return {"files": self.files, "symbols": symbols, "refs": self.refs, "diagnostics": self.diagnostics}


KEYWORDS = set(__import__("keyword").kwlist) | {"self", "cls", "True", "False", "None"}
BUILTINS = set(dir(__import__("builtins")))


class Walker(ast.NodeVisitor):
    """References in one module, with what each name means where it is used"""

    def __init__(self, index, module):
        self.index = index
        self.module = module
        self.owner = None          # the symbol the code is in (None at module level)
        self.cls = None            # the class around a method
        self.locals = [{}]         # per function: name -> class Symbol (its type) or None
        self.globals_declared = [set()]

    def run(self):
        self.visit(self.module.tree)

    # Scopes

    def visit_ClassDef(self, node):
        for d in node.decorator_list:
            self.visit(d)
        parent = self.owner
        c = self.index.by_usr.get(self.scope_usr() + "." + node.name)
        if c is None:
            return
        outer = self.cls
        self.owner, self.cls = c, c
        for b in node.bases + [k.value for k in node.keywords]:
            self.visit(b)
        for stmt in node.body:
            self.visit(stmt)
        self.owner, self.cls = parent, outer

    def scope_usr(self):
        return self.owner.usr if self.owner is not None else self.module.symbol.usr

    def visit_FunctionDef(self, node):
        for d in node.decorator_list:
            self.visit(d)
        f = self.index.by_usr.get(self.scope_usr() + "." + node.name) if (self.owner is None or self.owner.kind == CLASS) else None
        args = node.args
        # Annotations and defaults are read where the def is
        for a in args.posonlyargs + args.args + args.kwonlyargs + [x for x in (args.vararg, args.kwarg) if x]:
            if a.annotation is not None:
                self.visit_annotation(a.annotation)
        for d in args.defaults + [d for d in args.kw_defaults if d is not None]:
            self.visit(d)
        if node.returns is not None:
            self.visit_annotation(node.returns)
        outer_owner = self.owner
        if f is not None:
            self.owner = f
        scope = {}
        params = args.posonlyargs + args.args
        is_method = f is not None and f.kind in (METHOD, CONSTRUCTOR, OPERATOR) and not f.flags & STATIC
        for i, a in enumerate(params + args.kwonlyargs + [x for x in (args.vararg, args.kwarg) if x]):
            scope[a.arg] = self.annotation_class(a.annotation) if a.annotation is not None else None
        if is_method and params and self.cls is not None:
            classmethod_ = any(dotted(d) == "classmethod" for d in node.decorator_list)
            scope[params[0].arg] = ("class" if classmethod_ else "instance", self.cls)
        self.locals.append(scope)
        self.globals_declared.append(set())
        for stmt in node.body:
            self.collect_locals(stmt, scope)
        for stmt in node.body:
            self.visit(stmt)
        self.locals.pop()
        self.globals_declared.pop()
        self.owner = outer_owner

    visit_AsyncFunctionDef = visit_FunctionDef

    def visit_Lambda(self, node):
        scope = {a.arg: None for a in node.args.args}
        self.locals.append(scope)
        self.visit(node.body)
        self.locals.pop()

    def collect_locals(self, stmt, scope):
        """Names a function assigns are its own, unless declared global"""
        for n in ast.walk(stmt):
            if isinstance(n, (ast.Global, ast.Nonlocal)):
                self.globals_declared[-1].update(n.names)
            elif isinstance(n, ast.Name) and isinstance(n.ctx, ast.Store):
                scope.setdefault(n.id, None)
            elif isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                scope.setdefault(n.name, None)
            elif isinstance(n, ast.alias):
                scope.setdefault((n.asname or n.name).split(".")[0], None)
            elif isinstance(n, ast.ExceptHandler) and n.name:
                scope.setdefault(n.name, None)
        # A local assigned from a project class has its type
        for n in ast.walk(stmt):
            if isinstance(n, ast.Assign) and len(n.targets) == 1 and isinstance(n.targets[0], ast.Name):
                if isinstance(n.value, ast.Call):
                    target = self.resolve_expr(n.value.func)
                    if target is not None and target.kind == CLASS:
                        scope[n.targets[0].id] = ("instance", target)
            elif isinstance(n, ast.AnnAssign) and isinstance(n.target, ast.Name):
                scope[n.target.id] = self.annotation_class(n.annotation)

    def annotation_class(self, annotation):
        name = dotted(annotation)
        if isinstance(annotation, ast.Constant) and isinstance(annotation.value, str):
            name = annotation.value
        if isinstance(annotation, ast.Subscript):   # Optional[Foo], list[Foo]
            return self.annotation_class(annotation.slice)
        if not name:
            return None
        target = self.resolve_dotted(name)
        return ("instance", target) if target is not None and target.kind == CLASS else None

    # What a name or expression means

    def local(self, name):
        if name in self.globals_declared[-1]:
            return None, False
        for scope in reversed(self.locals[1:]):
            if name in scope:
                return scope[name], True
        return None, False

    def resolve_name(self, name):
        """A symbol, ("instance"/"class", class), or None"""
        value, is_local = self.local(name)
        if is_local:
            return value
        if self.cls is not None and self.owner is self.cls:
            found = self.cls.members.get(name)   # class body: earlier class attributes
            if found is not None:
                return found
        found = self.index.module_level(self.module, name)
        return found

    def resolve_dotted(self, name):
        head, _, rest = name.partition(".")
        target = self.resolve_name(head)
        for part in rest.split(".") if rest else []:
            target = self.attribute(target, part)[0] if target is not None else None
        return target if isinstance(target, Symbol) else None

    def attribute(self, base, name):
        """(symbol, dynamic) for base.name"""
        if isinstance(base, tuple):
            _, cls = base
            return self.index.member(cls, name), False
        if isinstance(base, Symbol):
            if base.kind in (NAMESPACE, CLASS):
                return self.index.member(base, name), False
            return None, False
        return None, False

    def resolve_expr(self, node):
        if isinstance(node, ast.Name):
            r = self.resolve_name(node.id)
            return r if isinstance(r, Symbol) else None
        if isinstance(node, ast.Attribute):
            base = self.value_of(node.value)
            target, _ = self.attribute(base, node.attr)
            return target
        return None

    def value_of(self, node):
        """What an expression is: a symbol, an instance or class of one, or None"""
        if isinstance(node, ast.Name):
            r = self.resolve_name(node.id)
            if isinstance(r, Symbol) and r.kind == VARIABLE:
                return ("instance", r.type_symbol) if r.type_symbol is not None else None
            return r
        if isinstance(node, ast.Attribute):
            base = self.value_of(node.value)
            target, _ = self.attribute(base, node.attr)
            if target is not None and target.kind in (FIELD, VARIABLE):
                if target.type_symbol is not None:
                    return ("instance", target.type_symbol)
                if target.type:
                    cls = self.resolve_dotted(target.type)
                    if cls is not None and cls.kind == CLASS:
                        return ("instance", cls)
                return None   # a value of unknown type
            return target
        if isinstance(node, ast.Call):
            if isinstance(node.func, ast.Name) and node.func.id == "super" and self.cls is not None:
                return ("super", self.cls)
            callee = self.resolve_expr(node.func)
            if callee is not None and callee.kind == CLASS:
                return ("instance", callee)
        return None

    # References

    def emit(self, target, kind, node, flags=0, name=None):
        name = name or target.name
        line = node.lineno
        if isinstance(node, ast.Attribute):
            col = node.end_col_offset - len(node.attr) + 1
        else:
            col = node.col_offset + 1
        self.index.ref(self.owner if self.owner is not None and self.owner.kind != NAMESPACE else None,
                       target, kind, self.module, line, col, col + len(name.encode("utf-8")), flags)

    def kind_for(self, target, called, store):
        if target.kind in (FUNCTION, METHOD, CONSTRUCTOR, OPERATOR):
            return CALL, (0 if called else ADDRESS_OF)
        if target.kind == CLASS:
            return TYPE_USE, 0
        if target.kind in (FIELD, VARIABLE):
            return VAR_USE, (WRITE if store else READ)
        return None, 0

    def use(self, target, node, called=False, store=False, dynamic=False):
        if target is None or target.kind == NAMESPACE:
            return
        kind, flags = self.kind_for(target, called, store)
        if kind is None:
            return
        if dynamic:
            flags |= DYNAMIC
        self.emit(target, kind, node, flags)
        if called and target.kind == CLASS:
            # Making an instance calls its __init__
            init = self.index.member(target, "__init__")
            if init is not None:
                self.emit(init, CALL, node, IMPLICIT, target.name)

    def visit_Name(self, node):
        if node.id in BUILTINS and self.index.module_level(self.module, node.id) is None:
            return
        target = self.resolve_name(node.id)
        if isinstance(target, Symbol):
            self.use(target, node, store=isinstance(node.ctx, ast.Store))

    def visit_Attribute(self, node, called=False):
        base = self.value_of(node.value)
        target, dynamic = None, False
        if isinstance(base, tuple) and base[0] == "super":
            for b in self.index.mro(base[1])[1:]:
                target = b.members.get(node.attr)
                if target is not None:
                    break
        else:
            target, dynamic = self.attribute(base, node.attr)
        if target is None and base is None and not (node.attr.startswith("__") and node.attr.endswith("__")):
            # Something of unknown type: the one project class that has it
            candidates = self.index.members_by_name.get(node.attr, [])
            if len(candidates) == 1:
                target, dynamic = candidates[0], True
        if target is not None:
            self.use(target, node, called=called, store=isinstance(node.ctx, ast.Store), dynamic=dynamic)
        self.visit(node.value)

    def visit_Call(self, node):
        if isinstance(node.func, ast.Attribute):
            self.visit_Attribute(node.func, called=True)
        elif isinstance(node.func, ast.Name):
            if not (node.func.id in BUILTINS and self.index.module_level(self.module, node.func.id) is None):
                target = self.resolve_name(node.func.id)
                if isinstance(target, Symbol):
                    self.use(target, node.func, called=True)
        else:
            self.visit(node.func)
        for a in node.args:
            self.visit(a)
        for k in node.keywords:
            self.visit(k.value)

    def visit_annotation(self, node):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            target = self.resolve_dotted(node.value)
            if target is not None:
                self.use(target, node)
            return
        self.visit(node)

    def visit_Assign(self, node):
        # self.x = <a Foo>: the field holds Foos, for later self.x.method()
        kind = self.value_of(node.value)
        if isinstance(kind, tuple) and kind[0] == "instance":
            for t in node.targets:
                if isinstance(t, ast.Attribute):
                    field, _ = self.attribute(self.value_of(t.value), t.attr)
                    if field is not None and field.kind in (FIELD, VARIABLE) and field.type_symbol is None:
                        field.type_symbol = kind[1]
                elif isinstance(t, ast.Name) and self.owner is None:
                    var = self.index.module_level(self.module, t.id)
                    if var is not None and var.kind == VARIABLE and var.type_symbol is None:
                        var.type_symbol = kind[1]
        self.generic_visit(node)

    def visit_AnnAssign(self, node):
        self.visit_annotation(node.annotation)
        self.visit(node.target)
        if node.value is not None:
            self.visit(node.value)

    def visit_AugAssign(self, node):
        # Both read and written
        if isinstance(node.target, (ast.Name, ast.Attribute)):
            target = self.resolve_expr(node.target) if isinstance(node.target, ast.Name) else None
            if isinstance(node.target, ast.Attribute):
                base = self.value_of(node.target.value)
                target, _ = self.attribute(base, node.target.attr)
            if target is not None and target.kind in (FIELD, VARIABLE):
                self.emit(target, VAR_USE, node.target, READ | WRITE)
                self.visit(node.value)
                return
        self.generic_visit(node)

    def visit_Import(self, node):
        pass

    def visit_ImportFrom(self, node):
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    paths = [line.strip() for line in sys.stdin if line.strip()]
    sys.setrecursionlimit(10000)
    index = Index(os.path.abspath(args.root))
    index.run(paths)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(index.output(), f)


if __name__ == "__main__":
    main()
