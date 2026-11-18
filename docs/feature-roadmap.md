# Unravel: review-centric feature roadmap

## Recommendation

Build on Unravel's existing graph-based navigation rather than adding twelve
independent views. The next major capability should be **explainable review
prioritization**: combine the structure and complexity Unravel already knows with
Git history, then let a reviewer move from a hotspot to its dependencies, changed
code, and supporting evidence.

Recommended delivery order:

1. History-based hotspots and heatmaps.
2. A ranked Review Focus mode, integrated with Changes.
3. Architecture rules and header-impact analysis.
4. Qt-aware event-flow navigation and structural symbol queries.
5. Function-level control-flow diagrams.
6. Experimental state, ownership, and data-flow analysis, promoted separately
   only when their evidence is useful and trustworthy.

The baseline below describes the source at planning time, before the first
increment. It came from reading the source and documentation, not from running
or benchmarking the app. Delivery progress is recorded separately below.
Effort labels are relative: **S** is a localized extension, **M** spans the core
and UI, **L** introduces a substantial analysis pipeline, and **XL** requires
research before a delivery estimate is credible.

### Delivery progress

The first slice of A is implemented:

- Batched historical file metrics for resolved `HEAD`, with a configurable
  180-day default window and per-project window settings.
- Non-merge commit counts, added/removed lines, mailmap-resolved author counts,
  and last-change timestamps.
- File Metrics columns and file-level Treemap churn coloring, with shared
  refresh/cancel controls and asynchronous, generation-checked results.
- Explicit unavailable/error/cancelled/shallow-history states, unknown binary
  line totals, and CSV scope metadata.
- Focused Git fixtures and app/model tests covering metrics, file-level
  Treemap values, cancellation, project switching, and saved window settings.

This slice deliberately counts **pathnames**, not rename lineages. Renames are
treated as deletion/addition; the UI and export state that limitation. A is not
complete: rename continuity, persistent result caching, revision selection,
complexity-plus-churn ranking/coloring, and large-project performance validation
remain. The next slice should establish trustworthy rename attribution and
cache invalidation before building B's historical ranking.

## 1. What already exists, and what should change

| Suggested capability | Current Unravel support | Proposed direction |
|---|---|---|
| Call graphs | Flow has source-ordered call trees, callers, virtual overrides, recursion, depth-limited graphs, and paths between functions. | Improve precision labels, call-site navigation, and Qt event connections; do not build another call-graph view. |
| Include dependency graphs | The model records includes and reverse includes; the file graph shows direct relationships; analysis finds include cycles. | Add transitive header impact, explanatory include chains, and distinct include-only analysis. |
| Class interaction diagrams | Graph exposes members and typed relationships; Architecture and Matrix support class-level dependencies; class metrics already exist. | Add relationship filters, structural queries, and architecture constraints rather than a separate UML editor. |
| Hotspot analysis | Function complexity and Git changes exist separately. | Combine them with historical change frequency, initially at file level. |
| Interactive control flow | Flow currently represents calls, not basic blocks, branch conditions, or loop paths. | Add an explicitly named Control Flow mode backed by a real CFG. |
| State transitions | Enum symbols and references provide ingredients, but no transition model exists. | Start with explicit enum-backed state assignments and supported Qt state-machine constructs; show inference limits. |
| Lifetime visualization | Types, references, and read/write flags provide ingredients, but there is no ownership or lifetime model. | Begin with ownership relationships, not claims that lifetimes are proven safe. |
| Git-powered review | Changes maps selected commits and working-tree changes to current symbols and findings. | Add historical metrics, on-demand blame, and contributor context. |
| Architectural layers | Architecture shows observed folder/file/class dependencies and cycles. | Add user-defined logical layers and forbidden-edge rules; observed layout is not architectural intent. |
| Data flow | References to fields/globals have read/write flags, but locals are skipped by the C++ reference indexer. There is no def-use model. | Introduce local identities and def-use facts before offering backward/forward slices. |
| Complexity heatmaps | Treemap already colors by complexity and other metrics; Metrics already provides sortable measurements. | Add churn, review priority, and header-impact color modes to the existing views. |
| Semantic search and symbol graphs | Compiler-resolved references, inheritance, overrides, and fuzzy symbol/file search already exist. | Add structured relationship queries; distinguish these from natural-language or embedding search. |

### Source anchors

- [README](../README.md): documented workflows, views, and limitations.
- [Code model](../src/core/model.h): symbols, reference kinds/flags, includes,
  metrics, and immutable finalized indexes.
