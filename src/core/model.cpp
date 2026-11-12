#include "model.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <functional>
#include <tuple>

namespace Unravel {

QString kindName(SymbolKind k)
{
    switch (k) {
    case SymbolKind::Namespace: return QStringLiteral("namespace");
    case SymbolKind::Class: return QStringLiteral("class");
    case SymbolKind::Struct: return QStringLiteral("struct");
    case SymbolKind::Union: return QStringLiteral("union");
    case SymbolKind::Enum: return QStringLiteral("enum");
    case SymbolKind::EnumConstant: return QStringLiteral("enumerator");
    case SymbolKind::Typedef: return QStringLiteral("typedef");
    case SymbolKind::Function: return QStringLiteral("function");
    case SymbolKind::Method: return QStringLiteral("method");
    case SymbolKind::Constructor: return QStringLiteral("constructor");
    case SymbolKind::Destructor: return QStringLiteral("destructor");
    case SymbolKind::Operator: return QStringLiteral("operator");
    case SymbolKind::Field: return QStringLiteral("field");
    case SymbolKind::Variable: return QStringLiteral("variable");
    case SymbolKind::Macro: return QStringLiteral("macro");
    case SymbolKind::Concept: return QStringLiteral("concept");
    case SymbolKind::Unknown: break;
    }
    return QStringLiteral("symbol");
}

QString refKindName(RefKind k)
{
    switch (k) {
    case RefKind::Call: return QStringLiteral("call");
    case RefKind::TypeUse: return QStringLiteral("type use");
    case RefKind::VarUse: return QStringLiteral("variable use");
    case RefKind::Inherit: return QStringLiteral("inheritance");
    case RefKind::Override: return QStringLiteral("override");
    case RefKind::MacroUse: return QStringLiteral("macro use");
    case RefKind::Include: return QStringLiteral("include");
    case RefKind::Declaration: return QStringLiteral("declaration");
    case RefKind::Count: break;
    }
    return QString();
}

bool isType(SymbolKind k)
{
    switch (k) {
    case SymbolKind::Class:
    case SymbolKind::Struct:
    case SymbolKind::Union:
    case SymbolKind::Enum:
    case SymbolKind::Typedef:
    case SymbolKind::Concept:
        return true;
    default:
        return false;
    }
}

bool isFunction(SymbolKind k)
{
    switch (k) {
    case SymbolKind::Function:
    case SymbolKind::Method:
    case SymbolKind::Constructor:
    case SymbolKind::Destructor:
    case SymbolKind::Operator:
        return true;
    default:
        return false;
    }
}

bool isVariable(SymbolKind k)
{
    return k == SymbolKind::Field || k == SymbolKind::Variable || k == SymbolKind::EnumConstant;
}

bool isScope(SymbolKind k)
{
    return k == SymbolKind::Namespace || k == SymbolKind::Class || k == SymbolKind::Struct
        || k == SymbolKind::Union || k == SymbolKind::Enum;
}

qint32 CodeModel::addFile(const QString &path)
{
    const auto it = m_fileIds.constFind(path);
    if (it != m_fileIds.cend())
        return *it;
    File f;
    f.path = path;
    files.append(f);
    const qint32 id = qint32(files.size() - 1);
    m_fileIds.insert(path, id);
    return id;
}

qint32 CodeModel::addSymbol(const QString &usr)
{
    const auto it = m_usrIds.constFind(usr);
    if (it != m_usrIds.cend())
        return *it;
    Symbol s;
    s.usr = usr;
    symbols.append(s);
    const qint32 id = qint32(symbols.size() - 1);
    m_usrIds.insert(usr, id);
    return id;
}

void CodeModel::clear()
{
    *this = CodeModel();
}

void CodeModel::rebuildIndexes()
{
    m_fileIds.clear();
    m_usrIds.clear();
    for (qint32 i = 0; i < files.size(); ++i)
        m_fileIds.insert(files[i].path, i);
    for (qint32 i = 0; i < symbols.size(); ++i)
        m_usrIds.insert(symbols[i].usr, i);
}

qint32 CodeModel::nodeOwner(qint32 s) const
{
    if (s < 0 || s >= symbols.size())
        return -1;
    qint32 cur = s;
    int guard = 0;
    while (guard++ < 64) {
        const qint32 p = symbols[cur].parent;
        if (p < 0 || symbols[p].kind == SymbolKind::Namespace)
            return cur;
        cur = p;
    }
    return cur;
}

QVector<qint32> CodeModel::ancestors(qint32 s) const
{
    QVector<qint32> out;
    int guard = 0;
    for (qint32 p = s >= 0 ? symbols[s].parent : -1; p >= 0 && guard++ < 64; p = symbols[p].parent)
        out.prepend(p);
    return out;
}

qint32 CodeModel::outermostType(qint32 s) const
{
    qint32 best = -1;
    int guard = 0;
    for (qint32 cur = s; cur >= 0 && guard++ < 64; cur = symbols[cur].parent) {
        if (isType(symbols[cur].kind))
            best = cur;
    }
    return best >= 0 ? best : s;
}

qint32 CodeModel::enclosingFunction(const Location &loc) const
{
    if (loc.file < 0 || loc.file >= m_functionsInFile.size())
        return -1;
    const QVector<qint32> &list = m_functionsInFile[loc.file];
    // Candidates start at or before the location; the last one that
    // contains it is the innermost, as extents nest
    auto it = std::upper_bound(list.cbegin(), list.cend(), loc, [this](const Location &l, qint32 id) {
        const Location &d = symbols[id].def;
        return std::tie(l.line, l.col) < std::tie(d.line, d.col);
    });
    int steps = 0;
    while (it != list.cbegin() && steps++ < 4000) {
        --it;
        if (symbols[*it].def.contains(loc))
            return *it;
    }
    return -1;
}

qint32 CodeModel::innermostAt(qint32 file, qint32 line) const
{
    if (file < 0 || file >= m_functionsInFile.size())
        return -1;
    const QVector<qint32> &list = m_functionsInFile[file];
    auto it = std::upper_bound(list.cbegin(), list.cend(), line, [this](qint32 l, qint32 id) {
        return l < symbols[id].def.line;
    });
    int steps = 0;
    while (it != list.cbegin() && steps++ < 4000) {
        --it;
        const Location &d = symbols[*it].def;
        if (d.line <= line && line <= d.endLine)
            return *it;
    }
    return -1;
}

bool CodeModel::isProjectSymbol(qint32 s) const
{
    if (s < 0 || s >= symbols.size())
        return false;
    return !symbols[s].has(External);
}

QString CodeModel::displayName(qint32 s) const
{
    if (s < 0 || s >= symbols.size())
        return QString();
    const Symbol &sym = symbols[s];
    return sym.qualified.isEmpty() ? sym.name : sym.qualified;
}

QString CodeModel::locationText(const Location &loc) const
{
    if (loc.file < 0 || loc.file >= files.size())
        return QString();
    return files[loc.file].relative + QLatin1Char(':') + QString::number(loc.line);
}

QVector<qint32> CodeModel::edgesFrom(qint32 s, RefKind kind) const
{
    QVector<qint32> out;
    if (s < 0 || s >= outEdges.size())
        return out;
    for (qint32 e : outEdges[s]) {
        if (kind == RefKind::Count || edges[e].kind == kind)
            out.append(e);
    }
    return out;
}

QVector<qint32> CodeModel::edgesTo(qint32 s, RefKind kind) const
{
    QVector<qint32> out;
    if (s < 0 || s >= inEdges.size())
        return out;
    for (qint32 e : inEdges[s]) {
        if (kind == RefKind::Count || edges[e].kind == kind)
            out.append(e);
    }
    return out;
}

void CodeModel::finalize()
{
    const qint32 nf = qint32(files.size());
    const qint32 ns = qint32(symbols.size());

    // Files: their path relative to the root and their module
    const QDir rootDir(root);
    const QString build = buildDir.isEmpty() ? QString() : withSlash(QDir::cleanPath(buildDir));
    const QString rootSlash = withSlash(QDir::cleanPath(root));
    for (File &f : files) {
        f.project = !root.isEmpty() && f.path.startsWith(rootSlash);
        f.generated = f.project && !build.isEmpty() && build != rootSlash && f.path.startsWith(build);
        if (f.path.contains(QLatin1String("_autogen/")) || f.path.contains(QLatin1String("/moc_"))
            || QFileInfo(f.path).fileName().startsWith(QLatin1String("moc_"))
            || QFileInfo(f.path).fileName().startsWith(QLatin1String("ui_"))
            || QFileInfo(f.path).fileName().startsWith(QLatin1String("qrc_")))
            f.generated = true;
        f.relative = f.project ? rootDir.relativeFilePath(f.path) : f.path;
        static const QStringList headerSuffixes = {"", "h", "hh", "hpp", "hxx", "h++", "inl", "ipp", "tpp", "tcc", "inc", "def"};
        f.header = headerSuffixes.contains(QFileInfo(f.path).suffix().toLower());
        if (f.project) {
            const qsizetype slash = f.relative.lastIndexOf(QLatin1Char('/'));
            f.module = slash < 0 ? QString() : f.relative.left(slash);
        } else {
            f.module = QStringLiteral("<external>");
        }
        f.includedBy.clear();
        f.symbols.clear();
    }

    // Qualified names, children, the files' symbols
    for (Symbol &s : symbols)
        s.children.clear();
    QVector<qint8> done(ns, 0);
    std::function<void(qint32)> qualify = [&](qint32 i) {
        if (done[i])
            return;
        done[i] = 1;
        Symbol &s = symbols[i];
        if (s.parent == i)
            s.parent = -1;
        if (s.parent >= 0) {
            qualify(s.parent);
            const Symbol &p = symbols[s.parent];
            const QString pq = p.qualified.isEmpty() ? p.name : p.qualified;
            // Python's names are dotted
            const QString sep = s.usr.startsWith(QLatin1String("py:")) ? QStringLiteral(".") : QStringLiteral("::");
            s.qualified = pq.isEmpty() ? s.name : pq + sep + s.name;
        } else if (s.qualified.isEmpty()) {
            s.qualified = s.name;
        }
    };
    for (qint32 i = 0; i < ns; ++i)
        qualify(i);
    for (qint32 i = 0; i < ns; ++i) {
        Symbol &s = symbols[i];
        if (s.parent >= 0)
            symbols[s.parent].children.append(i);
        const Location w = s.where();
        if (w.file >= 0 && w.file < nf)
            files[w.file].symbols.append(i);
        if (s.kind == SymbolKind::Function && s.parent < 0
            && (s.name == QLatin1String("main") || s.name == QLatin1String("wmain")
                || s.name == QLatin1String("WinMain") || s.name == QLatin1String("wWinMain")
                || s.name == QLatin1String("LLVMFuzzerTestOneInput")))
            s.flags |= EntryPoint;
        if (s.where().file >= 0 && s.where().file < nf && files[s.where().file].generated)
            s.flags |= Generated;
    }
    for (Symbol &s : symbols) {
        // Only forward-declared here (class QImage;): someone else's type
        if (isType(s.kind) && s.kind != SymbolKind::Typedef && !s.has(Defined))
            s.flags |= External;
        if (s.kind == SymbolKind::Class || s.kind == SymbolKind::Struct) {
            for (qint32 c : std::as_const(s.children)) {
                if (symbols[c].has(PureVirtual))
                    s.flags |= Abstract;
            }
        }
    }

    // Definitions by file, to find what a location is in
    m_functionsInFile = QVector<QVector<qint32>>(nf);
    for (qint32 i = 0; i < ns; ++i) {
        const Symbol &s = symbols[i];
        if ((isFunction(s.kind) || isType(s.kind)) && s.def.valid() && s.def.endLine >= s.def.line
            && s.def.file < nf)
            m_functionsInFile[s.def.file].append(i);
    }
    for (QVector<qint32> &list : m_functionsInFile) {
        std::sort(list.begin(), list.end(), [this](qint32 a, qint32 b) {
            const Location &x = symbols[a].def, &y = symbols[b].def;
            return std::tie(x.line, x.col, y.endLine) < std::tie(y.line, y.col, x.endLine);
        });
    }
    // References the indexer found outside of any entity (macro expansions)
    for (Reference &r : refs) {
        if (r.from < 0 && r.kind != RefKind::Declaration)
            r.from = enclosingFunction(r.loc);
    }

    // Macros used in other macros' definitions
    if (!macroNameUses.isEmpty()) {
        QHash<QString, QVector<qint32>> macros;
        for (qint32 i = 0; i < ns; ++i) {
            if (symbols[i].kind == SymbolKind::Macro && !symbols[i].has(External))
                macros[symbols[i].name].append(i);
        }
        for (const NameUse &u : std::as_const(macroNameUses)) {
            for (qint32 to : macros.value(u.name)) {
                if (to == u.from)
                    continue;
                Reference r;
                r.from = u.from;
                r.to = to;
                r.kind = RefKind::MacroUse;
                r.loc = u.loc;
                refs.append(r);
            }
        }
        macroNameUses.clear();
    }

    // Sort and deduplicate the references: headers are indexed by every
    // translation unit that includes them
    std::sort(refs.begin(), refs.end(), [](const Reference &a, const Reference &b) {
        return std::tie(a.loc.file, a.loc.line, a.loc.col, a.to, a.kind, a.from)
             < std::tie(b.loc.file, b.loc.line, b.loc.col, b.to, b.kind, b.from);
    });
    refs.erase(std::unique(refs.begin(), refs.end(), [](const Reference &a, const Reference &b) {
                   return a.loc.file == b.loc.file && a.loc.line == b.loc.line && a.loc.col == b.loc.col
                       && a.to == b.to && a.kind == b.kind && a.from == b.from;
               }), refs.end());

    // Edges
    edges.clear();
    QHash<quint64, qint32> edgeIds;
    refsInFile = QVector<QVector<qint32>>(nf);
    refsTo = QVector<QVector<qint32>>(ns);
    declsOf = QVector<QVector<qint32>>(ns);
    for (qint32 i = 0; i < refs.size(); ++i) {
        const Reference &r = refs[i];
        if (r.loc.file >= 0 && r.loc.file < nf)
            refsInFile[r.loc.file].append(i);
        if (r.to < 0 || r.to >= ns)
            continue;
        if (r.kind == RefKind::Declaration) {
            declsOf[r.to].append(i);
            continue;
        }
        refsTo[r.to].append(i);
        if (r.from < 0 || r.from >= ns)
            continue;
        const quint64 key = (quint64(quint32(r.from)) << 34) ^ (quint64(quint32(r.to)) << 4) ^ quint64(r.kind);
        auto it = edgeIds.find(key);
        if (it == edgeIds.end()) {
            Edge e;
            e.from = r.from;
            e.to = r.to;
            e.kind = r.kind;
            e.firstRef = i;
            edges.append(e);
            it = edgeIds.insert(key, qint32(edges.size() - 1));
        }
        Edge &e = edges[*it];
        // Keys can collide only in theory; be safe
        if (e.from != r.from || e.to != r.to || e.kind != r.kind)
            continue;
        e.count++;
        e.flags |= r.flags;
    }
    outEdges = QVector<QVector<qint32>>(ns);
    inEdges = QVector<QVector<qint32>>(ns);
    for (qint32 i = 0; i < edges.size(); ++i) {
        outEdges[edges[i].from].append(i);
        inEdges[edges[i].to].append(i);
    }

    // Fan-in and fan-out between nodes, not within a class
    for (qint32 i = 0; i < ns; ++i) {
        Symbol &s = symbols[i];
        const qint32 own = nodeOwner(i);
        QSet<qint32> in, out;
        for (qint32 e : std::as_const(inEdges[i])) {
            if (nodeOwner(edges[e].from) != own)
                in.insert(edges[e].from);
        }
        for (qint32 e : std::as_const(outEdges[i])) {
            if (nodeOwner(edges[e].to) != own)
                out.insert(edges[e].to);
        }
        s.fanIn = qint32(in.size());
        s.fanOut = qint32(out.size());
    }

    // Includes, both ways
    for (qint32 i = 0; i < nf; ++i) {
        File &f = files[i];
        std::sort(f.includes.begin(), f.includes.end(), [](const Include &a, const Include &b) {
            return std::tie(a.line, a.file) < std::tie(b.line, b.file);
        });
        f.includes.erase(std::unique(f.includes.begin(), f.includes.end(), [](const Include &a, const Include &b) {
                             return a.line == b.line && a.file == b.file;
                         }), f.includes.end());
        for (const Include &inc : std::as_const(f.includes)) {
            QVector<qint32> &by = files[inc.file >= 0 && inc.file < nf ? inc.file : 0].includedBy;
            if (inc.file >= 0 && inc.file < nf && (by.isEmpty() || by.last() != i))
                by.append(i);
        }
    }

    {
        QSet<QString> seen;
        QVector<Diagnostic> unique;
        for (const Diagnostic &d : std::as_const(diagnostics)) {
            const QString key = QString::number(d.loc.file) + QLatin1Char(':') + QString::number(d.loc.line) + QLatin1Char(':')
                              + QString::number(d.loc.col) + QLatin1Char(':') + d.message;
            if (!seen.contains(key)) {
                seen.insert(key);
                unique.append(d);
            }
        }
        diagnostics = unique;
    }

    QSet<QString> mods;
    for (const File &f : std::as_const(files)) {
        if (f.project)
            mods.insert(f.module);
    }
    modules = QStringList(mods.cbegin(), mods.cend());
    modules.sort();
    moduleIndex.clear();
    for (qint32 i = 0; i < modules.size(); ++i)
        moduleIndex.insert(modules[i], i);
}

Stats CodeModel::stats() const
{
    Stats st;
    st.files = qint32(files.size());
    for (const File &f : files) {
        if (f.project) {
            st.projectFiles++;
            st.lines += f.lines;
            st.codeLines += f.codeLines;
        }
        if (f.indexed)
            st.units++;
    }
    for (const Symbol &s : symbols) {
        if (s.has(External))
            continue;
        st.symbols++;
        if (isType(s.kind))
            st.types++;
        else if (isFunction(s.kind))
            st.functions++;
        else if (isVariable(s.kind))
            st.variables++;
    }
    st.references = refs.size();
    for (const Diagnostic &d : diagnostics) {
        if (d.severity >= Diagnostic::Error)
            st.errors++;
        else if (d.severity == Diagnostic::Warning)
            st.warnings++;
    }
    st.indexMs = indexMs;
    return st;
}

// --- Saving and loading ----------------------------------------------------

namespace {

constexpr quint32 kMagic = 0x554e5256; // "UNRV"
constexpr quint32 kVersion = 5;

QDataStream &operator<<(QDataStream &s, const Location &l)
{
    return s << l.file << l.line << l.col << l.endLine << l.endCol;
}
QDataStream &operator>>(QDataStream &s, Location &l)
{
    return s >> l.file >> l.line >> l.col >> l.endLine >> l.endCol;
}

} // namespace

bool CodeModel::save(const QString &path) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QDataStream s(&file);
    s.setVersion(QDataStream::Qt_6_5);
    s << kMagic << kVersion << root << buildDir << indexedAt << indexMs;

