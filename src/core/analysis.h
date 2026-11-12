#pragma once

#include "model.h"

#include <QHash>
#include <QPair>
#include <QSet>

// What a reviewer wants to know about a code base beyond its facts:
// dependency graphs at the level of modules, files and classes, their
// cycles, the coupling of modules, code that looks unused, duplicated or
// too complex, and the call paths between functions.
namespace Unravel::Analysis {

// --- Dependency graphs --------------------------------------------------------

enum class Level { Module, File, Class };

struct DepGraph {
    Level level = Level::Module;
    QStringList names;          // per node
    QVector<qint32> ids;        // per node: module index, file id or symbol id
    QVector<qint32> size;       // lines of code (modules, files) or members (classes)
    QVector<qint32> complexity; // summed complexity of the node's functions
    struct Link {
        qint32 from, to;
        qint32 count = 0;       // references
        qint32 includes = 0;    // #includes (modules and files)
        QVector<qint32> kinds = QVector<qint32>(int(RefKind::Count), 0);
        QVector<qint32> refs;   // the references behind it
        bool cyclic = false;    // both ends are in one strongly connected component
    };
    QVector<Link> links;
    QVector<qint32> component;  // per node, its strongly connected component
    QVector<qint32> componentSize;
    QVector<QVector<qint32>> out; // per node, indexes into links
    QVector<QVector<qint32>> in;

    qint32 linkIndex(qint32 from, qint32 to) const { return m_linkIds.value(key(from, to), -1); }

    // Internal
    static quint64 key(qint32 a, qint32 b) { return (quint64(quint32(a)) << 32) | quint32(b); }
    QHash<quint64, qint32> m_linkIds;
};

// The module of a file cut to `depth` folders (0 = whole path)
QString moduleAt(const QString &module, int depth);

// `filterModule`: only nodes in this module (and what they connect to), "" = all
DepGraph dependencyGraph(const CodeModel &m, Level level, int moduleDepth = 0, bool withExternal = false,
                         const QString &filterModule = QString());

// The references behind a link of a dependency graph, at most `limit`
QVector<qint32> linkReferences(const CodeModel &m, const DepGraph &g, qint32 link, int limit = 500);

// Strongly connected components (Tarjan, iterative): per node its component
QVector<qint32> components(qint32 n, const QVector<QVector<qint32>> &adjacency, QVector<qint32> *sizes = nullptr);

// Orders nodes so dependencies come first, cycles kept together
QVector<qint32> dependencyOrder(const DepGraph &g);

// --- Module metrics (Robert C. Martin) ---------------------------------------

struct ModuleMetrics {
    QString name;
    qint32 files = 0, lines = 0, classes = 0, abstractClasses = 0, functions = 0;
    qint32 afferent = 0;     // modules that depend on it (Ca)
    qint32 efferent = 0;     // modules it depends on (Ce)
    double instability = 0;  // Ce / (Ca + Ce)
    double abstractness = 0; // abstract classes / classes
    double distance = 0;     // |A + I - 1|, from the main sequence
    qint32 complexity = 0;
};
QVector<ModuleMetrics> moduleMetrics(const CodeModel &m, int moduleDepth = 0);

// --- Classes ---------------------------------------------------------------

struct ClassMetrics {
    qint32 symbol = -1;
    qint32 methods = 0, fields = 0, publicMethods = 0;
    qint32 wmc = 0;            // weighted methods: summed complexity
    qint32 lines = 0;          // of the methods' definitions
    qint32 depth = 0;          // of inheritance
    qint32 children = 0;       // direct subclasses
    qint32 coupling = 0;       // distinct classes used (CBO, outgoing)
    qint32 usedBy = 0;         // distinct classes using it
    double cohesion = 0;       // share of method pairs using a common field (TCC-like)
};
ClassMetrics classMetrics(const CodeModel &m, qint32 cls);

// --- Calls -----------------------------------------------------------------

struct CallSite {
    qint32 callee = -1;
    qint32 ref = -1;       // the first reference, for its location
    qint32 count = 1;
    bool implicit = false;
    bool virtualCall = false;
};
// What a function calls, in source order, one entry per callee
QVector<CallSite> callsFrom(const CodeModel &m, qint32 fn, bool withImplicit = false);
// Who calls a function: one entry per caller
QVector<CallSite> callersOf(const CodeModel &m, qint32 fn, bool withImplicit = false);
// The methods overriding a method, transitively
QVector<qint32> overriders(const CodeModel &m, qint32 method);

// Every function and edge on a call path from `from` to `to` of at most
// `maxDepth` calls (overrides count as possible calls)
struct Subgraph {
    QVector<qint32> nodes;
    QVector<QPair<qint32, qint32>> edges;   // symbol ids
    QVector<bool> dispatch;                 // per edge: a virtual dispatch, not a call
};
Subgraph callPaths(const CodeModel &m, qint32 from, qint32 to, int maxDepth = 8);
// What `root` calls (or is called by) up to `depth` levels
Subgraph callNeighbourhood(const CodeModel &m, qint32 root, int depth, bool callees, bool callers,
                           int maxNodes = 400);

// --- Findings --------------------------------------------------------------

enum class FindingKind {
    ParseErrors,
    IncludeCycle,
    ModuleCycle,
    ClassCycle,
    Recursion,
    Complexity,
    LongFunction,
    DeepNesting,
    ManyParameters,
    GodClass,
    Duplicate,
    Unused,
    UnreadField,
    GlobalState,
    DeepInheritance,
    NonVirtualDestructor,
    LargeFile,
    OrphanFile,
    Count
};

struct Thresholds {
    qint32 complexity = 15;
    qint32 cognitive = 25;
    qint32 lines = 80;
    qint32 nesting = 4;
    qint32 params = 6;
    qint32 classMethods = 30;
    qint32 classWmc = 80;
    qint32 classCoupling = 20;
    qint32 inheritance = 5;
    qint32 fileLines = 1000;
    qint32 cloneTokens = 60;
    double cloneSimilarity = 0.8;
};

struct Finding {
    FindingKind kind = FindingKind::Complexity;
    qint32 severity = 1;      // 0 note, 1 warning, 2 serious
    QString title;
    QString detail;
    QVector<qint32> symbols;  // the symbols concerned, the main one first
    QVector<qint32> files;
    Location loc;
    double score = 0;         // for sorting within a kind: bigger is worse
};

struct Report {
    QVector<Finding> findings;
    QVector<qint32> perKind = QVector<qint32>(int(FindingKind::Count), 0);
    qint32 serious = 0, warnings = 0, notes = 0;
};

QString findingKindName(FindingKind k);
QString findingKindHint(FindingKind k);   // why it matters

Report analyze(const CodeModel &m, const Thresholds &t = {});

// Symbols that look unused (no references from anywhere but their declarations)
bool looksUnused(const CodeModel &m, qint32 s);

} // namespace Unravel::Analysis