- [Analysis API](../src/core/analysis.h) and
  [analysis implementation](../src/core/analysis.cpp): dependency graphs,
  call paths, findings, and current kind/severity/score ordering.
- [History API](../src/core/history.h) and
  [implementation](../src/core/history.cpp): Git log, diffs, forward line mapping,
  and touched symbols, but not historical hotspot statistics.
- [Indexer](../src/core/indexer.cpp): body measurements, Qt meta-callable
  recognition, and reference extraction.
- [File/call graph scenes](../src/app/graphscene.cpp),
  [models](../src/app/models.h), and [Flow UI](../qml/FlowView.qml):
  existing navigation and presentation foundations.
- [Changes controller](../src/app/changes.h) and
  [Session](../src/app/session.h): asynchronous change analysis and shared
  selection/model lifecycle.
- [Core tests](../tests/tst_core.cpp): existing analysis fixtures, index
  round-trips, Python coverage, and temporary Git repositories.

## 2. Principles for every increment

1. **Evidence before inference.** Every relationship and ranking contribution
   should lead to source locations, commits, or an explicit rule.
2. **Do not imply execution certainty.** Separate direct calls, address-taking,
   possible virtual targets, event connections, and unresolved dynamic behavior.
3. **Reuse the shared active symbol.** Review Focus, Changes, graphs, metrics,
   and source should remain one connected workflow.
4. **Preserve existing behavior.** Add optional modes and overlays; do not
   silently replace current finding severity or change selection semantics.
5. **Keep costs bounded and visible.** Use background jobs, cancellation,
   progressive results, and explicit truncation/coverage indicators.
6. **Keep analysis local by default.** Do not require a hosted service, upload
   code, or execute the analyzed program.
7. **Respect language differences.** Shared views should remain usable for
   Python and mixed projects; C++-specific analyses must say when unsupported.

## 3. Delivery increments

### A. Historical hotspots and heatmaps

**Priority:** P0. **Effort:** M. **Prerequisite:** none.

**Reviewer question:** Which complicated files are repeatedly being changed?

Extend the existing Git backend with batched historical statistics over an
explicit revision and time window. Start with files, where attribution is
defensible without reconstructing every historical function.

Deliver:

- Per-file distinct touching commits, added/deleted lines, last-change date,
  and contributor count.
- A default 180-day window, visible in the UI and adjustable per project.
- Explicit merge treatment: initially count non-merge commits reachable from
  the selected revision, so merge commits do not double-count branch changes.
- Rename tracking where Git can establish continuity; surface uncertain
  lineage, shallow history, and unavailable objects.
- `.mailmap`-aware contributor identities, displayed as code context rather
  than a measure of an individual's quality.
- Metrics columns and Treemap color modes for churn and complexity-plus-churn.
- A scatterplot or compact table pairing complexity with change frequency,
  using the existing Metrics/Review surfaces before adding another main tab.
- Separate states for no Git repository, successful query with zero changes,
  incomplete history, and command failure.

Do not treat the current commit-list default of 500 entries as the statistical
sample. The history query must cover its stated window or label itself partial.
Uncommitted changes belong in Changes, not historical commit counts.

**Acceptance:**

- A controlled Git fixture yields exact file touch counts and line totals
  across edits, a rename, deletion, merge, and a time-window boundary.
- Multiple hunks in one commit count as one touching commit per file.
- Git failure is not displayed as zero churn.
- Changing revision/window invalidates the historical result without forcing
  a C++ reindex; warm-cache results match uncached results.

### B. Review Focus: an explainable top-20 queue

**Priority:** P0. **Effort:** M-L. **Prerequisite:** A.

**Reviewer question:** Where should I start, and why?

Add **Focus** as a mode of Review, not a tenth primary view. Provide two
independent scopes: the whole project and the symbols touched by the current
Changes selection. Default to the top 20 functions, with an option to see all.

Each row should show:

- Function, file, priority, complexity, churn, and dependency fan-in/fan-out.
- Existing findings and whether the function is changed.
- A concise explanation of the largest score contributions.
- History attribution quality: function estimate, file-level proxy, partial,
  or unavailable.

Use an initial, versioned **review-priority heuristic**, not a defect
probability. A concrete starting point for evaluation is:

```text
C = max(1, McCabe / 15, cognitiveComplexity / 25)
H = log2(1 + distinctTouchingCommits)
D = log2(1 + distinctIncomingAndOutgoingSymbolNeighbours)

priority = C * (1 + H) * (1 + 0.25 * D)
```

