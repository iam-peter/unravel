# Unravel

See the architecture, flows and connections of C, C++ and Python code: what a
class is made of and what it touches, how control flows from `main`,
which modules lean on which, where the cycles, the duplicates and the
complexity are. Built for reviewing and understanding code you didn't
write yourself — especially code an assistant wrote.

![The graph of a class: who uses it on the left, what it uses on the right, its code beside it](docs/screenshots/graph.png)

clang does the understanding. Unravel parses every translation unit with
libclang, exactly as your build compiles it (from `compile_commands.json`),
and keeps every declaration, reference, call, include, inheritance and
override it reports. On top of that it measures function bodies, finds
duplicated code, cycles and dead code, and shows it all in views taken
from the tools that did this best: [Sourcetrail](https://github.com/CoatiSoftware/Sourcetrail)'s
graph and code views, and the hierarchical edge bundles, treemaps,
table lenses and dependency matrices of
[SolidSX and CPPVis](https://webspace.science.uu.nl/~telea001/SoftVis/CPPVis).

Python goes through the same model and the same views. Python's own `ast`
module reads every `.py` file under the project (nothing is imported or
run): modules become namespaces, with their classes, functions, methods,
fields and module variables, the imports between them, inheritance and
overrides, and the calls it can resolve — names in scope, imports,
`self` and `super()`, parameters and variables with a type annotation or
a constructor call, and fields assigned such values. A mixed project gets
both front ends at once.

Qt 6 (C++ and Qt Quick), with the controls, themes and fonts of
[Cache](../cache) and [igitt](../igitt).

## The views

All views share one *active symbol*. Clicking anything — a node, a
member, a name in the code, a row, a finding — makes it the active one
everywhere; Alt+Left and Alt+Right, or the mouse's thumb buttons as in a
browser, go back and forth.

| View | What it shows |
|---|---|
| **Graph** (Ctrl+1) | Sourcetrail's view. The active symbol in the middle with its members grouped by access; what uses it on the left, what it uses on the right, base classes above, subclasses below. Uses within the class are brackets beside the members. Edges are coloured by kind (calls, type uses, variable uses, inheritance, overrides, macros), thick by count; click one for the references behind it. Libraries are hatched and hidden unless asked for; more than 24 neighbours on a side fold into “n more”. Group by namespace or file. Export as SVG or PNG. |
| **Flow** (Ctrl+2) | How control flows. On the left a call tree in the order of the code (or who calls it), unfolding on demand, with virtual dispatch to every override and recursion marked. On the right the same calls as a layered graph to any depth — or every call path from the function to another one. |
| **Architecture** (Ctrl+3) | The architecture as it is: modules (folders, to any depth), files or classes, layered so dependencies sit below what uses them. Cycles in red. Beside it each module's coupling after Robert C. Martin (afferent, efferent, instability, abstractness, distance from the main sequence) with the main-sequence chart. |
| **Matrix** (Ctrl+4) | A dependency structure matrix of modules, files or classes, ordered so dependencies come first: in a layered design everything is below the diagonal, cycles are framed in red. Click a cell for its references. |
| **Bundles** (Ctrl+5) | Holten's hierarchical edge bundles: folders, files and their classes and functions on a circle, every dependency inside it bundled along the hierarchy, green at the user and red at the used. Point at a node for only its own. |
| **Treemap** (Ctrl+6) | Folders, files and what is written in them, area by lines (or statements), colour by complexity, callers, uses, nesting, findings or “looks unused”. Double-click to zoom into a folder. |
| **Metrics** (Ctrl+7) | A table lens of every function, class, file and module: lines, McCabe and cognitive complexity, nesting, parameters, callers, callees; methods, weighted methods, inheritance depth, coupling and cohesion of classes. Bars for each value's share, review thresholds in red. Copies as CSV. |
| **Review** (Ctrl+8) | What deserves a reviewer's attention, each with why it matters and a link to its code. Exported as a Markdown report for a pull request, a ticket or an assistant. |
| **Changes** (Ctrl+9) | Only the code that chosen commits changed. Pick commits from the git history (Ctrl adds one, Shift a range), or the uncommitted changes; their changed lines are carried forward to the files as they are now and mapped to the classes, functions, fields and macros they are in. The graph shows those with their changed members (new ones in green, changed in yellow, with the number of lines) and how they use each other — optionally with what uses them, the change's reach, and what they use. Beside it the changed symbols, files and the review's findings in the change, exported as Markdown. Every other view marks the changed members, and the code view the changed lines. |

Beside every view: the active symbol's **details** (signature, where it
is, its measurements, what the review says about it) and the **code**,
as in Sourcetrail — the definition first, then every place that uses it
with the lines around, each under the function it is in. Every name
clang resolved is a link. Or the whole file.

**Folding.** Any folder can be folded into one node — third-party code, generated code, a subsystem
you aren't reviewing — and everything that uses something in it then points at that node. Fold from
the project tree or a node's context menu (right-click), or with *Fold → Fold third-party code*, which
finds folders such as `3rdparty`, `third_party`, `vendor` or `external`. Folded folders apply to the
Graph, Architecture, Matrix and Bundles views, are kept per project, and open again with a click on
their node, from the menu or the tree.

In the Graph, neighbours from another namespace (or Python module) than the active symbol's fold into
one node per namespace, `ns:: 4 folded: …`, when there are two or more of them; a click opens it, its
context menu folds it again, and *Fold namespaces* in the toolbar turns this off.

Everything is drawn on the GPU through Qt Quick's scene graph: the edges as one vertex buffer per
colour, the bundles, treemap and matrix as coloured meshes with scene-graph text, so panning,
scrolling and zooming only move a transform and hovering redraws only what it highlights. They can be curved, straight or at right angles
(*Settings*, or the button beside the zoom).

On the left the **project**: folders, files and what they declare, or the
namespaces; bookmarks (Ctrl+D). **Search** (Ctrl+K) is fuzzy over every
symbol and file (`inv::rep` finds `Inventory::report`).

## What the review looks for

- **Parse errors**: files clang couldn't compile cleanly, so the picture of them may be incomplete
- **Include, module and class cycles**, and **recursion** (direct or through other functions)
- **Complex functions** (McCabe and cognitive complexity), **long** ones, **deep nesting**, **many parameters**
- **Large classes**: many methods, much logic (weighted methods), many dependencies, low cohesion
- **Duplicated code**: functions with the same token structure (names and literals made alike), and near
  duplicates by MinHash over token shingles — the helper written three times
- **Possibly unused** functions, methods, types, macros and globals. Qt's meta-object system is taken into
  account: slots, signals, `Q_INVOKABLE`, `Q_PROPERTY` accessors, `Q_ENUM`s and QML types count as used
- **Written, never read** fields and variables
- **Mutable global state** with the functions that write it
- **Deep inheritance**, and polymorphic bases whose **destructor isn't virtual**
- **Large files**, and **files nothing builds** (sources no translation unit compiles, headers nothing includes)

Thresholds are those of common practice (complexity 15, cognitive 25, 80 lines, nesting 4, 6 parameters…);
see `Analysis::Thresholds` in [src/core/analysis.h](src/core/analysis.h).

## Opening a project

Open a folder (Ctrl+O), a `compile_commands.json`, or drop either on the
window; from a terminal `unravel path/to/project`. Unravel then

1. uses the `compile_commands.json` in the folder or one of its build folders (CMake with
   `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, Meson, `bear -- make` all write one);
2. or, for a CMake project without one, runs `cmake` to write one into its cache (with the Qt it was
   built with on the prefix path, so Qt projects configure);
3. or compiles every source under the folder with the include paths where the headers are, and Qt's when
   the code includes Qt headers.

Python files under the folder are read as well (with `python3`, which must be on the path), skipping
virtual environments, `site-packages`, `__pycache__`, build trees and hidden folders.

Parsing runs on all cores (up to 12); the index is kept in the cache folder, so the next start is
instant, and the status bar says when the sources changed since. Extra compiler arguments for a project
go in the settings. Build trees inside the project count as *generated*: indexed for their uses (moc's
calls of slots), left out of the architecture views and the review.

The editor that “open in editor” (Ctrl+E) starts is set in the settings, with `{file}` and `{line}`
(`code -g {file}:{line}` by default).

## Building

Requires Qt ≥ 6.9, CMake ≥ 3.21 and libclang from LLVM 17 or newer (`libclang-dev` on Debian and Ubuntu,
`llvm` from Homebrew, the LLVM installer on Windows). `UNRAVEL_LLVM_ROOT` points CMake at an LLVM it
doesn't find by itself.

```bash
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=$HOME/Qt/6.12.0/gcc_64
cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/unravel samples/warehouse
```

`samples/warehouse` is a small program with known flaws (layers that depend on each other, a helper
written three times, a base without a virtual destructor, global state, dead code) to try it on.

### The command line

`unravel-cli` indexes without the window, for scripts and CI:

```bash
./build/bin/unravel-cli path/to/project --report review.md      # the review as Markdown
./build/bin/unravel-cli path/to/compile_commands.json --dump    # every symbol and relation
./build/bin/unravel-cli path/to/project --save index.idx        # the index, to --load later
```

`unravel --grab shot.png --view architecture --symbol ns::Class project` saves a screenshot of a view;
`unravel --view changes --commits a1b2c3d,worktree project` opens on the code those commits changed;
`--fold vendor,src/generated` folds folders on start. `UNRAVEL_PAN_BENCH=1` pans the visible graph and
prints the time per frame.

## How it is made

- [src/core](src/core) — the engine, Qt Core and libclang only:
  - `model` — files, symbols (by clang's USR), references, the aggregated edges; saving and loading
  - `project` — finding and reading the compilation database, cleaning compiler arguments for clang
  - `indexer` — libclang's indexer callbacks on worker threads, one index per thread. Each project header
    is indexed by the first unit that includes it with a given set of defines, so a header included by
    hundreds of units is read once per configuration; macros (and macros used in macros) from the
    preprocessing record; body metrics and clone signatures. `UNRAVEL_TIMING=1` prints where the time went
  - `analysis` — dependency graphs at three levels, strongly connected components, Martin's metrics,
    class metrics, call trees and paths, the findings
  - `layout` — a layered (Sugiyama) layout and a squarified treemap
  - `report` — the Markdown review
  - `python` — the Python front end: a script embedded in the binary (`python/unravel_python.py`, using
    only `ast` and `tokenize`) run with `python3 -I`, its JSON merged into the model with `py:` USRs
  - `history` — the git log and diffs (with the git command line), changed lines carried forward to the
    work tree and mapped to symbols
- [src/app](src/app) — what the QML uses: the `Session` (opening, indexing, the active symbol, history,
  bookmarks), list and tree models, the graph scenes, and the painted views (edges, bundles, treemap,
  matrix)
- [qml](qml) — the window and the views
- [style](style) — igitt's Qt Quick Controls style and themes, unchanged, as in Cache

## Limits

- What clang can't see, Unravel can't: code behind `#if`s the build doesn't take, calls through function
  pointers stored at run time, what QML, scripts or other programs call. “Possibly unused” is a list to
  check, not to delete.
- Calls of virtual methods are recorded to the method named; the flow view adds every override as a
  possible target.
- Templates are indexed as written, not per instantiation.
- Without a compilation database flags are guessed, and parse errors show where they were wrong.
- Python is dynamic: a call through a name whose type the code doesn't state is resolved only when
  exactly one method of the project has that name, `getattr`, monkey patching
  and decorators that replace functions are invisible, and what is imported from outside the project
  stays a name.

## Credits

The ideas: Sourcetrail (Coati Software, GPL) for the graph and code views; Alexandru Telea's group
(SolidSX, CPPVis, Utrecht and Groningen) and Danny Holten for edge bundles, table lenses and treemaps.
clang and libclang (LLVM, Apache 2.0 with LLVM exception). Icons: Lucide (ISC). Fonts: Inter and
JetBrains Mono (SIL Open Font License). Style and themes: igitt.