    s << qint32(files.size());
    for (const File &f : files) {
        s << f.path << f.indexed << f.system << f.lines << f.codeLines << qint32(f.includes.size());
        for (const Include &inc : f.includes)
            s << inc.file << inc.line;
    }
    s << qint32(symbols.size());
    for (const Symbol &y : symbols) {
        s << y.usr << y.name << y.signature << y.type << quint8(y.kind) << quint8(y.access) << y.flags
          << y.parent << y.decl << y.def;
        const FunctionMetrics &m = y.metrics;
        s << m.lines << m.statements << m.complexity << m.nesting << m.params << m.returns << m.cognitive
          << m.cloneHash << m.tokens << m.minHash;
    }
    s << qint64(refs.size());
    for (const Reference &r : refs)
        s << r.from << r.to << quint8(r.kind) << r.flags << r.loc;
    s << qint32(diagnostics.size());
    for (const Diagnostic &d : diagnostics)
        s << quint8(d.severity) << d.message << d.category << d.loc << d.unit;
    return s.status() == QDataStream::Ok && file.commit();
}

bool CodeModel::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QDataStream s(&file);
    s.setVersion(QDataStream::Qt_6_5);
    quint32 magic = 0, version = 0;
    s >> magic >> version;
    if (magic != kMagic || version != kVersion)
        return false;
    CodeModel m;
    s >> m.root >> m.buildDir >> m.indexedAt >> m.indexMs;
    // No count can be more than the file has bytes: a cut or broken file
    // mustn't make us allocate gigabytes
    const qint64 limit = file.size();

    qint32 nf = 0;
    s >> nf;
    if (nf < 0 || nf > limit)
        return false;
    m.files.resize(nf);
    for (File &f : m.files) {
        qint32 ni = 0;
        s >> f.path >> f.indexed >> f.system >> f.lines >> f.codeLines >> ni;
        if (ni < 0 || ni > limit || s.status() != QDataStream::Ok)
            return false;
        f.includes.resize(ni);
        for (Include &inc : f.includes)
            s >> inc.file >> inc.line;
    }
    qint32 ns = 0;
    s >> ns;
    if (ns < 0 || ns > limit)
        return false;
    m.symbols.resize(ns);
    for (Symbol &y : m.symbols) {
        quint8 kind = 0, access = 0;
        s >> y.usr >> y.name >> y.signature >> y.type >> kind >> access >> y.flags >> y.parent >> y.decl >> y.def;
        y.kind = SymbolKind(kind);
        y.access = Access(access);
        FunctionMetrics &mm = y.metrics;
        s >> mm.lines >> mm.statements >> mm.complexity >> mm.nesting >> mm.params >> mm.returns >> mm.cognitive
          >> mm.cloneHash >> mm.tokens >> mm.minHash;
        if (s.status() != QDataStream::Ok)
            return false;
    }
    qint64 nr = 0;
    s >> nr;
    if (nr < 0 || nr > limit)
        return false;
    m.refs.resize(nr);
    for (Reference &r : m.refs) {
        quint8 kind = 0;
        s >> r.from >> r.to >> kind >> r.flags >> r.loc;
        r.kind = RefKind(kind);
    }
    qint32 nd = 0;
    s >> nd;
    if (nd < 0 || nd > limit)
        return false;
    m.diagnostics.resize(nd);
    for (Diagnostic &d : m.diagnostics) {
        quint8 sev = 0;
        s >> sev >> d.message >> d.category >> d.loc >> d.unit;
        d.severity = Diagnostic::Severity(sev);
    }
    if (s.status() != QDataStream::Ok)
        return false;
    // Ids in the file must be in range before anything indexes with them
    for (const Symbol &y : m.symbols) {
        if (y.parent >= ns || y.decl.file >= nf || y.def.file >= nf)
            return false;
    }
    for (const Reference &r : m.refs) {
        if (r.to >= ns || r.from >= ns || r.loc.file >= nf)
            return false;
    }
    for (const File &f : m.files) {
        for (const Include &inc : f.includes) {
            if (inc.file >= nf)
                return false;
        }
    }
    for (const Diagnostic &d : m.diagnostics) {
        if (d.loc.file >= nf || d.unit >= nf)
            return false;
    }
    m.rebuildIndexes();
    m.finalize();
    *this = std::move(m);
    return true;
}

} // namespace Unravel