This uses existing complexity thresholds, compresses extreme history values,
and does not erase complex new code merely because churn is zero. Keep current
finding severity separate: parse errors and other serious findings must remain
visible even when their functions are not in the queue.

Do **not** multiply by number of authors by default. Contributor count can
reflect healthy collaboration, bot activity, or identity aliases rather than
defect risk. Likewise, do not claim an 80% review-effort reduction without
measuring it.

Stage function history honestly:

1. Initially use file history as an explicitly labeled proxy for each function.
   The queue prioritizes functions but does not claim function-level churn.
2. Add historical hunk attribution using the existing line-mapping machinery.
   Mark overlap/replacement ambiguity rather than inventing exact symbol
   continuity. Do not equate current `Touched.lines` with historical churn.
3. Only introduce exact historical symbol comparisons if parsing historical
   revisions proves worth its storage and indexing cost.

For unavailable history, offer a clearly named static-priority mode using
`C * (1 + 0.25 * D)`. Do not silently mix unknown churn and known zero churn
in a supposedly complete historical ranking.

Integrate with:

- Treemap and Metrics through a priority overlay/column.
- Graph/Flow through activation and dependency inspection.
- Changes through a touched-functions scope and a separately labeled,
  depth-bounded potential-impact expansion.
- Markdown reports and a structured JSON export containing score version,
  scope, window, component values, and coverage.

**Acceptance:**

- Returns `min(20, eligibleFunctionCount)` unique functions by default.
- Stable ties resolve by qualified name, file, and source position.
- A complex zero-churn function retains nonzero priority.
- File-proxy and incomplete-history labels survive UI and report export.
- Scope changes cannot publish results from an older asynchronous job.
- Existing review categories, severity, and Changes exports remain available.

### C. Architecture contracts

**Priority:** P1. **Effort:** M. **Prerequisite:** none; can follow B for focus.

**Reviewer question:** Which dependencies violate the intended architecture?

Keep the observed architecture graph and add an optional, version-controlled
project rule file, for example `.unravel/architecture.json`. The exact schema
is a design decision for implementation, not an existing file format.

First release:

- Named logical modules/layers mapped by root-relative path globs.
- Directed allowed/forbidden dependencies between those modules.
- Separate rule scopes for includes and semantic symbol dependencies.
- Explicit behavior for overlapping assignments; reject ambiguous mappings
  rather than silently choosing a layer.
- An unassigned-files bucket and configuration diagnostics.
- Highlighted violating edges in Architecture and Matrix, with references or
  include lines as evidence.
- Rule IDs in Review and reports; suppressions require a reason and identify
  the specific exception.
- CLI output and a dedicated opt-in exit status for contract violations.

Later additions: namespace-based mapping, rule baselines, and comparison of
new violations against a base revision. These are not required for the first
contract checker.

**Acceptance:** a fixture with UI -> Domain -> Infrastructure plus UI -> Database
reports only the forbidden edge, with the correct direction and evidence.
Malformed rules produce a configuration error, not a clean architecture report.
Projects without rules keep today's observed-architecture behavior.

### D. Header impact and include hygiene

**Priority:** P1. **Effort:** M. **Prerequisite:** none.

**Reviewer question:** Which headers have the widest build impact, and why?

Extend the existing file graph and include data with:

- An include-only mode, distinct from semantic dependencies.
- Transitive reverse-include reachability, cycle-safe and deduplicated.
- A count/list of affected indexed translation units, with witness chains such
  as translation unit -> intermediate header -> selected header.
- Header-impact ranking and a corresponding Treemap color mode.
- Include-cycle evidence and candidates for splitting highly exposed headers.
- Visible graph limits and an expand action instead of unexplained clipping.

Call the result **estimated translation-unit impact**, not measured rebuild
time. It reflects indexed build configurations and may omit dependency paths
through external/generated code, precompiled headers, header units, or modules.
The current model aggregates include facts without retaining every
translation-unit/configuration provenance edge; add that provenance where
necessary before promising per-configuration accuracy.

Forward-declaration and pimpl suggestions should initially be advisory.
Do not label an include unnecessary merely because no direct symbol use was
found: macros, transitive requirements, and completeness rules matter. Evaluate
IWYU or clang-scan-deps as optional, explicitly invoked integrations later.
Do not introduce a mandatory external-tool dependency for basic impact analysis.

