#include "analysis.h"

#include <QFileInfo>
#include <QQueue>

#include <algorithm>
#include <functional>
#include <numeric>

namespace Unravel::Analysis {

QString moduleAt(const QString &module, int depth)
{
    if (depth <= 0 || module.isEmpty() || module.startsWith(QLatin1Char('<')))
        return module;
    const QStringList parts = module.split(QLatin1Char('/'));
    return parts.mid(0, depth).join(QLatin1Char('/'));
}

QVector<qint32> components(qint32 n, const QVector<QVector<qint32>> &adj, QVector<qint32> *sizes)
{
    QVector<qint32> index(n, -1), low(n, 0), comp(n, -1);
    QVector<bool> onStack(n, false);
    QVector<qint32> stack;
    qint32 counter = 0, compCount = 0;
    struct Frame {
        qint32 v;
        qint32 next;
    };
    QVector<Frame> call;
    for (qint32 s = 0; s < n; ++s) {
        if (index[s] >= 0)
            continue;
        call.append({s, 0});
        index[s] = low[s] = counter++;
        stack.append(s);
        onStack[s] = true;
        while (!call.isEmpty()) {
            Frame &f = call.last();
            const qint32 v = f.v;
            if (f.next < adj[v].size()) {
                const qint32 w = adj[v][f.next++];
                if (w < 0 || w >= n)
                    continue;
                if (index[w] < 0) {
                    index[w] = low[w] = counter++;
                    stack.append(w);
                    onStack[w] = true;
                    call.append({w, 0});
                } else if (onStack[w]) {
                    low[v] = std::min(low[v], index[w]);
                }
            } else {
                if (low[v] == index[v]) {
                    qint32 w;
                    do {
                        w = stack.takeLast();
                        onStack[w] = false;
                        comp[w] = compCount;
                    } while (w != v);
                    compCount++;
                }
                call.removeLast();
                if (!call.isEmpty()) {
                    const qint32 u = call.last().v;
                    low[u] = std::min(low[u], low[v]);
                }
            }
        }
    }
    if (sizes) {
        sizes->fill(0, compCount);
        for (qint32 c : std::as_const(comp))
            (*sizes)[c]++;
    }
    return comp;
}

namespace {

bool countsAsDependency(RefKind k)
{
    return k == RefKind::Call || k == RefKind::TypeUse || k == RefKind::VarUse || k == RefKind::Inherit
        || k == RefKind::MacroUse || k == RefKind::Override;
}

// The file code using a symbol depends on: where a type is defined (not
// where it is forward-declared), where a function is declared
qint32 declFile(const CodeModel &m, qint32 s)
{
    const Symbol &sym = m.symbols[s];
    if (isType(sym.kind) && sym.def.valid())
        return sym.def.file;
    return sym.decl.valid() ? sym.decl.file : sym.def.file;
}

void finishGraph(DepGraph &g)
{
    const qint32 n = qint32(g.names.size());
    g.out = QVector<QVector<qint32>>(n);
    g.in = QVector<QVector<qint32>>(n);
    QVector<QVector<qint32>> adj(n);
    for (qint32 i = 0; i < g.links.size(); ++i) {
        g.out[g.links[i].from].append(i);
        g.in[g.links[i].to].append(i);
        adj[g.links[i].from].append(g.links[i].to);
    }
    g.component = components(n, adj, &g.componentSize);
    for (DepGraph::Link &l : g.links)
        l.cyclic = g.component[l.from] == g.component[l.to] && g.componentSize[g.component[l.from]] > 1;
}

} // namespace

QString foldedFolderOf(const QString &module, const QStringList &folded)
{
    QString best;
    for (const QString &f : folded) {
        if (!f.isEmpty() && (module == f || module.startsWith(f + QLatin1Char('/'))) && (best.isEmpty() || f.size() < best.size()))
            best = f;
    }
    return best;
}

QStringList vendorFolders(const CodeModel &m)
{
    static const QStringList names = {"3rdparty", "3rd_party", "3rd-party", "thirdparty", "third_party", "third-party",
                                      "vendor", "vendors", "external", "externals", "extern", "deps", "_deps",
                                      "contrib", "submodules"};
    QStringList out;
    for (const QString &module : m.modules) {
        const QStringList parts = module.split(QLatin1Char('/'));
        for (qsizetype i = 0; i < parts.size(); ++i) {
            if (names.contains(parts[i].toLower())) {
                const QString top = parts.mid(0, i + 1).join(QLatin1Char('/'));
                if (!out.contains(top) && foldedFolderOf(top, out).isEmpty())
                    out << top;
                break;
            }
        }
    }
    // The outermost only
    QStringList outer;
    for (const QString &f : std::as_const(out)) {
        QStringList others = out;
        others.removeAll(f);
        if (foldedFolderOf(f, others).isEmpty())
            outer << f;
    }
    outer.sort();
    return outer;
}

DepGraph dependencyGraph(const CodeModel &m, Level level, int moduleDepth, bool withExternal,
                         const QString &filterModule, const QStringList &folded, bool includesOnly)
{
    DepGraph g;
    g.level = level;
    const qint32 nf = qint32(m.files.size());
    const qint32 ns = qint32(m.symbols.size());
    QHash<qint32, qint32> nodeOfId;     // file id / symbol id -> node
    QHash<QString, qint32> nodeOfModule;
    QVector<bool> nodeInFilter;

    // Each file's module, cut to the depth, once
    QVector<QString> moduleOfFile(nf);
    for (qint32 f = 0; f < nf; ++f) {
        const File &file = m.files[f];
        moduleOfFile[f] = file.project ? moduleAt(file.module, moduleDepth) : QStringLiteral("<external>");
    }
    auto inFilter = [&](const QString &module) {
        if (filterModule.isEmpty())
            return true;
        return module == filterModule || module.startsWith(filterModule + QLatin1Char('/'));
    };
    auto newNode = [&](const QString &name, qint32 id, qint32 size, bool in, const QString &folder = QString()) {
        g.names.append(name);
        g.ids.append(id);
        g.size.append(size);
        g.complexity.append(0);
        g.folder.append(folder);
        nodeInFilter.append(in);
        return qint32(g.names.size() - 1);
    };
    // Folded folders: one node each, at every level
    QVector<QString> foldOfFile(nf);
    if (!folded.isEmpty()) {
        for (qint32 f = 0; f < nf; ++f) {
            if (m.files[f].project)
                foldOfFile[f] = foldedFolderOf(m.files[f].module, folded);
        }
    }
    QHash<QString, qint32> foldNode;
    auto nodeForFold = [&](const QString &folder) {
        auto it = foldNode.find(folder);
        if (it == foldNode.end())
            it = foldNode.insert(folder, newNode(folder.isEmpty() ? QStringLiteral("(root)") : folder, -1, 0,
                                                 inFilter(folder) || (!filterModule.isEmpty() && filterModule.startsWith(folder + QLatin1Char('/'))),
                                                 folder));
        return *it;
    };

    // The node of a file, made on first use, cached; -1 to leave it out
    QVector<qint32> fileNode(nf, -2);
    auto nodeForFile = [&](qint32 file) -> qint32 {
        if (file < 0 || file >= nf)
            return -1;
        qint32 &cached = fileNode[file];
        if (cached != -2)
            return cached;
        const File &f = m.files[file];
        if ((!f.project || f.generated) && !withExternal) {
            cached = -1;
            return -1;
        }
        const QString &mod = moduleOfFile[file];
        if (!foldOfFile[file].isEmpty()) {
            cached = nodeForFold(foldOfFile[file]);
            return cached;
        }
        if (level == Level::Module) {
            auto it = nodeOfModule.find(mod);
            if (it == nodeOfModule.end())
                it = nodeOfModule.insert(mod, newNode(mod.isEmpty() ? QStringLiteral("(root)") : mod,
                                                      m.moduleIndex.value(mod, -1), 0, inFilter(mod)));
            cached = *it;
        } else {
            cached = newNode(f.relative, file, f.codeLines, inFilter(mod));
            nodeOfId.insert(file, cached);
        }
        return cached;
    };
    // The node of a symbol's outermost class, cached
    QVector<qint32> classNode(level == Level::Class ? ns : 0, -2);
    auto nodeForClass = [&](qint32 s) -> qint32 {
        if (s < 0 || s >= ns)
            return -1;
        qint32 &cached = classNode[s];
        if (cached != -2)
            return cached;
        cached = -1;
        const qint32 owner = m.outermostType(m.nodeOwner(s));
        if (owner < 0 || !isType(m.symbols[owner].kind) || m.symbols[owner].kind == SymbolKind::Typedef)
            return -1;
        const Symbol &sym = m.symbols[owner];
        if ((sym.has(External) && !withExternal) || sym.has(Generated) || sym.has(FromMacro))
            return -1;
        const Location where = sym.where();
        if (!folded.isEmpty() && where.file >= 0 && where.file < nf && !foldOfFile[where.file].isEmpty()) {
            cached = nodeForFold(foldOfFile[where.file]);
            return cached;
        }
        auto it = nodeOfId.find(owner);
        if (it == nodeOfId.end()) {
            const Location w = sym.where();
            const bool in = w.file >= 0 && inFilter(moduleOfFile[w.file]);
            it = nodeOfId.insert(owner, newNode(m.displayName(owner), owner, qint32(sym.children.size()), in));
        }
        cached = *it;
        return cached;
    };

    // A link counts only when one of its ends is in the filter
    auto addLink = [&](qint32 a, qint32 b, RefKind kind, bool include, qint32 ref) {
        if (a < 0 || b < 0 || a == b || !(nodeInFilter[a] || nodeInFilter[b]))
            return;
        const quint64 k = DepGraph::key(a, b);
        auto it = g.m_linkIds.find(k);
        if (it == g.m_linkIds.end()) {
            DepGraph::Link l;
            l.from = a;
            l.to = b;
            g.links.append(l);
            it = g.m_linkIds.insert(k, qint32(g.links.size() - 1));
        }
        DepGraph::Link &l = g.links[*it];
        if (include) {
            l.includes++;
        } else {
            l.count++;
            l.kinds[int(kind)]++;
            if (ref >= 0)
                l.refs.append(ref);
        }
    };

    if (level == Level::Class) {
        for (qint32 i = 0; i < ns; ++i) {
            const Symbol &s = m.symbols[i];
            if (!isType(s.kind) || s.kind == SymbolKind::Typedef || s.has(External)
                || (s.parent >= 0 && isType(m.symbols[s.parent].kind)))
                continue;
            const Location w = s.where();
            if (w.file < 0 || !inFilter(moduleOfFile[w.file]))
                continue;
            nodeForClass(i);
        }
        for (qint32 i = 0; i < m.refs.size(); ++i) {
            const Reference &r = m.refs[i];
            if (!countsAsDependency(r.kind) || r.from < 0 || r.to < 0)
                continue;
            const qint32 a = nodeForClass(r.from);
            if (a < 0)
                continue;
            addLink(a, nodeForClass(r.to), r.kind, false, i);
        }
        // Only what is in the filter, and what it links to
        for (qint32 n = 0; n < g.ids.size(); ++n) {
            if (g.ids[n] < 0) {
                g.size[n]++;   // a folded folder: count its classes
                continue;
            }
            const Symbol &cls = m.symbols[g.ids[n]];
            for (qint32 c : cls.children) {
                if (isFunction(m.symbols[c].kind))
                    g.complexity[n] += m.symbols[c].metrics.complexity;
            }
        }
    } else {
        for (qint32 f = 0; f < nf; ++f) {
            const File &file = m.files[f];
            if (!file.project || file.generated || !inFilter(moduleOfFile[f]))
                continue;
            const qint32 n = nodeForFile(f);
            if (n >= 0 && (level == Level::Module || !g.folder[n].isEmpty()))
                g.size[n] += file.codeLines;
        }
        for (qint32 f = 0; f < nf; ++f) {
            const File &file = m.files[f];
            if (!file.project || file.generated)
                continue;
            const qint32 a = nodeForFile(f);
            if (a < 0)
                continue;
            for (const Include &inc : file.includes)
                addLink(a, nodeForFile(inc.file), RefKind::Include, true, -1);
        }
        // References: from where the code is to where the used symbol is declared
        QVector<qint32> declFileOf(ns, -2);
        for (qint32 i = 0; !includesOnly && i < m.refs.size(); ++i) {
            const Reference &r = m.refs[i];
            if (!countsAsDependency(r.kind) || r.to < 0 || (r.flags & RefImplicit))
                continue;
            const qint32 a = nodeForFile(r.loc.file);
            if (a < 0)
                continue;
            qint32 &df = declFileOf[r.to];
            if (df == -2)
                df = declFile(m, r.to);
            addLink(a, nodeForFile(df), r.kind, false, i);
        }
        for (qint32 s = 0; s < ns; ++s) {
            const Symbol &sym = m.symbols[s];
            if (!isFunction(sym.kind) || !sym.def.valid() || sym.def.file >= nf)
                continue;
            const qint32 n = fileNode[sym.def.file];
            if (n >= 0)
                g.complexity[n] += sym.metrics.complexity;
        }
    }

    // With a filter: what is in it, and what that links to directly
    if (!filterModule.isEmpty()) {
        QVector<bool> keep(g.names.size(), false);
        for (qint32 n = 0; n < g.names.size(); ++n)
            keep[n] = nodeInFilter[n];
        for (const DepGraph::Link &l : std::as_const(g.links))
            keep[l.from] = keep[l.to] = true;
        QVector<qint32> map(g.names.size(), -1);
        DepGraph out;
        out.level = g.level;
        for (qint32 n = 0; n < g.names.size(); ++n) {
            if (!keep[n])
                continue;
            map[n] = qint32(out.names.size());
            out.names.append(g.names[n]);
            out.ids.append(g.ids[n]);
            out.size.append(g.size[n]);
            out.complexity.append(g.complexity[n]);
            out.folder.append(g.folder[n]);
        }
        for (DepGraph::Link l : std::as_const(g.links)) {
            l.from = map[l.from];
            l.to = map[l.to];
            out.m_linkIds.insert(DepGraph::key(l.from, l.to), qint32(out.links.size()));
            out.links.append(l);
        }
        g = std::move(out);
    }
    finishGraph(g);
    return g;
}

QVector<qint32> linkReferences(const CodeModel &, const DepGraph &g, qint32 link, int limit)
{
    if (link < 0 || link >= g.links.size())
        return {};
    return g.links[link].refs.mid(0, limit);
}

QVector<qint32> dependencyOrder(const DepGraph &g)
{
    // Condense the components, order them topologically (dependencies
    // first), and list each component's members together
    const qint32 n = qint32(g.names.size());
    const qint32 nc = qint32(g.componentSize.size());
    QVector<QSet<qint32>> cout(nc);
    QVector<qint32> indeg(nc, 0);
    for (const DepGraph::Link &l : g.links) {
        const qint32 a = g.component[l.from], b = g.component[l.to];
        if (a != b && !cout[b].contains(a)) {
            cout[b].insert(a);   // b must come before a: a depends on b
            indeg[a]++;
        }
    }
    QVector<qint32> order;
    QVector<qint32> ready;
    for (qint32 c = 0; c < nc; ++c) {
        if (indeg[c] == 0)
            ready.append(c);
    }
    QVector<QVector<qint32>> members(nc);
    for (qint32 i = 0; i < n; ++i)
        members[g.component[i]].append(i);
    auto byName = [&](qint32 x, qint32 y) { return g.names[x] < g.names[y]; };
    while (!ready.isEmpty()) {
        std::sort(ready.begin(), ready.end(), [&](qint32 x, qint32 y) {
            return g.names[members[x].first()] > g.names[members[y].first()];
        });
        const qint32 c = ready.takeLast();
        QVector<qint32> mem = members[c];
        std::sort(mem.begin(), mem.end(), byName);
        order += mem;
        for (qint32 d : std::as_const(cout[c])) {
            if (--indeg[d] == 0)
                ready.append(d);
        }
    }
    return order;
}

QVector<ModuleMetrics> moduleMetrics(const CodeModel &m, int moduleDepth)
{
    const DepGraph g = dependencyGraph(m, Level::Module, moduleDepth);
    QVector<ModuleMetrics> out(g.names.size());
    QHash<QString, qint32> byName;
    for (qint32 i = 0; i < g.names.size(); ++i) {
        out[i].name = g.names[i];
        out[i].lines = g.size[i];
        out[i].complexity = g.complexity[i];
        byName.insert(g.names[i], i);
    }
    for (const DepGraph::Link &l : g.links) {
        if (l.count + l.includes == 0)
            continue;
        out[l.from].efferent++;
        out[l.to].afferent++;
    }
    for (const File &f : m.files) {
        if (!f.project || f.generated)
            continue;
        const QString mod = moduleAt(f.module, moduleDepth);
        const qint32 i = byName.value(mod.isEmpty() ? QStringLiteral("(root)") : mod, -1);
        if (i >= 0)
            out[i].files++;
    }
    for (const Symbol &s : m.symbols) {
        if (s.has(External) || s.has(Generated) || s.has(FromMacro))
            continue;
        const Location w = s.where();
        if (w.file < 0 || !m.files[w.file].project)
            continue;
        const QString mod = moduleAt(m.files[w.file].module, moduleDepth);
        const qint32 i = byName.value(mod.isEmpty() ? QStringLiteral("(root)") : mod, -1);
        if (i < 0)
            continue;
        if ((s.kind == SymbolKind::Class || s.kind == SymbolKind::Struct) && s.has(Defined)) {
            out[i].classes++;
            if (s.has(Abstract))
                out[i].abstractClasses++;
        } else if (isFunction(s.kind) && s.has(Defined)) {
            out[i].functions++;
        }
    }
    for (ModuleMetrics &mm : out) {
        const qint32 total = mm.afferent + mm.efferent;
        mm.instability = total ? double(mm.efferent) / total : 0;
        mm.abstractness = mm.classes ? double(mm.abstractClasses) / mm.classes : 0;
        mm.distance = std::abs(mm.abstractness + mm.instability - 1);
    }
    return out;
}

ClassMetrics classMetrics(const CodeModel &m, qint32 cls)
{
    ClassMetrics c;
    c.symbol = cls;
    if (cls < 0 || cls >= m.symbols.size())
        return c;
    const Symbol &s = m.symbols[cls];
    QVector<qint32> methods, fields;
    for (qint32 ch : s.children) {
        const Symbol &x = m.symbols[ch];
        if (x.has(FromMacro) || x.has(Implicit))
            continue;
        if (isFunction(x.kind)) {
            methods.append(ch);
            c.methods++;
            if (x.access == Access::Public)
                c.publicMethods++;
            c.wmc += std::max(1, x.metrics.complexity);
            c.lines += x.metrics.lines;
        } else if (x.kind == SymbolKind::Field) {
            fields.append(ch);
            c.fields++;
        }
    }
    // Depth of inheritance, through project and other classes
    std::function<int(qint32, int)> depth = [&](qint32 k, int guard) -> int {
        if (guard > 32)
            return 0;
        int best = 0;
        for (qint32 e : m.outEdges[k]) {
            if (m.edges[e].kind == RefKind::Inherit)
                best = std::max(best, 1 + depth(m.edges[e].to, guard + 1));
        }
        return best;
    };
    c.depth = depth(cls, 0);
    for (qint32 e : m.inEdges[cls]) {
        if (m.edges[e].kind == RefKind::Inherit)
            c.children++;
    }
    QSet<qint32> uses, usedBy;
    auto collect = [&](qint32 member) {
        for (qint32 e : m.outEdges[member]) {
            const qint32 t = m.outermostType(m.nodeOwner(m.edges[e].to));
            if (t != cls && t >= 0 && isType(m.symbols[t].kind))
                uses.insert(t);
        }
        for (qint32 e : m.inEdges[member]) {
            const qint32 t = m.outermostType(m.nodeOwner(m.edges[e].from));
            if (t != cls && t >= 0 && isType(m.symbols[t].kind))
                usedBy.insert(t);
        }
    };
    collect(cls);
    for (qint32 ch : s.children)
        collect(ch);
    c.coupling = qint32(uses.size());
    c.usedBy = qint32(usedBy.size());
    // Cohesion: share of method pairs that use a field in common
    QVector<QSet<qint32>> fieldUse;
    for (qint32 mt : std::as_const(methods)) {
        QSet<qint32> f;
        for (qint32 e : m.outEdges[mt]) {
            if (m.edges[e].kind == RefKind::VarUse && fields.contains(m.edges[e].to))
                f.insert(m.edges[e].to);
        }
        fieldUse.append(f);
    }
    qint64 pairs = 0, connected = 0;
    for (qint32 i = 0; i < fieldUse.size(); ++i) {
        for (qint32 j = i + 1; j < fieldUse.size(); ++j) {
            ++pairs;
            if (fieldUse[i].intersects(fieldUse[j]))
                ++connected;
        }
    }
    c.cohesion = pairs ? double(connected) / pairs : 1.0;
    return c;
}

// --- Calls -----------------------------------------------------------------

QVector<CallSite> callsFrom(const CodeModel &m, qint32 fn, bool withImplicit)
{
    QVector<CallSite> out;
    if (fn < 0 || fn >= m.symbols.size())
        return out;
    QHash<qint32, qint32> seen;
    // The references give the order; collect each callee's first site
    struct Site {
        qint32 callee, ref;
    };
    QVector<Site> sites;
    for (qint32 ri : m.refsFrom[fn]) {
        const Reference &r = m.refs[ri];
        if (r.kind != RefKind::Call || r.to < 0 || (r.flags & RefAddressOf))
            continue;
        if (!withImplicit && (r.flags & RefImplicit))
            continue;
        sites.append({r.to, ri});
    }
    const auto &symbol = m.symbols[fn];
    const int definitionFile = symbol.def.valid() ? symbol.def.file : symbol.decl.file;
    std::stable_partition(sites.begin(), sites.end(), [&](const Site &site) {
        return m.refs[site.ref].loc.file == definitionFile;
    });
    for (const Site &s : std::as_const(sites)) {
        auto it = seen.find(s.callee);
        if (it != seen.end()) {
            auto &call = out[*it];
            ++call.count;
            call.implicit = call.implicit && bool(m.refs[s.ref].flags & RefImplicit);
            call.virtualCall = call.virtualCall || bool(m.refs[s.ref].flags & RefDynamic);
            continue;
        }
        CallSite c;
        c.callee = s.callee;
        c.ref = s.ref;
        c.implicit = m.refs[s.ref].flags & RefImplicit;
        c.virtualCall = m.refs[s.ref].flags & RefDynamic;
        seen.insert(s.callee, qint32(out.size()));
        out.append(c);
    }
    return out;
}

QVector<CallSite> callersOf(const CodeModel &m, qint32 fn, bool withImplicit)
{
    QVector<CallSite> out;
    if (fn < 0 || fn >= m.symbols.size())
        return out;
    QHash<qint32, qint32> seen;
    for (qint32 ref : m.refsTo[fn]) {
        const auto &reference = m.refs[ref];
        if (reference.kind != RefKind::Call || reference.from < 0 || (reference.flags & RefAddressOf))
            continue;
        if (!withImplicit && (reference.flags & RefImplicit))
            continue;
        if (seen.contains(reference.from)) {
            auto &call = out[seen.value(reference.from)];
            ++call.count;
            call.implicit = call.implicit && bool(reference.flags & RefImplicit);
            call.virtualCall = call.virtualCall || bool(reference.flags & RefDynamic);
            continue;
        }
        CallSite c;
        c.callee = reference.from;
        c.ref = ref;
        c.count = 1;
        c.implicit = reference.flags & RefImplicit;
        c.virtualCall = reference.flags & RefDynamic;
        seen.insert(c.callee, int(out.size()));
        out.append(c);
    }
    std::sort(out.begin(), out.end(), [&](const CallSite &a, const CallSite &b) {
        return m.displayName(a.callee) < m.displayName(b.callee);
    });
    return out;
}

QVector<qint32> overriders(const CodeModel &m, qint32 method)
{
    QVector<qint32> out;
    QVector<qint32> todo = {method};
    QSet<qint32> seen = {method};
    while (!todo.isEmpty()) {
        const qint32 cur = todo.takeLast();
        for (qint32 e : m.inEdges[cur]) {
            if (m.edges[e].kind != RefKind::Override)
                continue;
            const qint32 o = m.edges[e].from;
            if (!seen.contains(o)) {
                seen.insert(o);
                out.append(o);
                todo.append(o);
            }
        }
    }
    return out;
}

namespace {

// Calls and virtual dispatch from a function: (target, isDispatch)
QVector<QPair<qint32, bool>> successors(const CodeModel &m, qint32 fn)
{
    QVector<QPair<qint32, bool>> out;
    for (const auto &call : callsFrom(m, fn, true))
        out.append({call.callee, false});
    // A call to a virtual method may land in any override of it
    if (m.symbols[fn].has(Virtual)) {
        for (qint32 e : m.inEdges[fn]) {
            if (m.edges[e].kind == RefKind::Override)
                out.append({m.edges[e].from, true});
        }
    }
    return out;
}

QVector<QPair<qint32, bool>> predecessors(const CodeModel &m, qint32 fn)
{
    QVector<QPair<qint32, bool>> out;
    for (const auto &call : callersOf(m, fn, true))
        out.append({call.callee, false});
    for (qint32 e : m.outEdges[fn]) {
        if (m.edges[e].kind == RefKind::Override)
            out.append({m.edges[e].to, true});
    }
    return out;
}

QHash<qint32, int> distances(const CodeModel &m, qint32 start, int maxDepth, bool forward)
{
    QHash<qint32, int> dist;
    dist.insert(start, 0);
    QQueue<qint32> q;
    q.enqueue(start);
    while (!q.isEmpty()) {
        const qint32 v = q.dequeue();
        const int d = dist.value(v);
        if (d >= maxDepth)
            continue;
        const auto next = forward ? successors(m, v) : predecessors(m, v);
        for (const auto &[w, dispatch] : next) {
            Q_UNUSED(dispatch)
            if (!dist.contains(w)) {
                dist.insert(w, d + 1);
                q.enqueue(w);
            }
        }
    }
    return dist;
}

} // namespace

Subgraph callPaths(const CodeModel &m, qint32 from, qint32 to, int maxDepth)
{
    Subgraph g;
    if (from < 0 || to < 0)
        return g;
    const QHash<qint32, int> da = distances(m, from, maxDepth, true);
    if (!da.contains(to))
        return g;
    const QHash<qint32, int> db = distances(m, to, maxDepth, false);
    const int limit = std::min(maxDepth, da.value(to) + 2);   // shortest paths and a little longer
    QSet<qint32> nodes;
    for (auto it = da.cbegin(); it != da.cend(); ++it) {
        const auto jt = db.constFind(it.key());
        if (jt != db.cend() && it.value() + *jt <= limit)
            nodes.insert(it.key());
    }
    for (qint32 v : std::as_const(nodes)) {
        for (const auto &[w, dispatch] : successors(m, v)) {
            if (nodes.contains(w) && da.value(v) + 1 + db.value(w) <= limit) {
                g.edges.append({v, w});
                g.dispatch.append(dispatch);
            }
        }
    }
    g.nodes = QVector<qint32>(nodes.cbegin(), nodes.cend());
    std::sort(g.nodes.begin(), g.nodes.end(), [&](qint32 a, qint32 b) { return da.value(a) < da.value(b); });
    return g;
}

Subgraph callNeighbourhood(const CodeModel &m, qint32 root, int depth, bool callees, bool callers, int maxNodes)
{
    if (root < 0)
        return {};
    return callNeighbourhood(m, QVector<qint32>{root}, depth, callees, callers, maxNodes);
}

QVector<qint32> methodsOf(const CodeModel &m, qint32 type)
{
    QVector<qint32> out;
    if (type < 0 || type >= m.symbols.size())
        return out;
    for (qint32 ch : m.symbols[type].children) {
        const Symbol &s = m.symbols[ch];
        if (isFunction(s.kind) && !s.has(Implicit) && !s.has(FromMacro))
            out << ch;
    }
    std::sort(out.begin(), out.end(), [&](qint32 a, qint32 b) {
        const Location la = m.symbols[a].where(), lb = m.symbols[b].where();
        return la.file != lb.file ? la.file < lb.file : la.line < lb.line;
    });
    return out;
}

Subgraph callNeighbourhood(const CodeModel &m, const QVector<qint32> &roots, int depth, bool callees, bool callers,
                           int maxNodes)
{
    Subgraph g;
    if (roots.isEmpty())
        return g;
    QSet<qint32> nodes(roots.cbegin(), roots.cend());
    QSet<quint64> edgeSet;
    auto addEdge = [&](qint32 a, qint32 b, bool dispatch) {
        const quint64 k = (quint64(quint32(a)) << 32) | quint32(b);
        if (edgeSet.contains(k))
            return;
        edgeSet.insert(k);
        g.edges.append({a, b});
        g.dispatch.append(dispatch);
    };
    auto walk = [&](bool forward) {
        QVector<qint32> frontier = roots;
        for (int d = 0; d < depth && !frontier.isEmpty(); ++d) {
            QVector<qint32> next;
            for (qint32 v : std::as_const(frontier)) {
                const auto adj = forward ? successors(m, v) : predecessors(m, v);
                for (const auto &[w, dispatch] : adj) {
                    if (!nodes.contains(w)) {
                        if (nodes.size() >= maxNodes)
                            continue;
                        nodes.insert(w);
                        next.append(w);
                    }
                    if (forward)
                        addEdge(v, w, dispatch);
                    else
                        addEdge(w, v, dispatch);
                }
            }
            frontier = next;
        }
    };
    if (callees)
        walk(true);
    if (callers)
        walk(false);
    g.nodes = QVector<qint32>(nodes.cbegin(), nodes.cend());
    std::sort(g.nodes.begin(), g.nodes.end());
    return g;
}

// --- Findings --------------------------------------------------------------

QString findingKindName(FindingKind k)
{
    switch (k) {
    case FindingKind::ParseErrors: return QStringLiteral("Parse errors");
    case FindingKind::IncludeCycle: return QStringLiteral("Include and import cycles");
    case FindingKind::ModuleCycle: return QStringLiteral("Module cycles");
    case FindingKind::ClassCycle: return QStringLiteral("Class dependency cycles");
    case FindingKind::Recursion: return QStringLiteral("Recursion");
    case FindingKind::Complexity: return QStringLiteral("Complex functions");
    case FindingKind::LongFunction: return QStringLiteral("Long functions");
    case FindingKind::DeepNesting: return QStringLiteral("Deep nesting");
    case FindingKind::ManyParameters: return QStringLiteral("Many parameters");
    case FindingKind::GodClass: return QStringLiteral("Large classes");
    case FindingKind::Duplicate: return QStringLiteral("Duplicated code");
    case FindingKind::Unused: return QStringLiteral("Possibly unused");
    case FindingKind::UnreadField: return QStringLiteral("Written, never read");
    case FindingKind::GlobalState: return QStringLiteral("Mutable global state");
    case FindingKind::DeepInheritance: return QStringLiteral("Deep inheritance");
    case FindingKind::NonVirtualDestructor: return QStringLiteral("Non-virtual destructors");
    case FindingKind::LargeFile: return QStringLiteral("Large files");
    case FindingKind::OrphanFile: return QStringLiteral("Files nothing builds");
    case FindingKind::Count: break;
    }
    return QString();
}

QString findingKindHint(FindingKind k)
{
    switch (k) {
    case FindingKind::ParseErrors:
        return QStringLiteral("clang could not compile these files cleanly, so what is shown about them may be incomplete. "
                              "A compile_commands.json from the real build fixes most of them.");
    case FindingKind::IncludeCycle:
        return QStringLiteral("Headers that include each other, or Python modules that import each other, directly or "
                              "around a loop: they can't be understood, built or reused one without the other.");
    case FindingKind::ModuleCycle:
        return QStringLiteral("Folders that depend on each other: the layering is broken there.");
    case FindingKind::ClassCycle:
        return QStringLiteral("Classes that use each other around a loop: tightly coupled, hard to test in isolation.");
    case FindingKind::Recursion:
        return QStringLiteral("Functions that call themselves, directly or through others. Intended recursion is fine; "
                              "accidental recursion is a classic generated-code bug.");
    case FindingKind::Complexity:
        return QStringLiteral("Many decisions (McCabe complexity) or hard to follow (cognitive complexity): "
                              "where bugs hide and tests miss paths.");
    case FindingKind::LongFunction:
        return QStringLiteral("Long functions doing many things; candidates for splitting.");
    case FindingKind::DeepNesting:
        return QStringLiteral("Blocks nested deeply: early returns or extracted functions read better.");
    case FindingKind::ManyParameters:
        return QStringLiteral("Long parameter lists: easy to call wrongly; a struct often fits better.");
    case FindingKind::GodClass:
        return QStringLiteral("Classes with many methods, much logic or many dependencies: they do too much.");
    case FindingKind::Duplicate:
        return QStringLiteral("Functions with the same or nearly the same token structure. Generated code often "
                              "repeats a helper instead of reusing it.");
    case FindingKind::Unused:
        return QStringLiteral("Nothing in the indexed code refers to these. They may be dead, or used from outside "
                              "(QML, scripts, other programs, the meta-object system): check before deleting.");
    case FindingKind::UnreadField:
        return QStringLiteral("Fields and variables that are assigned but never read.");
    case FindingKind::GlobalState:
        return QStringLiteral("Non-const globals and static members: hidden coupling, not thread-safe by default.");
    case FindingKind::DeepInheritance:
        return QStringLiteral("Long inheritance chains: behaviour is spread over many classes.");
    case FindingKind::NonVirtualDestructor:
        return QStringLiteral("C++ base classes with virtual methods whose destructor is not virtual: deleting a "
                              "subclass through a base pointer is undefined behaviour.");
    case FindingKind::LargeFile:
        return QStringLiteral("Very large source files.");
    case FindingKind::OrphanFile:
        return QStringLiteral("Sources no translation unit compiles and headers nothing includes: leftovers or "
                              "missing from the build.");
    case FindingKind::Count: break;
    }
    return QString();
}

bool looksUnused(const CodeModel &m, qint32 i)
{
    const Symbol &s = m.symbols[i];
    if (s.has(External) || s.has(Generated) || s.has(FromMacro) || s.has(Implicit) || s.has(EntryPoint)
        || s.has(Deleted) || s.has(Defaulted) || s.has(MetaCallable))
        return false;
    if (!s.has(Defined))
        return false;
    const Location w = s.where();
    if (w.file < 0 || !m.files[w.file].project || m.files[w.file].generated)
        return false;
    switch (s.kind) {
    case SymbolKind::Function:
    case SymbolKind::Method:
        break;
    case SymbolKind::Class:
    case SymbolKind::Struct:
    case SymbolKind::Enum:
    case SymbolKind::Typedef:
    case SymbolKind::Macro:
    case SymbolKind::Variable:
        if (s.parent >= 0 && isFunction(m.symbols[s.parent].kind))
            return false;
        break;
    default:
        return false;   // constructors, operators, fields (see UnreadField)…
    }
    if (s.has(Virtual))
        return false;   // called through a base, or overriding one
    if (s.usr.startsWith(QLatin1String("py:"))) {
        // Python calls these itself, or tools do: __dunder__ names, tests,
        // what a module exports in __all__
        if (s.name.startsWith(QLatin1String("__")) && s.name.endsWith(QLatin1String("__")))
            return false;
        const QString file = w.file >= 0 ? QFileInfo(m.files[w.file].path).fileName() : QString();
        if ((file.startsWith(QLatin1String("test_")) || file.endsWith(QLatin1String("_test.py")) || file == QLatin1String("conftest.py"))
            && (s.name.startsWith(QLatin1String("test")) || s.name.startsWith(QLatin1String("Test")) || s.kind == SymbolKind::Function))
            return false;
        if (s.name == QLatin1String("main") || s.name == QLatin1String("setup"))
            return false;
    }
    if (s.name.startsWith(QLatin1Char('~')) || s.name.startsWith(QLatin1String("operator")))
        return false;
    // Include guards and macros used only by #if/#ifdef aren't seen as uses
    if (s.kind == SymbolKind::Macro && (s.name.endsWith(QLatin1String("_H")) || s.name.endsWith(QLatin1String("_H_"))
                                        || s.name.endsWith(QLatin1String("_HPP")) || s.name.endsWith(QLatin1String("_INCLUDED"))))
        return false;
    const qint32 ownType = isType(s.kind) ? m.outermostType(i) : -1;
    if (s.usr.startsWith(QLatin1String("py:")) && (s.kind == SymbolKind::Variable)) {
        for (qint32 r : m.refsTo[i]) {
            if (m.refs[r].flags & RefRead)
                return false;
        }
        return true;
    }
    for (qint32 r : m.refsTo[i]) {
        const Reference &ref = m.refs[r];
        if (ref.from == i)
            continue;
        // A class named by its own members (Foo::bar() { … }, Foo(const Foo &)) isn't used yet
        if (ownType >= 0 && ref.from >= 0 && m.outermostType(m.nodeOwner(ref.from)) == ownType)
            continue;
        return false;   // used by someone else
    }
    // An enum is used when one of its enumerators is
    if (s.kind == SymbolKind::Enum) {
        for (qint32 c : s.children) {
            if (!m.refsTo[c].isEmpty())
                return false;
        }
    }
    // A type is used when one of its members is used from outside
    if (isType(s.kind)) {
        for (qint32 c : s.children) {
            for (qint32 r : m.refsTo[c]) {
                const qint32 from = m.refs[r].from;
                if (from < 0 || m.outermostType(m.nodeOwner(from)) != m.outermostType(i))
                    return false;
            }
        }
    }
    return true;
}

namespace {

QString symbolTitle(const CodeModel &m, qint32 s)
{
    return m.displayName(s);
}

bool reviewable(const CodeModel &m, const Symbol &s)
{
    if (s.has(External) || s.has(Generated) || s.has(FromMacro) || s.has(Implicit))
        return false;
    const Location w = s.where();
    return w.file >= 0 && m.files[w.file].project && !m.files[w.file].generated;
}

void cycles(const CodeModel &m, Report &rep)
{
    // Includes between project files
    {
        const qint32 nf = qint32(m.files.size());
        QVector<QVector<qint32>> adj(nf);
        for (qint32 f = 0; f < nf; ++f) {
            if (!m.files[f].project)
                continue;
            for (const Include &inc : m.files[f].includes) {
                if (inc.file >= 0 && m.files[inc.file].project)
                    adj[f].append(inc.file);
            }
        }
        QVector<qint32> sizes;
        const QVector<qint32> comp = components(nf, adj, &sizes);
        QHash<qint32, QVector<qint32>> groups;
        for (qint32 f = 0; f < nf; ++f) {
            if (sizes[comp[f]] > 1)
                groups[comp[f]].append(f);
        }
        for (const QVector<qint32> &files : std::as_const(groups)) {
            Finding fd;
            fd.kind = FindingKind::IncludeCycle;
            fd.severity = 1;
            fd.files = files;
            QStringList names;
            for (qint32 f : files)
                names << m.files[f].relative;
            names.sort();
            const bool python = std::all_of(files.cbegin(), files.cend(), [&](qint32 f) {
                return m.files[f].path.endsWith(QLatin1String(".py"));
            });
            fd.title = python ? QStringLiteral("%1 modules import each other").arg(files.size())
                              : QStringLiteral("%1 files include each other").arg(files.size());
            fd.detail = names.join(QStringLiteral(" ↔ "));
            fd.loc.file = files.first();
            fd.loc.line = 1;
            fd.score = files.size();
            rep.findings.append(fd);
        }
    }
    // Modules
    {
        const DepGraph g = dependencyGraph(m, Level::Module);
        QHash<qint32, QVector<qint32>> groups;
        for (qint32 i = 0; i < g.names.size(); ++i) {
            if (g.componentSize[g.component[i]] > 1)
                groups[g.component[i]].append(i);
        }
        for (const QVector<qint32> &nodes : std::as_const(groups)) {
            Finding fd;
            fd.kind = FindingKind::ModuleCycle;
            fd.severity = 1;
            QStringList names;
            for (qint32 n : nodes)
                names << g.names[n];
            names.sort();
            fd.title = QStringLiteral("%1 folders depend on each other").arg(nodes.size());
            fd.detail = names.join(QStringLiteral(" ↔ "));
            fd.score = nodes.size();
            rep.findings.append(fd);
        }
    }
    // Classes
    {
        const DepGraph g = dependencyGraph(m, Level::Class);
        QHash<qint32, QVector<qint32>> groups;
        for (qint32 i = 0; i < g.names.size(); ++i) {
            if (g.componentSize[g.component[i]] > 1)
                groups[g.component[i]].append(i);
        }
        for (const QVector<qint32> &nodes : std::as_const(groups)) {
            Finding fd;
            fd.kind = FindingKind::ClassCycle;
            fd.severity = nodes.size() > 4 ? 1 : 0;
            QStringList names;
            for (qint32 n : nodes) {
                names << g.names[n];
                fd.symbols.append(g.ids[n]);
            }
            names.sort();
            fd.title = QStringLiteral("%1 classes depend on each other").arg(nodes.size());
            fd.detail = names.mid(0, 12).join(QStringLiteral(" ↔ ")) + (names.size() > 12 ? QStringLiteral(" …") : QString());
            fd.loc = m.symbols[fd.symbols.first()].where();
            fd.score = nodes.size();
            rep.findings.append(fd);
        }
    }
    // Recursion in the call graph
    {
        const qint32 ns = qint32(m.symbols.size());
        QVector<QVector<qint32>> adj(ns);
        QVector<bool> self(ns, false);
        for (const auto &reference : m.refs) {
            if (reference.kind != RefKind::Call || (reference.flags & (RefImplicit | RefAddressOf))
                || reference.from < 0 || reference.to < 0)
                continue;
            if (!reviewable(m, m.symbols[reference.from]) || !reviewable(m, m.symbols[reference.to]))
                continue;
            if (reference.from == reference.to)
                self[reference.from] = true;
            else
                adj[reference.from].append(reference.to);
        }
        QVector<qint32> sizes;
        const QVector<qint32> comp = components(ns, adj, &sizes);
        QHash<qint32, QVector<qint32>> groups;
        for (qint32 s = 0; s < ns; ++s) {
            if (sizes[comp[s]] > 1)
                groups[comp[s]].append(s);
            else if (self[s])
                groups[-1 - s].append(s);
        }
        for (const QVector<qint32> &fns : std::as_const(groups)) {
            Finding fd;
            fd.kind = FindingKind::Recursion;
            fd.severity = 0;
            fd.symbols = fns;
            if (fns.size() == 1) {
                fd.title = symbolTitle(m, fns.first());
                fd.detail = QStringLiteral("calls itself");
            } else {
                QStringList names;
                for (qint32 f : fns)
                    names << m.symbols[f].name;
                fd.title = QStringLiteral("%1 functions call each other").arg(fns.size());
                fd.detail = names.mid(0, 10).join(QStringLiteral(" → ")) + (names.size() > 10 ? QStringLiteral(" …") : QString());
            }
            fd.loc = m.symbols[fns.first()].where();
            fd.score = fns.size();
            rep.findings.append(fd);
        }
    }
}

void functionFindings(const CodeModel &m, const Thresholds &t, Report &rep)
{
    for (qint32 i = 0; i < m.symbols.size(); ++i) {
        const Symbol &s = m.symbols[i];
        if (!isFunction(s.kind) || !s.def.valid() || !reviewable(m, s))
            continue;
        const FunctionMetrics &mm = s.metrics;
        if (mm.complexity >= t.complexity || mm.cognitive >= t.cognitive) {
            Finding f;
            f.kind = FindingKind::Complexity;
            f.severity = (mm.complexity >= t.complexity * 2 || mm.cognitive >= t.cognitive * 2) ? 2 : 1;
            f.symbols = {i};
            f.title = symbolTitle(m, i);
            f.detail = QStringLiteral("complexity %1, cognitive %2, %3 lines").arg(mm.complexity).arg(mm.cognitive).arg(mm.lines);
            f.loc = s.def;
            f.score = mm.complexity + mm.cognitive / 2.0;
            rep.findings.append(f);
        }
        if (mm.lines >= t.lines) {
            Finding f;
            f.kind = FindingKind::LongFunction;
            f.severity = mm.lines >= t.lines * 2 ? 2 : 1;
            f.symbols = {i};
            f.title = symbolTitle(m, i);
            f.detail = QStringLiteral("%1 lines, %2 statements").arg(mm.lines).arg(mm.statements);
            f.loc = s.def;
            f.score = mm.lines;
            rep.findings.append(f);
        }
        if (mm.nesting >= t.nesting) {
            Finding f;
            f.kind = FindingKind::DeepNesting;
            f.severity = mm.nesting >= t.nesting + 2 ? 2 : 1;
            f.symbols = {i};
            f.title = symbolTitle(m, i);
            f.detail = QStringLiteral("nested %1 levels deep").arg(mm.nesting);
            f.loc = s.def;
            f.score = mm.nesting;
            rep.findings.append(f);
        }
        if (mm.params >= t.params) {
            Finding f;
            f.kind = FindingKind::ManyParameters;
            f.severity = mm.params >= t.params + 3 ? 2 : 1;
            f.symbols = {i};
            f.title = symbolTitle(m, i);
            f.detail = QStringLiteral("%1 parameters").arg(mm.params);
            f.loc = s.def;
            f.score = mm.params;
            rep.findings.append(f);
        }
    }
}

void classFindings(const CodeModel &m, const Thresholds &t, Report &rep)
{
    for (qint32 i = 0; i < m.symbols.size(); ++i) {
        const Symbol &s = m.symbols[i];
        if ((s.kind != SymbolKind::Class && s.kind != SymbolKind::Struct) || !s.has(Defined) || !reviewable(m, s))
            continue;
        const ClassMetrics c = classMetrics(m, i);
        if (c.methods >= t.classMethods || c.wmc >= t.classWmc || c.coupling >= t.classCoupling) {
            Finding f;
            f.kind = FindingKind::GodClass;
            f.severity = (c.wmc >= t.classWmc * 2 || c.methods >= t.classMethods * 2) ? 2 : 1;
            f.symbols = {i};
            f.title = symbolTitle(m, i);
            f.detail = QStringLiteral("%1 methods, %2 fields, weighted complexity %3, uses %4 classes, cohesion %5%")
                           .arg(c.methods).arg(c.fields).arg(c.wmc).arg(c.coupling)
                           .arg(qRound(c.cohesion * 100));
            f.loc = s.where();
            f.score = c.wmc + c.methods + c.coupling;
            rep.findings.append(f);
        }
        if (c.depth >= t.inheritance) {
            Finding f;
            f.kind = FindingKind::DeepInheritance;
            f.severity = 0;
            f.symbols = {i};
            f.title = symbolTitle(m, i);
            f.detail = QStringLiteral("%1 levels of base classes").arg(c.depth);
            f.loc = s.where();
            f.score = c.depth;
            rep.findings.append(f);
        }
        // A polymorphic base the project derives from, without a virtual destructor
        bool hasVirtual = false;
        qint32 dtor = -1;
        for (qint32 ch : s.children) {
            const Symbol &x = m.symbols[ch];
            if (x.kind == SymbolKind::Destructor)
                dtor = ch;
            else if (isFunction(x.kind) && x.has(Virtual))
                hasVirtual = true;
        }
        const bool hidden = dtor >= 0 && (m.symbols[dtor].access == Access::Protected || m.symbols[dtor].access == Access::Private);
        const bool python = s.usr.startsWith(QLatin1String("py:"));
        if (!python && hasVirtual && c.children > 0 && !hidden && (dtor < 0 || !m.symbols[dtor].has(Virtual))) {
            // A base of its own with a virtual destructor makes it virtual
            bool inherited = false;
            for (qint32 e : m.outEdges[i]) {
                if (m.edges[e].kind == RefKind::Inherit)
                    inherited = true;   // conservatively assume the base takes care of it
            }
            if (!inherited) {
                Finding f;
                f.kind = FindingKind::NonVirtualDestructor;
                f.severity = 2;
                f.symbols = {i};
                f.title = symbolTitle(m, i);
                f.detail = dtor < 0 ? QStringLiteral("has virtual methods and %1 subclasses, but no destructor").arg(c.children)
                                    : QStringLiteral("has virtual methods and %1 subclasses, its destructor isn't virtual").arg(c.children);
                f.loc = s.where();
                f.score = c.children;
                rep.findings.append(f);
            }
        }
    }
}

void usageFindings(const CodeModel &m, Report &rep)
{
    for (qint32 i = 0; i < m.symbols.size(); ++i) {
        const Symbol &s = m.symbols[i];
        if (looksUnused(m, i)) {
            // Methods of a class with a meta-object may be called through it
            bool metaObject = false;
            if (s.kind == SymbolKind::Method && s.parent >= 0) {
                for (qint32 c : m.symbols[s.parent].children) {
                    if (m.symbols[c].name == QLatin1String("qt_metacall") || m.symbols[c].name == QLatin1String("staticMetaObject")) {
                        metaObject = true;
                        break;
                    }
                }
            }
            Finding f;
            f.kind = FindingKind::Unused;
            f.severity = metaObject || s.kind == SymbolKind::Macro ? 0 : 1;
            f.symbols = {i};
            f.title = symbolTitle(m, i);
            f.detail = kindName(s.kind) + (metaObject ? QStringLiteral(" in a QObject: may be called from QML or by a signal")
                                         : s.kind == SymbolKind::Macro ? QStringLiteral(", expanded nowhere (#if and #ifdef tests aren't counted)")
                                                                       : QStringLiteral(", referenced nowhere"));
            f.loc = s.where();
            f.score = std::max(1, s.metrics.lines);
            rep.findings.append(f);
            continue;
        }
        const bool pythonDunder = s.usr.startsWith(QLatin1String("py:")) && s.name.startsWith(QLatin1String("__"));
        const bool pythonVariable = s.usr.startsWith(QLatin1String("py:")) && s.kind == SymbolKind::Variable;
        if ((s.kind == SymbolKind::Field || s.kind == SymbolKind::Variable) && reviewable(m, s) && !s.has(MetaCallable)
            && !pythonDunder) {
            if (s.parent >= 0 && isFunction(m.symbols[s.parent].kind))
                continue;
            int reads = 0, writes = 0;
            for (qint32 r : m.refsTo[i]) {
                const quint8 fl = m.refs[r].flags;
                if (fl & RefRead)
                    reads++;
                if (fl & RefWrite)
                    writes++;
            }
            if (writes > 0 && reads == 0 && !pythonVariable) {   // a Python variable: reported as unused
                Finding f;
                f.kind = FindingKind::UnreadField;
                f.severity = 1;
                f.symbols = {i};
                f.title = symbolTitle(m, i);
                f.detail = QStringLiteral("written %1 times, never read").arg(writes);
                f.loc = s.where();
                f.score = writes;
                rep.findings.append(f);
            }
            if (s.kind == SymbolKind::Variable && !s.has(Const) && !s.type.startsWith(QLatin1String("const "))
                && !(pythonVariable && m.refsTo[i].size() <= 1)   // a Python name assigned once: a constant in all but name
                && !s.type.contains(QLatin1String("constexpr")) && !s.type.contains(QLatin1String("atomic"))) {
                QSet<qint32> writers;
                for (qint32 r : m.refsTo[i]) {
                    if (m.refs[r].flags & RefWrite)
                        writers.insert(m.refs[r].from);
                }
                Finding f;
                f.kind = FindingKind::GlobalState;
                f.severity = writers.size() > 1 ? 1 : 0;
                f.symbols = {i};
                f.title = symbolTitle(m, i);
                f.detail = QStringLiteral("%1%2, written by %3 functions, used %4 times")
                               .arg(s.has(Static) ? QStringLiteral("static") : QStringLiteral("global"),
                                    s.type.isEmpty() ? QString() : QStringLiteral(" ") + s.type)
                               .arg(writers.size()).arg(m.refsTo[i].size());
                f.loc = s.where();
                f.score = writers.size() * 10 + m.refsTo[i].size();
                rep.findings.append(f);
            }
        }
    }
}

void duplicateFindings(const CodeModel &m, const Thresholds &t, Report &rep)
{
    // Exact structure: same token sequence with names and literals made alike
    QHash<quint64, QVector<qint32>> exact;
    QVector<qint32> candidates;
    for (qint32 i = 0; i < m.symbols.size(); ++i) {
        const Symbol &s = m.symbols[i];
        if (!isFunction(s.kind) || !reviewable(m, s) || s.metrics.tokens < t.cloneTokens)
            continue;
        exact[s.metrics.cloneHash].append(i);
        if (s.metrics.minHash.size() == 32)
            candidates.append(i);
    }
    // Union-find over clone pairs
    QHash<qint32, qint32> parent;
    std::function<qint32(qint32)> find = [&](qint32 x) -> qint32 {
        auto it = parent.find(x);
        if (it == parent.end()) {
            parent.insert(x, x);
            return x;
        }
        if (*it == x)
            return x;
        const qint32 r = find(*it);
        parent[x] = r;
        return r;
    };
    QHash<quint64, double> pairSimilarity;
    auto unite = [&](qint32 a, qint32 b, double sim) {
        const qint32 ra = find(a), rb = find(b);
        if (ra != rb)
            parent[ra] = rb;
        const quint64 k = (quint64(quint32(std::min(a, b))) << 32) | quint32(std::max(a, b));
        pairSimilarity[k] = std::max(pairSimilarity.value(k), sim);
    };
    for (const QVector<qint32> &group : std::as_const(exact)) {
        for (qint32 i = 1; i < group.size(); ++i)
            unite(group[0], group[i], 1.0);
    }
    // Near: locality-sensitive hashing over the min-hash signature,
    // 8 bands of 4, then the estimated similarity of each candidate pair
    for (int band = 0; band < 8; ++band) {
        QHash<quint64, QVector<qint32>> buckets;
        for (qint32 s : std::as_const(candidates)) {
            const QVector<quint32> &h = m.symbols[s].metrics.minHash;
            quint64 key = band;
            for (int r = 0; r < 4; ++r)
                key = key * 1099511628211ULL ^ h[band * 4 + r];
            buckets[key].append(s);
        }
        for (const QVector<qint32> &b : std::as_const(buckets)) {
            if (b.size() < 2 || b.size() > 50)
                continue;
            for (qint32 x = 0; x < b.size(); ++x) {
                for (qint32 y = x + 1; y < b.size(); ++y) {
                    const QVector<quint32> &ha = m.symbols[b[x]].metrics.minHash;
                    const QVector<quint32> &hb = m.symbols[b[y]].metrics.minHash;
                    int same = 0;
                    for (int k = 0; k < 32; ++k)
                        same += ha[k] == hb[k];
                    const double sim = same / 32.0;
                    // Sizes must be alike too
                    const double ta = m.symbols[b[x]].metrics.tokens, tb = m.symbols[b[y]].metrics.tokens;
                    if (sim >= t.cloneSimilarity && std::min(ta, tb) / std::max(ta, tb) > 0.6)
                        unite(b[x], b[y], sim);
                }
            }
        }
    }
    QHash<qint32, QVector<qint32>> groups;
    for (auto it = parent.cbegin(); it != parent.cend(); ++it)
        groups[find(it.key())].append(it.key());
    for (QVector<qint32> &g : groups) {
        if (g.size() < 2)
            continue;
        // The same definition seen twice (a template, a header) is no clone
        std::sort(g.begin(), g.end(), [&](qint32 a, qint32 b) {
            return std::tie(m.symbols[a].def.file, m.symbols[a].def.line)
                 < std::tie(m.symbols[b].def.file, m.symbols[b].def.line);
        });
        double minSim = 1.0;
        qint32 tokens = 0;
        for (qint32 i = 0; i < g.size(); ++i) {
            tokens += m.symbols[g[i]].metrics.tokens;
            for (qint32 j = i + 1; j < g.size(); ++j) {
                const quint64 k = (quint64(quint32(std::min(g[i], g[j]))) << 32) | quint32(std::max(g[i], g[j]));
                const auto it = pairSimilarity.constFind(k);
                if (it != pairSimilarity.cend())
                    minSim = std::min(minSim, *it);
            }
        }
        Finding f;
        f.kind = FindingKind::Duplicate;
        f.symbols = g;
        const bool same = minSim >= 0.999;
        f.severity = same ? 2 : 1;
        QStringList names;
        for (qint32 s : g)
            names << m.symbols[s].name;
        names.removeDuplicates();
        f.title = QStringLiteral("%1 functions alike: %2").arg(g.size()).arg(names.mid(0, 4).join(QStringLiteral(", "))
                                                                             + (names.size() > 4 ? QStringLiteral("…") : QString()));
        f.detail = same ? QStringLiteral("identical structure, %1 tokens each").arg(tokens / g.size())
                        : QStringLiteral("about %1% alike, %2 tokens each").arg(qRound(minSim * 100)).arg(tokens / g.size());
        f.loc = m.symbols[g.first()].def;
        f.score = tokens * minSim;
        rep.findings.append(f);
    }
}

void fileFindings(const CodeModel &m, const Thresholds &t, Report &rep)
{
    QHash<qint32, QVector<qint32>> errorsByFile;
    for (qint32 i = 0; i < m.diagnostics.size(); ++i) {
        const Diagnostic &d = m.diagnostics[i];
        if (d.severity >= Diagnostic::Error)
            errorsByFile[d.loc.file >= 0 ? d.loc.file : d.unit].append(i);
    }
    for (auto it = errorsByFile.cbegin(); it != errorsByFile.cend(); ++it) {
        Finding f;
        f.kind = FindingKind::ParseErrors;
        f.severity = 2;
        const qint32 file = it.key();
        if (file >= 0)
            f.files = {file};
        const Diagnostic &first = m.diagnostics[it.value().first()];
        f.title = file >= 0 ? m.files[file].relative : QStringLiteral("(unknown file)");
        f.detail = QStringLiteral("%1 errors, first: %2").arg(it.value().size()).arg(first.message);
        f.loc = first.loc;
        f.score = it.value().size();
        rep.findings.append(f);
    }
    for (qint32 i = 0; i < m.files.size(); ++i) {
        const File &file = m.files[i];
        if (!file.project || file.generated)
            continue;
        if (file.lines >= t.fileLines) {
            Finding f;
            f.kind = FindingKind::LargeFile;
            f.severity = file.lines >= t.fileLines * 2 ? 1 : 0;
            f.files = {i};
            f.title = file.relative;
            f.detail = QStringLiteral("%1 lines (%2 of code), %3 symbols").arg(file.lines).arg(file.codeLines).arg(file.symbols.size());
            f.loc.file = i;
            f.loc.line = 1;
            f.score = file.lines;
            rep.findings.append(f);
        }
        const bool source = !file.header;
        if (!file.indexed && file.includedBy.isEmpty()) {
            Finding f;
            f.kind = FindingKind::OrphanFile;
            f.severity = 0;
            f.files = {i};
            f.title = file.relative;
            f.detail = source ? QStringLiteral("a source no translation unit compiles")
                              : QStringLiteral("a header nothing includes");
            f.loc.file = i;
            f.loc.line = 1;
            f.score = file.lines;
            rep.findings.append(f);
        }
    }
}

} // namespace

Report analyze(const CodeModel &m, const Thresholds &t)
{
    Report rep;
    fileFindings(m, t, rep);
    cycles(m, rep);
    functionFindings(m, t, rep);
    classFindings(m, t, rep);
    usageFindings(m, rep);
    duplicateFindings(m, t, rep);
    std::stable_sort(rep.findings.begin(), rep.findings.end(), [](const Finding &a, const Finding &b) {
        if (a.kind != b.kind)
            return int(a.kind) < int(b.kind);
        if (a.severity != b.severity)
            return a.severity > b.severity;
        return a.score > b.score;
    });
    for (const Finding &f : std::as_const(rep.findings)) {
        rep.perKind[int(f.kind)]++;
        if (f.severity >= 2)
            rep.serious++;
        else if (f.severity == 1)
            rep.warnings++;
        else
            rep.notes++;
    }
    return rep;
}

} // namespace Unravel::Analysis