**Acceptance:** diamond-shaped includes count each affected translation unit
once; cycles terminate; every reported impact has a witness chain; partial
coverage is labeled; include-only mode excludes ordinary call/type-use edges.

### E. Qt-aware event flows and structural queries

**Priority:** P1. **Effort:** L. **Prerequisite:** none.

**Reviewer question:** How does an event reach this code, and what implements
this interface?

This has unusually high value for C++/Qt projects. Current meta-callable
recognition helps avoid false unused findings, but it does not establish
signal-to-slot event topology.

Deliver in two slices:

1. Improve existing call semantics: expose direct calls, address-taking,
   implicit calls, and possible virtual dispatch distinctly. Preserve
   source-site evidence when multiple sites share a callee.
2. Recognize typed `QObject::connect` expressions: sender, signal, receiver or
   context, slot/lambda, and a statically visible connection type. Add a
   dedicated event-connection relationship rather than treating connection
   registration as an immediate call.

Show event edges as potential delivery. `AutoConnection` can depend on runtime
thread affinity; queued delivery, disconnects, receiver lifetime, and runtime
conditions prevent exact execution claims. Mark string-based connects,
dynamic invocation, and unsupported QML connections as unresolved.

Extend search with structured filters/actions:

- Callers/callees of a symbol, optionally transitive and depth-bounded.
- Subclasses, implementors, and overrides.
- Readers/writers of a field or global.
- Dependencies crossing a selected module boundary.

Reuse indexed relationships and source navigation. Natural-language search
and embeddings are not necessary for this increment.

**Acceptance:** fixtures distinguish connect registration from signal delivery,
map a typed signal-to-slot connection and a lambda connection, preserve virtual
dispatch alternatives, and display unsupported dynamic cases as incomplete.
Queries return navigable symbols/references rather than text matches alone.

### F. Real control-flow diagrams

**Priority:** P2. **Effort:** L. **Prerequisite:** compiler capability spike.

**Reviewer question:** Which branches and exits can this function take?

The existing call graph is not a CFG. Start with a bounded feasibility spike
using representative C++/Qt functions. The current backend is libclang's C API;
do not assume it exposes Clang's C++ CFG analysis.

Evaluate an optional Clang LibTooling helper that consumes the same compilation
database and returns function-scoped CFG facts. Prefer keeping its additional
LLVM linkage and version coupling outside the existing lightweight core.
Retain the current backend for all existing features.

If feasible, add a Control Flow mode inside Flow:

- Entry/exit, basic blocks, branch conditions, loops, switch arms, and returns.
- Source-range navigation and selection of calls inside each block.
- Collapsible straight-line regions and explicit size limits.
- A visible exception-edge policy; do not imply exception completeness unless
  modeled and tested.

Initially defer coroutine suspension, full template-instantiation coverage,
and cross-function CFG expansion.

**Acceptance:** fixtures verify branch targets, loop back-edges, switch
fallthrough, early returns, and the declared exception policy. Unsupported
functions produce an explanation, never a plausible but fabricated diagram.
An installation without the optional helper retains all existing functionality.

### G. Evidence-limited state, ownership, and data-flow experiments

**Priority:** P3. **Effort:** XL collectively; separate go/no-go decisions.

These are different analyses, not three renderers over today's reference graph.
Prototype them one at a time after the preceding increments prove useful.

#### G1. State transitions

- Begin with a selected enum-backed state field, guarded assignments, and
  explicit transitions supported by Qt state-machine APIs.
- Record source/target state, guard, action, and source location where known.
- Distinguish an observed assignment from a proven transition whose source
  state is constrained. Display unknown origins explicitly.
- Invalid-transition findings require a declared allowed-transition contract;
  absence from statically observed code is not proof of invalidity.

**Gate:** supported fixtures reconstruct expected transitions with evidence
and leave unconstrained writes unknown rather than inventing source states.

#### G2. Ownership relationships before lifetime analysis

- Represent value containment, `unique_ptr`, `shared_ptr`, `weak_ptr`,
  references, raw pointers, `QPointer`, and supported Qt smart pointers.
- For QObject ownership, inspect explicit parent relationships and reparenting
  evidence; raw pointers alone do not prove ownership.
- Distinguish ownership, shared ownership, borrowing, observation, and unknown.
- Treat shared-pointer cycles as review candidates only where owning edges
  are established.

**Gate:** a shared object is represented once with multiple incoming owners;
borrowing is not drawn as ownership; unresolved transfers remain unknown.
Do not promise general dangling-pointer detection. Heaptrack, ASan, and static
analyzer result imports are separate optional integrations, not consequences
of drawing a type graph.

#### G3. Local data-flow slices

- First add local variables, parameters, expression/value identities,
  assignments, and def-use relationships; preserve lexical scope/shadowing.
- Start with intraprocedural backward/forward slicing for explicit assignments
  and supported expressions, using the CFG from F.
- Then consider bounded interprocedural summaries for arguments and returns.
- Show aliasing, macros, unsupported expressions, and unknown call effects as
  limits; do not imply whole-program data provenance.
- Validation/sanitization claims require explicit source/sink/transform models
  and separate verification.

**Gate:** fixtures cover shadowing, reassignment, branch joins, and an unknown
call effect. Every slice edge has source evidence, and unknown influence
remains visible.

## 4. Supporting engineering, scoped to the features

### Data and cache boundaries

- Keep Git metrics separate from the immutable finalized code model. Key
  history caches by repository identity, revision, window, algorithm version,
  and identity/rename policy; do not persist transient vector IDs as identity.
- Join current facts using normalized paths and symbol USRs where applicable.
  USRs do not by themselves establish identity through historical refactors.
- Invalidate worktree-derived results separately from committed history.
- Version any new serialized model facts and either migrate or explicitly
  rebuild incompatible index caches.
- Reuse Session/Changes generation checks to prevent stale asynchronous results
  from being adopted after a project switch or reindex.
- Add cancellation/error propagation to new Git jobs; avoid one subprocess per
  function and do not silently convert failures to empty successful results.

### Scale and responsiveness

Record a baseline before choosing data structures or promising performance.
Use a small correctness fixture and a reproducible large-project fixture,
documenting hardware, compiler configuration, commit range, and graph size.

Proposed release targets, subject to baseline confirmation:

- No full reindex when changing a history window or focus filter.
- Cached top-20 selection/filtering completes within 200 ms at p95 on the
  agreed large fixture; heavy mining runs off the GUI thread.
- Cancellation is acknowledged within one second, including during Git work.
- New overlays add no more than 10% to existing pan-frame time on the same
  fixture, using the existing pan benchmark.
- Report cold/warm history time, peak memory, cache size, and coverage; do not
  substitute a tiny sample result for large-codebase validation.

Incremental C++ reindexing may become necessary, but it is a separate project.
Only schedule it when measurements show full indexing blocks these workflows;
correct incremental invalidation must account for headers and build arguments.

### Testing and reporting

- Extend the existing core tests with focused analysis and temporary Git
  fixtures; use small purpose-built code snippets for new semantic facts.
- Validate round-trips when persistent model facts change.
- Add focused app/model tests for asynchronous cancellation, stale-result
  rejection, scope selection, and report/UI agreement.
- Keep C++, Python, and mixed-project behavior covered where features are
  shared; unsupported language-specific modes should explain why.
- All exports must include scope, revision/window, heuristic/rule version,
  coverage, and relevant evidence, so a report can be interpreted later.

## 5. Release sequence and success criteria

| Release | Scope | Exit criterion |
|---|---|---|
| 1: Hotspots | A, plus static/history coverage indicators | Reviewers can identify complex, frequently changed files and inspect the underlying commits. |
| 2: Review Focus | B, with explicitly labeled file-history proxies initially | A reproducible top-20 function queue works for project and touched-symbol scopes, with explanations and exports. |
| 3: Architecture review | C and D, deliverable independently | Rules identify evidenced violations, and header impact has deduplicated translation-unit estimates. |
| 4: Qt navigation | E | Reviewers can follow supported typed signal/slot relationships and query indexed structure without confusing registration with execution. |
| 5: Branch reasoning | F, only after the backend spike passes | Supported functions have source-linked, tested CFGs; the helper remains optional. |
| Research track | G1-G3, separately | Each prototype passes its evidence/unknown-handling gate before becoming a normal feature. |

Do not assign calendar dates to CFG, state, ownership, or data-flow work until
the compiler and inference spikes establish feasibility.

Evaluate usefulness with representative review tasks: identify a hotspot,
explain a header's impact, locate a forbidden dependency, and trace a Qt event.
Compare time to a correct explanation, navigation steps, and false-positive
burden against the current app. Treat priority scores as hypotheses to tune,
not validated defect predictors.

**First implementation slice:** historical file metrics, exact Git-fixture
tests, Metrics columns, and Treemap churn coloring. This is the smallest
coherent addition that combines capabilities Unravel already has and establishes
the evidence needed for Review Focus.
