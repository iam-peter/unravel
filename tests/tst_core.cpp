// The engine on samples/warehouse, a small program with known flaws:
// clang must find its structure, and the analyses its problems.

#include <QFile>
#include <QProcess>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

#include "analysis.h"
#include "history.h"
#include "focus.h"
#include "indexer.h"
#include "layout.h"
#include "project.h"
#include "python.h"
#include "report.h"

using namespace Unravel;

class TestCore : public QObject
{
    Q_OBJECT

private:
    CodeModel m;
    Analysis::Report report;

    qint32 sym(const QString &qualified) const
    {
        for (qint32 i = 0; i < m.symbols.size(); ++i) {
            if (m.symbols[i].qualified == qualified && !m.symbols[i].has(External))
                return i;
        }
        return -1;
    }
    bool hasEdge(const QString &from, const QString &to, RefKind kind) const
    {
        const qint32 a = sym(from), b = sym(to);
        if (a < 0 || b < 0)
            return false;
        for (qint32 e : m.outEdges[a]) {
            if (m.edges[e].to == b && m.edges[e].kind == kind)
                return true;
        }
        return false;
    }
    QVector<const Analysis::Finding *> findings(Analysis::FindingKind k) const
    {
        QVector<const Analysis::Finding *> out;
        for (const Analysis::Finding &f : report.findings) {
            if (f.kind == k)
                out.append(&f);
        }
        return out;
    }
    bool mentions(Analysis::FindingKind k, const QString &qualified) const
    {
        const qint32 s = sym(qualified);
        for (const Analysis::Finding *f : findings(k)) {
            if (f->symbols.contains(s) || f->detail.contains(qualified) || f->title.contains(qualified))
                return true;
        }
        return false;
    }

private slots:
    void initTestCase()
    {
        const ProjectSetup setup = Project::discover(QStringLiteral(UNRAVEL_SAMPLE_DIR));
        QVERIFY2(setup.error.isEmpty(), qPrintable(setup.error));
        QCOMPARE(setup.units.size(), 9);
        const IndexResult r = Indexer::run(setup, m);
        QVERIFY(r.ok);
        QCOMPARE(r.failedUnits, 0);
        report = Analysis::analyze(m);
        // What clang complained about, for when the checks below fail on a
        // platform whose headers it doesn't handle
        int shown = 0;
        for (const Diagnostic &d : m.diagnostics) {
            if (d.severity < Diagnostic::Error || shown++ >= 20)
                continue;
            const QString where = d.loc.file >= 0 ? m.files[d.loc.file].path : QStringLiteral("?");
            qWarning("%s:%d: %s", qPrintable(where), d.loc.line, qPrintable(d.message));
        }
    }

    void symbols()
    {
        QVERIFY(sym(QStringLiteral("wh::Inventory")) >= 0);
        QCOMPARE(m.symbols[sym(QStringLiteral("wh::Inventory"))].kind, SymbolKind::Class);
        QCOMPARE(m.symbols[sym(QStringLiteral("wh::Item"))].kind, SymbolKind::Struct);
        QCOMPARE(m.symbols[sym(QStringLiteral("wh::Category"))].kind, SymbolKind::Enum);
        QCOMPARE(m.symbols[sym(QStringLiteral("wh::net::Server::handle"))].kind, SymbolKind::Method);
        QVERIFY(m.symbols[sym(QStringLiteral("main"))].has(EntryPoint));
        QVERIFY(m.symbols[sym(QStringLiteral("wh::PricingPolicy"))].has(Abstract));
        QVERIFY(m.symbols[sym(QStringLiteral("wh::PricingPolicy::price"))].has(PureVirtual));
        // A method's definition in the .cpp, its declaration in the header
        const Symbol &add = m.symbols[sym(QStringLiteral("wh::Inventory::add"))];
        QVERIFY(m.files[add.def.file].relative.endsWith(QLatin1String("core/inventory.cpp")));
        QVERIFY(m.files[add.decl.file].relative.endsWith(QLatin1String("core/inventory.h")));
        // Libraries are there as what the project uses, not as its own
        bool string = false;
        for (const Symbol &s : m.symbols)
            string = string || (s.name == QLatin1String("basic_string") && s.has(External));
        QVERIFY(string);
    }

    void declarationsAreStableAcrossUnitOrder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto write = [&](const QString &name, const QByteArray &text) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(text), text.size());
        };
        write(QStringLiteral("api.h"), "int answer();\n");
        write(QStringLiteral("a.cpp"), "int answer();\n#include \"api.h\"\nint answer() { return 42; }\n");
        write(QStringLiteral("b.cpp"), "#include \"api.h\"\nint use() { return answer(); }\n");
        auto setup = Project::discover(dir.path());
        for (int threads : {1, 2}) {
            for (int order = 0; order < 2; ++order) {
                CodeModel model;
                QVERIFY(Indexer::run(setup, model, nullptr, {}, threads).ok);
                bool found = false;
                for (const Symbol &symbol : model.symbols) {
                    if (symbol.name != QLatin1String("answer"))
                        continue;
                    found = true;
                    QCOMPARE(model.files[symbol.decl.file].path, dir.filePath(QStringLiteral("api.h")));
                    QCOMPARE(model.files[symbol.def.file].path, dir.filePath(QStringLiteral("a.cpp")));
                }
                QVERIFY(found);
                std::reverse(setup.units.begin(), setup.units.end());
            }
        }
    }

    void includeOnlyCyclesExcludeSemanticEdges()
    {
        CodeModel model;
        model.root = QStringLiteral("/project");
        const int a = model.addFile(QStringLiteral("/project/a/a.h"));
        const int b = model.addFile(QStringLiteral("/project/b/b.h"));
        model.files[a].includes.append({b, 1});
        const int type = model.addSymbol(QStringLiteral("A"));
        model.symbols[type].name = QStringLiteral("A");
        model.symbols[type].kind = SymbolKind::Class;
        model.symbols[type].def = {a, 2, 1, 2, 12};
        model.refs.append({-1, type, RefKind::TypeUse, RefRead, {b, 2, 1, 2, 2}});
        model.finalize();
        const auto mixed = Analysis::dependencyGraph(model, Analysis::Level::File);
        QCOMPARE(mixed.links.size(), 2);
        QVERIFY(mixed.links.first().cyclic);
        for (const auto level : {Analysis::Level::File, Analysis::Level::Module}) {
            const auto includes = Analysis::dependencyGraph(model, level, 0, false, {}, {}, true);
            for (const auto &link : includes.links)
                QVERIFY(!link.cyclic);
        }
        auto includes = Analysis::dependencyGraph(model, Analysis::Level::File, 0, false, {}, {}, true);
        QCOMPARE(includes.links.size(), 1);
        QCOMPARE(includes.links.first().count, 0);
        model.files[b].includes.append({a, 1});
        model.finalize();
        includes = Analysis::dependencyGraph(model, Analysis::Level::File, 0, false, {}, {}, true);
        QCOMPARE(includes.links.size(), 2);
        for (const auto &link : includes.links)
            QVERIFY(link.cyclic);
    }

    void metrics()
    {
        const FunctionMetrics &report = m.symbols[sym(QStringLiteral("wh::Inventory::report"))].metrics;
        QCOMPARE(report.params, 6);
        QVERIFY(report.complexity >= 10);
        QCOMPARE(report.nesting, 4);
        const FunctionMetrics &value = m.symbols[sym(QStringLiteral("wh::Item::value"))].metrics;
        QCOMPARE(value.complexity, 1);
        QCOMPARE(value.statements, 1);
        // The three split loops are the same code with other names
        const quint64 h = m.symbols[sym(QStringLiteral("wh::splitSku"))].metrics.cloneHash;
        QCOMPARE(m.symbols[sym(QStringLiteral("wh::splitFields"))].metrics.cloneHash, h);
        QCOMPARE(m.symbols[sym(QStringLiteral("wh::splitWords"))].metrics.cloneHash, h);
    }

    void relations()
    {
        QVERIFY(hasEdge(QStringLiteral("wh::FlatPricing"), QStringLiteral("wh::PricingPolicy"), RefKind::Inherit));
        QVERIFY(hasEdge(QStringLiteral("wh::SeasonalPricing"), QStringLiteral("wh::TieredPricing"), RefKind::Inherit));
        QVERIFY(hasEdge(QStringLiteral("wh::FlatPricing::price"), QStringLiteral("wh::PricingPolicy::price"), RefKind::Override));
        QVERIFY(hasEdge(QStringLiteral("wh::runCommand"), QStringLiteral("wh::Inventory::add"), RefKind::Call));
        QVERIFY(hasEdge(QStringLiteral("wh::SeasonalPricing::price"), QStringLiteral("wh::TieredPricing::price"), RefKind::Call));
        QVERIFY(hasEdge(QStringLiteral("wh::Inventory::add"), QStringLiteral("wh::g_operations"), RefKind::VarUse));
        QVERIFY(hasEdge(QStringLiteral("wh::Inventory::m_store"), QStringLiteral("wh::Store"), RefKind::TypeUse));
        QVERIFY(hasEdge(QStringLiteral("wh::fibonacciDiscount"), QStringLiteral("wh::fibonacciDiscount"), RefKind::Call));
        // Includes
        const qint32 inv = m.fileId(QStringLiteral(UNRAVEL_SAMPLE_DIR "/core/inventory.h"));
        const qint32 store = m.fileId(QStringLiteral(UNRAVEL_SAMPLE_DIR "/storage/store.h"));
        QVERIFY(inv >= 0 && store >= 0);
        bool included = false;
        for (const Include &i : m.files[inv].includes)
            included = included || i.file == store;
        QVERIFY(included);
        QVERIFY(m.files[store].includedBy.contains(inv));
    }

    void calls()
    {
        const QVector<Analysis::CallSite> calls = Analysis::callsFrom(m, sym(QStringLiteral("wh::runCommand")));
        QVERIFY(calls.size() >= 6);
        // In the order of the code: args.empty() first, printUsage() before add()
        QCOMPARE(m.symbols[calls.first().callee].name, QStringLiteral("empty"));
        auto at = [&](const QString &name) {
            for (int i = 0; i < calls.size(); ++i) {
                if (m.symbols[calls[i].callee].name == name)
                    return i;
            }
            return -1;
        };
        QVERIFY(at(QStringLiteral("printUsage")) >= 0);
        QVERIFY(at(QStringLiteral("printUsage")) < at(QStringLiteral("add")));
        QVERIFY(at(QStringLiteral("add")) < at(QStringLiteral("applyPricing")));
        // printUsage() is called twice
        QCOMPARE(calls[at(QStringLiteral("printUsage"))].count, 2);
        // main reaches Inventory::restock through the server
        const Analysis::Subgraph paths = Analysis::callPaths(m, sym(QStringLiteral("main")), sym(QStringLiteral("wh::Inventory::restock")));
        QVERIFY(paths.nodes.contains(sym(QStringLiteral("wh::net::Server::handle"))));
        // A call of a pure virtual method may run any override
        const QVector<qint32> over = Analysis::overriders(m, sym(QStringLiteral("wh::PricingPolicy::price")));
        QCOMPARE(over.size(), 3);
    }

    void findingsAreFound()
    {
        using K = Analysis::FindingKind;
        QVERIFY(mentions(K::Duplicate, QStringLiteral("wh::splitWords")));
        QVERIFY(mentions(K::NonVirtualDestructor, QStringLiteral("wh::PricingPolicy")));
        QVERIFY(mentions(K::Unused, QStringLiteral("wh::splitSku")));
        QVERIFY(mentions(K::Unused, QStringLiteral("wh::MemoryStore::clear")));
        QVERIFY(!mentions(K::Unused, QStringLiteral("wh::Inventory::add")));
        QVERIFY(!mentions(K::Unused, QStringLiteral("wh::CsvStore::read")));   // overrides a base
        QVERIFY(mentions(K::UnreadField, QStringLiteral("wh::Inventory::m_lastAudit")));
        QVERIFY(mentions(K::GlobalState, QStringLiteral("wh::g_operations")));
        QVERIFY(mentions(K::Recursion, QStringLiteral("wh::fibonacciDiscount")));
        QVERIFY(mentions(K::ManyParameters, QStringLiteral("wh::Inventory::report")));
        // core and storage, app and net depend on each other
        QStringList cycles;
        for (const Analysis::Finding *f : findings(K::ModuleCycle))
            cycles << f->detail;
        QVERIFY2(cycles.contains(QStringLiteral("core ↔ storage")), qPrintable(cycles.join(QLatin1Char('|'))));
        QVERIFY(cycles.contains(QStringLiteral("app ↔ net")));
        QVERIFY(findings(K::ParseErrors).isEmpty());
    }

    void dependencyGraphs()
    {
        const Analysis::DepGraph g = Analysis::dependencyGraph(m, Analysis::Level::Module);
        QCOMPARE(g.names.size(), 4);
        const qint32 core = qint32(g.names.indexOf(QStringLiteral("core")));
        const qint32 storage = qint32(g.names.indexOf(QStringLiteral("storage")));
        QVERIFY(g.linkIndex(core, storage) >= 0 && g.linkIndex(storage, core) >= 0);
        QVERIFY(g.links[g.linkIndex(core, storage)].cyclic);
        // The order of a matrix keeps cycles together
        const QVector<qint32> order = Analysis::dependencyOrder(g);
        QCOMPARE(order.size(), 4);
        QCOMPARE(std::abs(order.indexOf(core) - order.indexOf(storage)), 1);
        const QVector<Analysis::ModuleMetrics> mm = Analysis::moduleMetrics(m);
        for (const Analysis::ModuleMetrics &x : mm) {
            QVERIFY(x.instability >= 0 && x.instability <= 1);
            QVERIFY(x.distance >= 0 && x.distance <= 1);
        }
        // Folded: storage is one node, at the file level too, and links go into it
        const Analysis::DepGraph files = Analysis::dependencyGraph(m, Analysis::Level::File, 0, false, {}, {QStringLiteral("storage")});
        const qint32 folder = qint32(files.folder.indexOf(QStringLiteral("storage")));
        QVERIFY(folder >= 0);
        QCOMPARE(files.folder.count(QStringLiteral("storage")), 1);
        for (const QString &name : files.names)
            QVERIFY2(!name.startsWith(QLatin1String("storage/")), qPrintable(name));
        const qint32 inventory = qint32(files.names.indexOf(QStringLiteral("core/inventory.cpp")));
        QVERIFY(files.linkIndex(inventory, folder) >= 0);
        const Analysis::DepGraph foldedClasses = Analysis::dependencyGraph(m, Analysis::Level::Class, 0, false, {}, {QStringLiteral("storage")});
        QVERIFY(!foldedClasses.names.contains(QStringLiteral("wh::CsvStore")));
        QVERIFY(foldedClasses.folder.contains(QStringLiteral("storage")));
        QCOMPARE(Analysis::foldedFolderOf(QStringLiteral("storage/sub"), {QStringLiteral("storage")}), QStringLiteral("storage"));
        QCOMPARE(Analysis::foldedFolderOf(QStringLiteral("storagex"), {QStringLiteral("storage")}), QString());

        const Analysis::DepGraph classes = Analysis::dependencyGraph(m, Analysis::Level::Class);
        QVERIFY(classes.names.contains(QStringLiteral("wh::Inventory")));
        QVERIFY(!Analysis::linkReferences(m, g, g.linkIndex(core, storage)).isEmpty());
    }

    void saveAndLoad()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("index.idx"));
        QVERIFY(m.save(path));
        CodeModel loaded;
        QVERIFY(loaded.load(path));
        QCOMPARE(loaded.symbols.size(), m.symbols.size());
        QCOMPARE(loaded.refs.size(), m.refs.size());
        QCOMPARE(loaded.edges.size(), m.edges.size());
        QCOMPARE(loaded.files.size(), m.files.size());
        const qint32 s = sym(QStringLiteral("wh::Inventory::report"));
        QCOMPARE(loaded.symbols[s].metrics.complexity, m.symbols[s].metrics.complexity);
        QCOMPARE(loaded.symbols[s].qualified, m.symbols[s].qualified);
        // Garbage is refused, not trusted
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.seek(20);
        f.write(QByteArray(64, '\xff'));
        f.close();
        CodeModel broken;
        QVERIFY(!broken.load(path) || broken.symbols.size() == m.symbols.size());
    }

    void markdownReport()
    {
        const QString md = Reporting::markdown(m, report);
        QVERIFY(md.contains(QLatin1String("# Code review: warehouse")));
        QVERIFY(md.contains(QLatin1String("Duplicated code")));
        QVERIFY(md.contains(QLatin1String("core/pricing.h")));
    }

    // What the reviews found: an in-source compilation database, a class only
    // its own members name, the destructor idiom, macros in macros, .inc files
    void edgeCases()
    {
        QTemporaryDir dir;
        auto write = [&](const QString &name, const QByteArray &text) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(text);
        };
        write(QStringLiteral("a.h"), "#pragma once\n#define INNER_MACRO 2\n#define OUTER_MACRO (INNER_MACRO + 1)\n"
                                    "class NeverUsed { public: void f(); NeverUsed(const NeverUsed &); };\n"
                                    "class Base { public: virtual void g(); protected: ~Base(); };\n"
                                    "class Derived : public Base { public: void g() override; };\n"
                                    "#include \"data.inc\"\n");
        write(QStringLiteral("data.inc"), "inline int data() { return 1; }\n");
        write(QStringLiteral("a.cpp"), "#include \"a.h\"\nvoid NeverUsed::f() {}\nNeverUsed::NeverUsed(const NeverUsed &) {}\n"
                                      "void Base::g() {}\nBase::~Base() {}\nvoid Derived::g() {}\n"
                                      "int main() { Derived d; d.g(); return OUTER_MACRO + data(); }\n");
        const QString db = QStringLiteral("[{\"directory\": \"%1\", \"file\": \"a.cpp\", \"arguments\": [\"c++\", \"-std=c++20\", \"-c\", \"a.cpp\"]}]").arg(dir.path());
        write(QStringLiteral("compile_commands.json"), db.toUtf8());

        const ProjectSetup setup = Project::discover(dir.filePath(QStringLiteral("compile_commands.json")));
        QCOMPARE(QFileInfo(setup.root).canonicalFilePath(), QFileInfo(dir.path()).canonicalFilePath());
        CodeModel e;
        QVERIFY(Indexer::run(setup, e).ok);
        QVERIFY(e.stats().projectFiles >= 3);
        QVERIFY(e.stats().symbols < 100);   // not the standard library
        const Analysis::Report r = Analysis::analyze(e);
        auto reported = [&](Analysis::FindingKind k, const QString &name) {
            for (const Analysis::Finding &f : r.findings) {
                if (f.kind == k && f.title == name)
                    return true;
            }
            return false;
        };
        QVERIFY(reported(Analysis::FindingKind::Unused, QStringLiteral("NeverUsed")));
        QVERIFY(!reported(Analysis::FindingKind::NonVirtualDestructor, QStringLiteral("Base")));
        QVERIFY(!reported(Analysis::FindingKind::Unused, QStringLiteral("INNER_MACRO")));
        for (const Analysis::Finding &f : r.findings)
            QVERIFY2(f.kind != Analysis::FindingKind::OrphanFile, qPrintable(f.title));
    }

    // samples/pyshop through the Python front end: the same model, the same
    // analyses
    void python()
    {
        if (Python::interpreter().isEmpty())
            QSKIP("no python3");
        const ProjectSetup setup = Project::discover(QStringLiteral(UNRAVEL_PYTHON_SAMPLE_DIR));
        QVERIFY2(setup.error.isEmpty(), qPrintable(setup.error));
        QVERIFY(setup.units.isEmpty());
        QCOMPARE(setup.pythonFiles.size(), 4);
        CodeModel p;
        const IndexResult r = Indexer::run(setup, p);
        QVERIFY(r.ok);
        auto find = [&](const QString &qualified) -> qint32 {
            for (qint32 i = 0; i < p.symbols.size(); ++i) {
                if (p.symbols[i].qualified == qualified && !p.symbols[i].has(External))
                    return i;
            }
            return -1;
        };
        auto edge = [&](const QString &from, const QString &to, RefKind kind) {
            const qint32 a = find(from), b = find(to);
            if (a < 0 || b < 0)
                return false;
            for (qint32 e : p.outEdges[a]) {
                if (p.edges[e].to == b && p.edges[e].kind == kind)
                    return true;
            }
            return false;
        };
        const qint32 order = find(QStringLiteral("shop.orders.Order"));
        QVERIFY(order >= 0);
        QCOMPARE(p.symbols[order].kind, SymbolKind::Class);
        QCOMPARE(p.symbols[find(QStringLiteral("shop.orders.Order.__init__"))].kind, SymbolKind::Constructor);
        QCOMPARE(p.symbols[find(QStringLiteral("shop.orders.Order.add"))].kind, SymbolKind::Method);
        QVERIFY(p.symbols[find(QStringLiteral("shop.orders.main"))].has(EntryPoint));
        // Through the annotated field: self.catalog.find() is Catalog.find
        QVERIFY(edge(QStringLiteral("shop.orders.Order.add"), QStringLiteral("shop.catalog.Catalog.find"), RefKind::Call));
        QVERIFY(edge(QStringLiteral("shop.orders.main"), QStringLiteral("shop.orders.Order.__init__"), RefKind::Call));
        QVERIFY(edge(QStringLiteral("shop.orders.Order"), QStringLiteral("shop.orders.Base"), RefKind::Inherit));
        QVERIFY(edge(QStringLiteral("shop.orders.Order.total"), QStringLiteral("shop.orders.Base.total"), RefKind::Override));

        const Analysis::Report pr = Analysis::analyze(p);
        auto reported = [&](Analysis::FindingKind k, const QString &qualified) {
            const qint32 s = find(qualified);
            for (const Analysis::Finding &f : pr.findings) {
                if (f.kind == k && ((s >= 0 && f.symbols.contains(s)) || f.title.contains(qualified)))
                    return true;
            }
            return false;
        };
        using K = Analysis::FindingKind;
        QVERIFY(reported(K::Duplicate, QStringLiteral("make_slug")));
        QVERIFY(reported(K::Unused, QStringLiteral("shop.orders.legacy_total")));
        QVERIFY(!reported(K::Unused, QStringLiteral("shop.orders.Order.total")));   // overrides
        QVERIFY(!reported(K::Unused, QStringLiteral("shop.orders.main")));
        QVERIFY(reported(K::UnreadField, QStringLiteral("shop.orders.Order.audit")));
        QVERIFY(reported(K::GlobalState, QStringLiteral("shop.catalog._registry_size")));
        QVERIFY(reported(K::ManyParameters, QStringLiteral("shop.orders.Order.summary")));
        bool importCycle = false;
        for (const Analysis::Finding &f : pr.findings) {
            if (f.kind == K::IncludeCycle && f.title.contains(QLatin1String("modules import each other")))
                importCycle = true;
            QVERIFY2(f.kind != K::NonVirtualDestructor, qPrintable(f.title));
            QVERIFY2(f.kind != K::ParseErrors, qPrintable(f.title));
        }
        QVERIFY(importCycle);
    }

    // Commits mapped to the symbols they changed, through later commits that
    // moved the lines, and what isn't committed yet
    void history()
    {
        QTemporaryDir dir;
        auto write = [&](const QString &name, const QByteArray &text) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(text);
        };
        auto git = [&](const QStringList &args) {
            QProcess p;
            p.setWorkingDirectory(dir.path());
            p.start(QStringLiteral("git"), QStringList{QStringLiteral("-c"), QStringLiteral("user.name=t"), QStringLiteral("-c"),
                                                     QStringLiteral("user.email=t@t"), QStringLiteral("-c"),
                                                     QStringLiteral("commit.gpgsign=false")} + args);
            QVERIFY(p.waitForFinished());
            QCOMPARE(p.exitCode(), 0);
        };
        git({QStringLiteral("init"), QStringLiteral("-q")});
        write(QStringLiteral("a.h"), "#pragma once\nclass Foo {\npublic:\n    int bar();\n    int baz();\n    int count = 0;\n};\n");
        write(QStringLiteral("a.cpp"), "#include \"a.h\"\n\nint Foo::bar()\n{\n    return 1;\n}\n\nint Foo::baz()\n{\n    return 2;\n}\n");
        git({QStringLiteral("add"), QStringLiteral(".")});
        git({QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("one")});
        // Two: bar's body changes, a new function at the end
        write(QStringLiteral("a.cpp"), "#include \"a.h\"\n\nint Foo::bar()\n{\n    return count + 1;\n}\n\nint Foo::baz()\n{\n    return 2;\n}\n\n"
                                      "int helper()\n{\n    return 3;\n}\n");
        git({QStringLiteral("commit"), QStringLiteral("-qam"), QStringLiteral("two")});
        // Three: lines at the top move everything down
        write(QStringLiteral("a.cpp"), "// A file\n// with a header\n\n#include \"a.h\"\n\nint Foo::bar()\n{\n    return count + 1;\n}\n\n"
                                      "int Foo::baz()\n{\n    return 2;\n}\n\nint helper()\n{\n    return 3;\n}\n");
        git({QStringLiteral("commit"), QStringLiteral("-qam"), QStringLiteral("three")});
        // Not committed: baz
        write(QStringLiteral("a.cpp"), "// A file\n// with a header\n\n#include \"a.h\"\n\nint Foo::bar()\n{\n    return count + 1;\n}\n\n"
                                      "int Foo::baz()\n{\n    return 4;\n}\n\nint helper()\n{\n    return 3;\n}\n");

        const QString repo = History::repositoryRoot(dir.path());
        QCOMPARE(QFileInfo(repo).canonicalFilePath(), QFileInfo(dir.path()).canonicalFilePath());
        const QVector<History::Commit> log = History::log(repo);
        QCOMPARE(log.size(), 3);
        QCOMPARE(log.first().subject, QStringLiteral("three"));

        ProjectSetup setup = Project::discover(repo);
        CodeModel h;
        QVERIFY(Indexer::run(setup, h).ok);
        auto names = [&](const QStringList &commits) {
            const History::Mapping map = History::touchedSymbols(h, History::changes(repo, commits, log));
            QStringList out;
            for (const History::Touched &t : map.symbols)
                out << h.displayName(t.symbol) + (t.added ? QStringLiteral(" +") : QString());
            out.sort();
            return out;
        };
        // "two", seen through "three"'s shift
        QCOMPARE(names({log[1].hash}), (QStringList{QStringLiteral("Foo::bar"), QStringLiteral("helper +")}));
        QCOMPARE(names({History::kWorkingTree}), QStringList{QStringLiteral("Foo::baz")});
        // "one" made everything
        const QStringList all = names({log[2].hash});
        QVERIFY(all.contains(QStringLiteral("Foo +")));
        QVERIFY(all.contains(QStringLiteral("Foo::count +")));

        // Line mapping through hunks
        const QVector<History::Hunk> hunks = {{0, 0, 1, 3}, {5, 1, 8, 2}};   // 3 lines at the top, line 5 became 2
        QCOMPARE(History::mapLine(hunks, 1), 4);
        QCOMPARE(History::mapLine(hunks, 4), 7);
        bool changed = false;
        QCOMPARE(History::mapLine(hunks, 5, &changed), 8);
        QVERIFY(changed);
        QCOMPARE(History::mapLine(hunks, 6), 10);
    }

    void fileActivity()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QByteArray output;
        qint64 time = 1700000000;
        QString email = QStringLiteral("one@example.test");
        auto git = [&](const QStringList &args) {
            QProcess p;
            p.setWorkingDirectory(dir.path());
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("GIT_AUTHOR_DATE"), QStringLiteral("@%1 +0000").arg(time));
            env.insert(QStringLiteral("GIT_COMMITTER_DATE"), QStringLiteral("@%1 +0000").arg(time));
            env.insert(QStringLiteral("GIT_AUTHOR_NAME"), QStringLiteral("Test"));
            env.insert(QStringLiteral("GIT_AUTHOR_EMAIL"), email);
            p.setProcessEnvironment(env);
            p.start(QStringLiteral("git"), QStringList{QStringLiteral("-c"), QStringLiteral("user.name=Test"),
                        QStringLiteral("-c"), QStringLiteral("user.email=committer@example.test"),
                        QStringLiteral("-c"), QStringLiteral("commit.gpgsign=false"),
                        QStringLiteral("-c"), QStringLiteral("core.autocrlf=false"),
                        QStringLiteral("-c"), QStringLiteral("core.hooksPath=") + dir.filePath(QStringLiteral("no-hooks"))}
                    + args);
            QVERIFY(p.waitForFinished(10000));
            const QByteArray error = p.readAllStandardError();
            QVERIFY2(p.exitCode() == 0, error.constData());
            output = p.readAllStandardOutput();
        };
        auto write = [&](const QString &path, const QByteArray &text) {
            QFile f(dir.filePath(path));
            QVERIFY(f.open(QIODevice::WriteOnly));
            QCOMPARE(f.write(text), text.size());
        };
        auto commit = [&](const QString &message) {
            git({QStringLiteral("add"), QStringLiteral("-A")});
            git({QStringLiteral("commit"), QStringLiteral("-qm"), message});
        };
        git({QStringLiteral("init"), QStringLiteral("-qb"), QStringLiteral("main")});
        write(QStringLiteral("a.cpp"), "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n");
        write(QStringLiteral("deleted.cpp"), "gone\n");
        write(QStringLiteral("renamed.cpp"), "same\n");
        commit(QStringLiteral("old"));
        time += 1000;
        const qint64 since = time;
        write(QStringLiteral("a.cpp"), "ONE\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nTEN\n");
        write(QStringLiteral("space name.cpp"), "space\n");
#ifndef Q_OS_WIN
        const QString unusual = QStringLiteral("tab\tand\nnewline.cpp");
        write(unusual, "unusual\n");
#endif
        write(QStringLiteral("binary.dat"), QByteArray("a\0b", 3));
        git({QStringLiteral("rm"), QStringLiteral("deleted.cpp")});
        git({QStringLiteral("mv"), QStringLiteral("renamed.cpp"), QStringLiteral("destination.cpp")});
        commit(QStringLiteral("boundary, two hunks and rename"));
        git({QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
        const QString boundary = QString::fromUtf8(output).trimmed();
        time += 100;
        git({QStringLiteral("checkout"), QStringLiteral("-qb"), QStringLiteral("topic")});
        email = QStringLiteral("alias@example.test");
        write(QStringLiteral("a.cpp"), "ONE\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nTEN\neleven\n");
        commit(QStringLiteral("branch edit"));
        time += 100;
        git({QStringLiteral("checkout"), QStringLiteral("main")});
        write(QStringLiteral("other.cpp"), "other\n");
        commit(QStringLiteral("main edit"));
        time += 100;
        git({QStringLiteral("merge"), QStringLiteral("--no-ff"), QStringLiteral("-qm"),
             QStringLiteral("merge"), QStringLiteral("topic")});
        const qint64 until = time;
        write(QStringLiteral(".mailmap"), "Test <one@example.test> <alias@example.test>\n");
        write(QStringLiteral("a.cpp"), "uncommitted\n");

        const History::Activity activity = History::fileActivity(dir.path(), QStringLiteral("HEAD"), since, until);
        QVERIFY2(activity.ready(), qPrintable(activity.error));
        QVERIFY(!activity.shallow);
        const auto a = activity.files.value(QStringLiteral("a.cpp"));
        QCOMPARE(a.commits, 2);
        QCOMPARE(a.added, 3);
        QCOMPARE(a.removed, 2);
        QCOMPARE(a.authors.size(), 1);
        QCOMPARE(a.lastChanged, since + 100);
        QCOMPARE(activity.files.value(QStringLiteral("space name.cpp")).commits, 1);
        QCOMPARE(activity.files.value(QStringLiteral("deleted.cpp")).removed, 1);
        QCOMPARE(activity.files.value(QStringLiteral("renamed.cpp")).removed, 1);
        QCOMPARE(activity.files.value(QStringLiteral("destination.cpp")).added, 1);
        QCOMPARE(activity.files.value(QStringLiteral("destination.cpp")).commits, 1);
        const auto lineage = History::fileActivity(dir.path(), QStringLiteral("HEAD"), 0, until, nullptr, {}, false, true);
        QVERIFY2(lineage.ready(), qPrintable(lineage.error));
        QVERIFY(lineage.followedRenames);
        QCOMPARE(lineage.files.value(QStringLiteral("destination.cpp")).commits, 2);
        const QString nestedDirectory = dir.filePath(QStringLiteral("nested"));
        QVERIFY(QDir().mkpath(nestedDirectory));
        const auto nestedLineage = History::fileActivity(nestedDirectory, QStringLiteral("HEAD"), 0, until,
                                                         nullptr, {}, false, true);
        QVERIFY2(nestedLineage.ready(), qPrintable(nestedLineage.error));
        QCOMPARE(nestedLineage.files.value(QStringLiteral("destination.cpp")).commits, 2);
        QCOMPARE(lineage.files.value(QStringLiteral("destination.cpp")).added, 1);
        QVERIFY(activity.files.value(QStringLiteral("binary.dat")).binary);
        QCOMPARE(activity.files.value(QStringLiteral("binary.dat")).added, 0);
        QVERIFY(!activity.files.contains(QStringLiteral(".mailmap")));
        QTemporaryDir cache;
        const auto cold = History::fileActivity(dir.path(), QStringLiteral("HEAD"), since, until, nullptr, cache.path());
        QVERIFY2(cold.ready(), qPrintable(cold.error));
        QVERIFY(!cold.cached);
        const auto warm = History::fileActivity(dir.path(), QStringLiteral("HEAD"), since, until, nullptr, cache.path());
        QVERIFY(warm.cached);
        QCOMPARE(warm.files.value(QStringLiteral("a.cpp")).added, a.added);
        QCOMPARE(warm.files.value(QStringLiteral("a.cpp")).authors, a.authors);
        const QStringList cachedFiles = QDir(cache.path()).entryList({QStringLiteral("*.json")}, QDir::Files);
        QCOMPARE(cachedFiles.size(), 1);
        QFile corrupt(QDir(cache.path()).filePath(cachedFiles.first()));
        QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
        corrupt.write("{broken");
        corrupt.close();
        const auto recovered = History::fileActivity(dir.path(), QStringLiteral("HEAD"), since, until, nullptr, cache.path());
        QVERIFY(recovered.ready());
        QVERIFY(!recovered.cached);
        QVERIFY(!recovered.cacheWarning.isEmpty());
        write(QStringLiteral(".mailmap"), "");
        const auto remapped = History::fileActivity(dir.path(), QStringLiteral("HEAD"), since, until, nullptr, cache.path());
        QVERIFY(remapped.ready());
        QVERIFY(!remapped.cached);
        QCOMPARE(remapped.files.value(QStringLiteral("a.cpp")).authors.size(), 2);
#ifndef Q_OS_WIN
        QCOMPARE(activity.files.value(unusual).added, 1);
#endif
        const auto exact = History::fileActivity(dir.path(), boundary, since, since);
        QVERIFY2(exact.ready(), qPrintable(exact.error));
        QCOMPARE(exact.files.value(QStringLiteral("a.cpp")).commits, 1);
        const auto empty = History::fileActivity(dir.path(), QStringLiteral("HEAD"), until + 1, until + 100);
        QVERIFY2(empty.ready(), qPrintable(empty.error));
        QVERIFY(empty.files.isEmpty());
        const auto invalid = History::fileActivity(dir.path(), QStringLiteral("--invalid"), since, until);
        QVERIFY(!invalid.ready());
        QVERIFY(!invalid.error.isEmpty());
        QVERIFY(!History::fileActivity(dir.path(), QStringLiteral("HEAD"), until, since).error.isEmpty());
        const std::atomic_bool cancelled{true};
        const auto stopped = History::fileActivity(dir.path(), QStringLiteral("HEAD"), since, until, &cancelled);
        QVERIFY(stopped.cancelled);
        QVERIFY(!stopped.ready());
        QTemporaryDir outside;
        const auto missing = History::fileActivity(outside.path(), QStringLiteral("HEAD"), since, until);
        QVERIFY(!missing.ready());
        QVERIFY(missing.repository.isEmpty());
        QVERIFY(missing.error.isEmpty());

        QTemporaryDir clone;
        git({QStringLiteral("clone"), QStringLiteral("-q"), QStringLiteral("--depth=1"),
             QUrl::fromLocalFile(dir.path()).toString(), clone.filePath(QStringLiteral("shallow"))});
        const auto shallow = History::fileActivity(clone.filePath(QStringLiteral("shallow")),
                                                   QStringLiteral("HEAD"), since, until);
        QVERIFY2(shallow.ready(), qPrintable(shallow.error));
        QVERIFY(shallow.shallow);
    }

    void functionHunkHistory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qint64 time = 1700000000;
        QByteArray output;
        auto git = [&](const QStringList &args) {
            QProcess process;
            process.setWorkingDirectory(dir.path());
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("GIT_AUTHOR_DATE"), QStringLiteral("@%1 +0000").arg(time));
            env.insert(QStringLiteral("GIT_COMMITTER_DATE"), QStringLiteral("@%1 +0000").arg(time));
            env.insert(QStringLiteral("GIT_AUTHOR_NAME"), QStringLiteral("Test"));
            env.insert(QStringLiteral("GIT_AUTHOR_EMAIL"), QStringLiteral("test@example.test"));
            process.setProcessEnvironment(env);
            process.start(QStringLiteral("git"), QStringList{
                QStringLiteral("-c"), QStringLiteral("user.name=Test"),
                QStringLiteral("-c"), QStringLiteral("user.email=test@example.test"),
                QStringLiteral("-c"), QStringLiteral("commit.gpgsign=false")} + args);
            QVERIFY(process.waitForFinished(10000));
            const QByteArray error = process.readAllStandardError();
            QVERIFY2(process.exitCode() == 0, error.constData());
            output = process.readAllStandardOutput().trimmed();
        };
        auto write = [&](const QString &path, const QByteArray &contents) {
            QFile file(dir.filePath(path));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(contents), contents.size());
        };
        auto commit = [&](const QString &message) {
            git({QStringLiteral("add"), QStringLiteral("-A")});
            git({QStringLiteral("commit"), QStringLiteral("-qm"), message});
        };
        git({QStringLiteral("init"), QStringLiteral("-qb"), QStringLiteral("main")});
        write(QStringLiteral("a.cpp"),
              "void alpha() {\n int value = 0;\n return;\n}\n"
              "void beta() {\n return;\n}\n");
        commit(QStringLiteral("base"));
        git({QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
        const QString base = QString::fromUtf8(output);
        time += 1000;
        const qint64 since = time;
        write(QStringLiteral("a.cpp"),
              "void alpha() {\n int value = 1;\n return;\n}\n"
              "void beta() {\n return /* touched */;\n}\n");
        commit(QStringLiteral("change two functions"));
        git({QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
        const QString changed = QString::fromUtf8(output);
        time += 1000;
        git({QStringLiteral("mv"), QStringLiteral("a.cpp"), QStringLiteral("b.cpp")});
        write(QStringLiteral("b.cpp"),
              "// shifted one\n// shifted two\n"
              "void alpha() {\n int value = 2;\n return;\n}\n"
              "void beta() {\n return /* touched */;\n}\n");
        commit(QStringLiteral("replace, shift, and rename"));

        CodeModel model;
        model.root = dir.path();
        const int file = model.addFile(dir.filePath(QStringLiteral("b.cpp")));
        model.files[file].project = true;
        const int alpha = model.addSymbol(QStringLiteral("alpha"));
        model.symbols[alpha].kind = SymbolKind::Function;
        model.symbols[alpha].flags = Defined;
        model.symbols[alpha].name = model.symbols[alpha].qualified = QStringLiteral("alpha");
        model.symbols[alpha].def = {file, 3, 1, 6, 1};
        const int beta = model.addSymbol(QStringLiteral("beta"));
        model.symbols[beta].kind = SymbolKind::Function;
        model.symbols[beta].flags = Defined;
        model.symbols[beta].name = model.symbols[beta].qualified = QStringLiteral("beta");
        model.symbols[beta].def = {file, 7, 1, 9, 1};
        model.finalize();
        model.indexedAt = QDateTime::currentMSecsSinceEpoch();

        const History::HunkActivity attributed = History::functionHunks(dir.path(), QStringLiteral("HEAD"), since, since, model);
        QVERIFY2(attributed.ready(), qPrintable(attributed.error));
        QVERIFY(attributed.firstParentOnly);
        QCOMPARE(attributed.mappingCommits, 2);
        QCOMPARE(attributed.functions.value(alpha).commits, 1);
        QCOMPARE(attributed.functions.value(beta).commits, 1);
        QCOMPARE(attributed.functions.value(alpha).lines, 1);
        QCOMPARE(attributed.functions.value(beta).lines, 1);
        QVERIFY(attributed.functions.value(alpha).ambiguity.contains(QStringLiteral("replacement")));
        QVERIFY(attributed.functions.value(alpha).ambiguity.contains(QStringLiteral("rename")));
        QCOMPARE(attributed.functions.value(alpha).ambiguousCommits, 1);
        QVERIFY(attributed.functions.value(beta).ambiguity.contains(QStringLiteral("rename")));

        const auto oldRevision = History::functionHunks(dir.path(), changed, since, since, model);
        QVERIFY2(oldRevision.ready(), qPrintable(oldRevision.error));
        QCOMPARE(oldRevision.revision, changed);
        QCOMPARE(oldRevision.functions.value(alpha).commits, 1);
        QVERIFY(oldRevision.functions.value(alpha).ambiguity.contains(QStringLiteral("replacement")));
        QVERIFY(oldRevision.postRevisionChangedFiles > 0);
        QVERIFY(oldRevision.functions.value(alpha).ambiguity.contains(QStringLiteral("post-revision")));
        const auto beforeChange = History::functionHunks(dir.path(), base, since, since, model);
        QVERIFY2(beforeChange.ready(), qPrintable(beforeChange.error));
        QVERIFY(beforeChange.functions.isEmpty());
        const auto outsideWindow = History::functionHunks(dir.path(), QStringLiteral("HEAD"), since + 1, since + 100, model);
        QVERIFY2(outsideWindow.ready(), qPrintable(outsideWindow.error));
        QVERIFY(outsideWindow.functions.isEmpty());
        QCOMPARE(outsideWindow.mappingCommits, 0);

        Focus::Options options;
        options.functionHunks = &attributed;
        const auto focused = Focus::rank(model, nullptr, options);
        QVERIFY(focused.functionHistory);
        QCOMPARE(focused.entries.first().hunkCommits, 1);
        QCOMPARE(focused.entries.first().history, 1.0);
        const QJsonObject report = Focus::json(model, focused);
        QCOMPARE(report.value(QStringLiteral("mode")).toString(), QStringLiteral("historical-function-hunk-estimate"));
        QVERIFY(report.value(QStringLiteral("historySemantics")).toString().contains(QStringLiteral("merge commits are not counted")));
        QVERIFY(report.value(QStringLiteral("postRevisionPolicy")).toString().contains(QStringLiteral("not counted")));
        const QJsonObject alphaReport = report.value(QStringLiteral("functions")).toArray().first().toObject();
        QCOMPARE(alphaReport.value(QStringLiteral("ambiguousHunkCommits")).toInt(), 1);
        QVERIFY(alphaReport.value(QStringLiteral("historyAttribution")).toString()
                    .contains(QStringLiteral("merge commits are not counted")));
        QVERIFY(Focus::markdown(model, focused).contains(QStringLiteral("all reachable non-merge commits")));
        QVERIFY(Focus::markdown(model, focused).contains(QStringLiteral("merge commits are not counted")));

        QTest::qWait(5);
        write(QStringLiteral("b.cpp"),
              "// shifted one\n// shifted two\n"
              "void alpha() {\n int value = 3;\n return;\n}\n"
              "void beta() {\n return /* touched */;\n}\n");
        const auto stale = History::functionHunks(dir.path(), QStringLiteral("HEAD"), since, since, model);
        QVERIFY(!stale.ready());
        QVERIFY(stale.error.contains(QStringLiteral("current index")));
        QVERIFY(stale.functions.isEmpty());
        model.indexedAt = QDateTime::currentMSecsSinceEpoch();
        const auto workingTree = History::functionHunks(dir.path(), QStringLiteral("HEAD"), since, since, model);
        QVERIFY2(workingTree.ready(), qPrintable(workingTree.error));
        QCOMPARE(workingTree.postRevisionChangedFiles, 1);
        QCOMPARE(workingTree.functions.value(alpha).commits, 1);
        QVERIFY(workingTree.functions.value(alpha).ambiguity.contains(QStringLiteral("post-revision")));

        const int overlapping = model.addSymbol(QStringLiteral("overlapping"));
        model.symbols[overlapping].kind = SymbolKind::Function;
        model.symbols[overlapping].flags = Defined;
        model.symbols[overlapping].name = model.symbols[overlapping].qualified = QStringLiteral("overlapping");
        model.symbols[overlapping].def = {file, 4, 1, 4, 20};
        model.finalize();
        const auto overlap = History::functionHunks(dir.path(), QStringLiteral("HEAD"), since, since, model);
        QVERIFY2(overlap.ready(), qPrintable(overlap.error));
        QCOMPARE(overlap.functions.value(alpha).commits, 0);
        QVERIFY(overlap.functions.value(alpha).ambiguity.contains(QStringLiteral("overlap")));
        QVERIFY(overlap.functions.value(overlapping).ambiguity.contains(QStringLiteral("overlap")));

        write(QStringLiteral("untracked.cpp"), "int untracked() { return 0; }\n");
        CodeModel untrackedModel = model;
        const int untrackedFile = untrackedModel.addFile(dir.filePath(QStringLiteral("untracked.cpp")));
        untrackedModel.files[untrackedFile].project = true;
        const int untrackedFunction = untrackedModel.addSymbol(QStringLiteral("untracked"));
        untrackedModel.symbols[untrackedFunction].kind = SymbolKind::Function;
        untrackedModel.symbols[untrackedFunction].flags = Defined;
        untrackedModel.symbols[untrackedFunction].name = untrackedModel.symbols[untrackedFunction].qualified =
            QStringLiteral("untracked");
        untrackedModel.symbols[untrackedFunction].def = {untrackedFile, 1, 1, 1, 30};
        untrackedModel.finalize();
        untrackedModel.indexedAt = QDateTime::currentMSecsSinceEpoch();
        const auto untracked = History::functionHunks(dir.path(), QStringLiteral("HEAD"), since, since, untrackedModel);
        QVERIFY(!untracked.ready());
        QVERIFY(untracked.error.contains(QStringLiteral("untracked")));

        const std::atomic_bool cancelled{true};
        const auto stopped = History::functionHunks(dir.path(), QStringLiteral("HEAD"), since, since, model, &cancelled);
        QVERIFY(stopped.cancelled);
        QVERIFY(!stopped.ready());
        const auto invalid = History::functionHunks(dir.path(), QStringLiteral("--invalid"), since, since, model);
        QVERIFY(!invalid.ready());
        QVERIFY(!invalid.error.isEmpty());
        QTemporaryDir outside;
        const auto missing = History::functionHunks(outside.path(), QStringLiteral("HEAD"), since, since, model);
        QVERIFY(!missing.ready());
        QVERIFY(!missing.error.isEmpty());
    }

    void functionHunkMappingWorkBound()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qint64 time = 1700000000;
        QByteArray output;
        auto git = [&](const QStringList &args) {
            QProcess process;
            process.setWorkingDirectory(dir.path());
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("GIT_AUTHOR_DATE"), QStringLiteral("@%1 +0000").arg(time));
            env.insert(QStringLiteral("GIT_COMMITTER_DATE"), QStringLiteral("@%1 +0000").arg(time));
            env.insert(QStringLiteral("GIT_AUTHOR_NAME"), QStringLiteral("Test"));
            env.insert(QStringLiteral("GIT_AUTHOR_EMAIL"), QStringLiteral("test@example.test"));
            process.setProcessEnvironment(env);
            process.start(QStringLiteral("git"), QStringList{
                QStringLiteral("-c"), QStringLiteral("user.name=Test"),
                QStringLiteral("-c"), QStringLiteral("user.email=test@example.test"),
                QStringLiteral("-c"), QStringLiteral("commit.gpgsign=false")} + args);
            QVERIFY(process.waitForFinished(10000));
            const QByteArray error = process.readAllStandardError();
            QVERIFY2(process.exitCode() == 0, error.constData());
            output = process.readAllStandardOutput().trimmed();
        };
        auto write = [&](const QByteArray &contents) {
            QFile file(dir.filePath(QStringLiteral("large.cpp")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(contents), contents.size());
        };
        auto commit = [&](const QString &message) {
            git({QStringLiteral("add"), QStringLiteral("large.cpp")});
            git({QStringLiteral("commit"), QStringLiteral("-qm"), message});
        };
        git({QStringLiteral("init"), QStringLiteral("-qb"), QStringLiteral("main")});
        QByteArray base = "void large() {\n";
        QByteArray changed = base;
        for (int i = 0; i < 3000; ++i) {
            base += "int value" + QByteArray::number(i) + " = 0;\n";
            changed += "int value" + QByteArray::number(i) + " = 1;\n";
        }
        base += "}\n";
        changed += "}\n";
        write(base);
        commit(QStringLiteral("base"));
        time += 1000;
        const qint64 since = time;
        write(changed);
        commit(QStringLiteral("window change"));
        time += 1000;
        QByteArray later = "void large() {\n";
        for (int i = 0; i < 3000; ++i)
            later += "int value" + QByteArray::number(i) + (i % 2 == 0 ? " = 2;\n" : " = 1;\n");
        later += "}\n";
        write(later);
        commit(QStringLiteral("many separated replacements"));

        CodeModel model;
        model.root = dir.path();
        const int file = model.addFile(dir.filePath(QStringLiteral("large.cpp")));
        model.files[file].project = true;
        const int function = model.addSymbol(QStringLiteral("large"));
        model.symbols[function].kind = SymbolKind::Function;
        model.symbols[function].flags = Defined;
        model.symbols[function].name = model.symbols[function].qualified = QStringLiteral("large");
        model.symbols[function].def = {file, 1, 1, 3002, 1};
        model.finalize();
        model.indexedAt = QDateTime::currentMSecsSinceEpoch();

        const auto result = History::functionHunks(dir.path(), QStringLiteral("HEAD"), since, since, model);
        QVERIFY(!result.ready());
        QVERIFY(result.error.contains(QStringLiteral("two million operations")));
        QVERIFY(result.functions.isEmpty());
    }

    void functionHunkRecentWindowBoundsTraversal()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QProcess init;
        init.setWorkingDirectory(dir.path());
        init.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("-q")});
        QVERIFY(init.waitForFinished(10000));
        QCOMPARE(init.exitCode(), 0);
        QProcess head;
        head.setWorkingDirectory(dir.path());
        head.start(QStringLiteral("git"), {QStringLiteral("symbolic-ref"), QStringLiteral("HEAD"),
                                            QStringLiteral("refs/heads/main")});
        QVERIFY(head.waitForFinished(10000));
        QCOMPARE(head.exitCode(), 0);

        QByteArray stream;
        auto appendCommit = [&](int mark, qint64 timestamp, const QByteArray &message,
                                const QString &path, const QByteArray &contents) {
            stream += "commit refs/heads/main\nmark :" + QByteArray::number(mark) + "\ncommitter Test <test@example.test> "
                + QByteArray::number(timestamp) + " +0000\ndata " + QByteArray::number(message.size()) + "\n"
                + message + "\n";
            if (!path.isEmpty())
                stream += "M 100644 inline " + path.toUtf8() + "\ndata "
                    + QByteArray::number(contents.size()) + "\n" + contents + "\n";
            stream += "\n";
        };
        appendCommit(1, 1700000000, "base", QStringLiteral("source.cpp"), "int recent() { return 0; }\n");
        for (int i = 0; i < 10000; ++i) {
            const QByteArray contents = "noise " + QByteArray::number(i) + "\n";
            appendCommit(i + 2, 1699990000 - i, "out-of-window", QStringLiteral("noise.txt"), contents);
        }
        appendCommit(10002, 1700001000, "window commit", QStringLiteral("source.cpp"), "int recent() { return 1; }\n");
        appendCommit(10003, 1699970000, "clock-skewed tip", QStringLiteral("noise.txt"), "tip\n");

        QProcess fastImport;
        fastImport.setWorkingDirectory(dir.path());
        fastImport.start(QStringLiteral("git"), {QStringLiteral("fast-import"), QStringLiteral("--quiet")});
        QVERIFY(fastImport.waitForStarted(10000));
        QCOMPARE(fastImport.write(stream), qint64(stream.size()));
        fastImport.closeWriteChannel();
        QVERIFY(fastImport.waitForFinished(60000));
        const QByteArray importError = fastImport.readAllStandardError();
        QVERIFY2(fastImport.exitCode() == 0, importError.constData());
        QProcess checkout;
        checkout.setWorkingDirectory(dir.path());
        checkout.start(QStringLiteral("git"), {QStringLiteral("checkout"), QStringLiteral("-f"), QStringLiteral("main")});
        QVERIFY(checkout.waitForFinished(10000));
        const QByteArray checkoutError = checkout.readAllStandardError();
        QVERIFY2(checkout.exitCode() == 0, checkoutError.constData());

        CodeModel model;
        model.root = dir.path();
        const int file = model.addFile(dir.filePath(QStringLiteral("source.cpp")));
        model.files[file].project = true;
        const int function = model.addSymbol(QStringLiteral("recent"));
        model.symbols[function].kind = SymbolKind::Function;
        model.symbols[function].flags = Defined;
        model.symbols[function].name = model.symbols[function].qualified = QStringLiteral("recent");
        model.symbols[function].def = {file, 1, 1, 1, 40};
        model.finalize();
        model.indexedAt = QDateTime::currentMSecsSinceEpoch();

        const auto recent = History::functionHunks(dir.path(), QStringLiteral("HEAD"), 1700001000, 1700001000, model);
        QVERIFY2(recent.ready(), qPrintable(recent.error));
        QCOMPARE(recent.mappingCommits, 2);
        QCOMPARE(recent.functions.value(function).commits, 1);
        const auto empty = History::functionHunks(dir.path(), QStringLiteral("HEAD"), 1700002000, 1700003000, model);
        QVERIFY2(empty.ready(), qPrintable(empty.error));
        QCOMPARE(empty.mappingCommits, 0);
        QVERIFY(empty.functions.isEmpty());
        const auto tooWide = History::functionHunks(dir.path(), QStringLiteral("HEAD"), 0, 1700005000, model);
        QVERIFY(!tooWide.ready());
        QVERIFY(tooWide.error.contains(QStringLiteral("line-mapping span exceeds 10,000 commits")));
    }

    void qtEventsAndCallEvidence()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("events.cpp"));
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write(
            "class QObject { public:\n"
            " template<class S, class Signal, class R, class Slot>\n"
            " static void connect(S*, Signal, R*, Slot) {} };\n"
            "class Source { public: void ping() {} };\n"
            "class Receiver { public: void handle() {} };\n"
            "class Base { public: virtual void go() {} };\n"
            "void dispatch(Base *p) { Base b; b.go(); p->go(); }\n"
            "void addressSelf() { auto p = &addressSelf; }\n"
            "void addressOnly() { auto p = &Receiver::handle; }\n"
            "void setup(Source* s, Receiver* r) {\n"
            " QObject::connect(s, &Source::ping, r, &Receiver::handle);\n"
            " QObject::connect(s, &Source::ping, r, [r] { r->handle(); });\n"
            " QObject::connect(s, \"ping\", r, \"handle\");\n"
            " r->handle();\n"
            "}\n");
        source.close();
        CodeModel model;
        QVERIFY(Indexer::run(Project::discover(dir.path()), model).ok);
        QCOMPARE(model.eventConnections.size(), 3);
        const auto first = model.eventConnections.first();
        QVERIFY(first.signal >= 0);
        QVERIFY(first.target >= 0);
        QCOMPARE(model.symbols[first.signal].qualified, QStringLiteral("Source::ping"));
        QCOMPARE(model.symbols[first.target].qualified, QStringLiteral("Receiver::handle"));
        QVERIFY(first.loc.valid());
        QCOMPARE(model.eventConnections[1].target, -1);
        QVERIFY(!model.eventConnections[1].limitation.isEmpty());
        QCOMPARE(model.eventConnections[2].signal, -1);
        for (int i = 0; i < model.symbols.size(); ++i) {
            if (model.symbols[i].qualified == QLatin1String("addressOnly"))
                QVERIFY(Analysis::callsFrom(model, i, true).isEmpty());
            if (model.symbols[i].qualified == QLatin1String("setup")) {
                for (const auto &call : Analysis::callsFrom(model, i, true))
                    QVERIFY(model.symbols[call.callee].qualified != QLatin1String("Source::ping"));
            }
            if (model.symbols[i].qualified == QLatin1String("dispatch")) {
                bool found = false;
                for (const auto &call : Analysis::callsFrom(model, i, true)) {
                    if (model.symbols[call.callee].qualified == QLatin1String("Base::go")) {
                        QCOMPARE(call.count, 2);
                        QVERIFY(call.virtualCall);
                        found = true;
                    }
                }
                QVERIFY(found);
            }
            if (model.symbols[i].qualified == QLatin1String("addressSelf")) {
                QVERIFY(Analysis::callsFrom(model, i, true).isEmpty());
                for (const auto &finding : Analysis::analyze(model).findings)
                    QVERIFY(finding.kind != Analysis::FindingKind::Recursion || !finding.symbols.contains(i));
            }
        }
        const QString saved = dir.filePath(QStringLiteral("events.idx"));
        QVERIFY(model.save(saved));
        CodeModel loaded;
        QVERIFY(loaded.load(saved));
        QCOMPARE(loaded.eventConnections.size(), model.eventConnections.size());
        QCOMPARE(loaded.eventConnections.first().loc, first.loc);
        QCOMPARE(loaded.eventConnections.first().signal, first.signal);
    }

    void reviewFocus()
    {
        CodeModel model;
        model.root = QDir::tempPath();
        const int file = model.addFile(model.root + QStringLiteral("/focus.cpp"));
        model.files[file].project = true;
        for (int i = 0; i < 25; ++i) {
            const int id = model.addSymbol(QStringLiteral("function:%1").arg(i));
            auto &symbol = model.symbols[id];
            symbol.kind = SymbolKind::Function;
            symbol.flags = Defined;
            symbol.name = symbol.qualified = QStringLiteral("f%1").arg(i, 2, 10, QLatin1Char('0'));
            symbol.def = {file, i + 1, 1, i + 1, 10};
            symbol.metrics.complexity = i == 24 ? 90 : 1;
        }
        model.refs.append({0, 3, RefKind::Call, 0, {file, 1, 1, 1, 2}});
        model.finalize();
        const auto result = Focus::rank(model);
        QCOMPARE(result.entries.size(), 20);
        QCOMPARE(result.eligible, 25);
        QCOMPARE(result.entries.first().symbol, 24);
        QVERIFY(result.entries.first().priority > 0);
        QCOMPARE(result.entries[1].symbol, 0);
        QSet<qint32> unique;
        for (const auto &entry : result.entries)
            unique.insert(entry.symbol);
        QCOMPARE(unique.size(), 20);
        Focus::Options allOptions;
        allOptions.limit = 0;
        const auto all = Focus::rank(model, nullptr, allOptions);
        QCOMPARE(all.entries.size(), 25);
        for (int i = 0; i < result.entries.size(); ++i)
            QCOMPARE(result.entries[i].symbol, all.entries[i].symbol);
        QCOMPARE(Focus::json(model, result).value(QStringLiteral("functions")).toArray().size(), 20);
        Focus::Options options;
        options.changedOnly = true;
        QVERIFY(Focus::rank(model, nullptr, options).entries.isEmpty());
        options.touched = {3};
        QCOMPARE(Focus::rank(model, nullptr, options).entries.first().symbol, 3);
        options.impactDepth = 1;
        const auto impact = Focus::rank(model, nullptr, options);
        QCOMPARE(impact.entries.size(), 2);
        QCOMPARE(Focus::json(model, impact).value(QStringLiteral("potentialImpactDepth")).toInt(), 1);
        QVERIFY(Focus::markdown(model, impact).contains(QStringLiteral("not proven execution impact")));
        History::Activity history;
        history.repository = model.root;
        history.revision = QStringLiteral("test-revision");
        history.shallow = true;
        history.files[QStringLiteral("focus.cpp")].commits = 3;
        const auto historical = Focus::rank(model, &history);
        QVERIFY(historical.historical);
        QVERIFY(historical.partial);
        QCOMPARE(historical.entries.first().history, 2.0);
        QVERIFY(Focus::markdown(model, historical).contains(QStringLiteral("file-level pathname proxy over all reachable non-merge commits")));
        QVERIFY(Focus::markdown(model, historical).contains(QStringLiteral("all reachable non-merge commits")));
        QVERIFY(Focus::json(model, historical).value(QStringLiteral("historySemantics")).toString()
                    .contains(QStringLiteral("all reachable non-merge commits")));

        History::HunkActivity failedHunks;
        failedHunks.repository = model.root;
        failedHunks.revision = QStringLiteral("test-revision");
        failedHunks.error = QStringLiteral("Function hunk history exceeded its configured bound");
        Focus::Options hunkOptions;
        hunkOptions.functionHunks = &failedHunks;
        const auto failedHistory = Focus::rank(model, &history, hunkOptions);
        QVERIFY(!failedHistory.error.isEmpty());
        QVERIFY(failedHistory.entries.isEmpty());
        QCOMPARE(Focus::json(model, failedHistory).value(QStringLiteral("mode")).toString(),
                 QStringLiteral("historical-function-hunk-estimate"));

        CodeModel outsideModel = model;
        const int outsideFile = outsideModel.addFile(QDir::rootPath() + QStringLiteral("unravel-outside.cpp"));
        outsideModel.files[outsideFile].project = true;
        const int outsideFunction = outsideModel.addSymbol(QStringLiteral("outside"));
        outsideModel.symbols[outsideFunction].kind = SymbolKind::Function;
        outsideModel.symbols[outsideFunction].flags = Defined;
        outsideModel.symbols[outsideFunction].name = outsideModel.symbols[outsideFunction].qualified =
            QStringLiteral("outside");
        outsideModel.symbols[outsideFunction].def = {outsideFile, 1, 1, 1, 10};
        outsideModel.finalize();
        outsideModel.files[outsideFile].project = true;
        History::HunkActivity availableHunks;
        availableHunks.repository = model.root;
        availableHunks.revision = QStringLiteral("test-revision");
        Focus::Options outsideOptions;
        outsideOptions.functionHunks = &availableHunks;
        const auto outsideResult = Focus::rank(outsideModel, nullptr, outsideOptions);
        QVERIFY2(outsideResult.error.contains(QStringLiteral("outside the history repository")),
                 qPrintable(QStringLiteral("error='%1' entries=%2 eligible=%3 repo='%4' path='%5' relative='%6'")
                     .arg(outsideResult.error).arg(outsideResult.entries.size()).arg(outsideResult.eligible)
                     .arg(availableHunks.repository, outsideModel.files[outsideFile].path,
                          QDir(availableHunks.repository).relativeFilePath(outsideModel.files[outsideFile].path))));
        QVERIFY(outsideResult.entries.isEmpty());
    }

    // Without a database: <plugin/tools/x.h> from another plugin resolves,
    // so its types are known and linked to
    void guessedFolderIncludes()
    {
        QTemporaryDir dir;
        auto write = [&](const QString &name, const QByteArray &text) {
            QVERIFY(QDir().mkpath(QFileInfo(dir.filePath(name)).absolutePath()));
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(text);
        };
        write(QStringLiteral("src/plugins/canvas/tools/controller.h"),
              "#pragma once\nnamespace Canvas { class Controller { public: void change(); }; }\n");
        write(QStringLiteral("src/plugins/gradient/observer.cpp"),
              "#include <canvas/tools/controller.h>\n"
              "class Observer { public: Observer(Canvas::Controller &c) : m_c{c} {} void on() { m_c.change(); }\n"
              "private: Canvas::Controller &m_c; };\n");
        const ProjectSetup setup = Project::discover(dir.path());
        QCOMPARE(setup.source, ProjectSetup::Guessed);
        QCOMPARE(setup.units.size(), 1);
        QVERIFY2(setup.units.first().args.contains(QStringLiteral("-I") + dir.filePath(QStringLiteral("src/plugins"))),
                 qPrintable(setup.units.first().args.join(QLatin1Char(' '))));
        CodeModel g;
        QVERIFY(Indexer::run(setup, g).ok);
        qint32 observer = -1, controller = -1;
        for (qint32 i = 0; i < g.symbols.size(); ++i) {
            if (g.symbols[i].qualified == QLatin1String("Observer"))
                observer = i;
            else if (g.symbols[i].qualified == QLatin1String("Canvas::Controller"))
                controller = i;
        }
        QVERIFY(observer >= 0 && controller >= 0);
        bool linked = false;
        for (const Edge &e : g.edges)
            linked = linked || (g.nodeOwner(e.from) == observer && g.nodeOwner(e.to) == controller);
        QVERIFY(linked);
        for (const Diagnostic &d : g.diagnostics)
            QVERIFY2(d.severity < Diagnostic::Error, qPrintable(d.message));
    }

    // The CMake a build of the project was configured with comes first
    void cmakeOfAnExistingBuild()
    {
#ifdef Q_OS_UNIX
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("build/Desktop_Kit"))));
        const QString cmake = QStringLiteral("/bin/sh");   // anything executable
        QFile cache(dir.filePath(QStringLiteral("build/Desktop_Kit/CMakeCache.txt")));
        QVERIFY(cache.open(QIODevice::WriteOnly));
        cache.write(QStringLiteral("CMAKE_COMMAND:INTERNAL=%1\nCMAKE_HOME_DIRECTORY:INTERNAL=%2\n"
                                   "CMAKE_CXX_COMPILER:STRING=/bin/g++-14\nCMAKE_BUILD_TYPE:STRING=Debug\n")
                        .arg(cmake, dir.path()).toUtf8());
        cache.close();
        const Project::CMakeChoice choice = Project::cmakeFor(dir.path());
        QCOMPARE(choice.executable, cmake);
        // Its compiler comes along, not what is about that build only
        QCOMPARE(choice.arguments, QStringList{QStringLiteral("-DCMAKE_CXX_COMPILER=/bin/g++-14")});
        // A build of another source tree doesn't count
        QVERIFY(cache.open(QIODevice::WriteOnly));
        cache.write(QStringLiteral("CMAKE_COMMAND:INTERNAL=%1\nCMAKE_HOME_DIRECTORY:INTERNAL=/elsewhere\n").arg(cmake).toUtf8());
        cache.close();
        QVERIFY(Project::cmakeFor(dir.path()).executable != cmake);
#else
        QSKIP("Uses /bin/sh as the CMake");
#endif
    }

    void cleanArguments()
    {
        const QStringList argv = {"/usr/bin/c++", "-Iinclude", "-DX=1", "-o", "out.o", "-c", "src/a.cpp", "-MD", "-MF", "a.d",
                                  "-Werror", "-fno-fat-lto-objects", "-std=c++20"};
        const QStringList out = Project::cleanArguments(argv, QStringLiteral("/p/src/a.cpp"), QStringLiteral("/p"));
        QVERIFY(out.contains(QStringLiteral("-I/p/include")));
        QVERIFY(out.contains(QStringLiteral("-DX=1")));
        QVERIFY(out.contains(QStringLiteral("-std=c++20")));
        QVERIFY(!out.contains(QStringLiteral("-o")) && !out.contains(QStringLiteral("out.o")));
        QVERIFY(!out.contains(QStringLiteral("src/a.cpp")) && !out.contains(QStringLiteral("-c")));
        QVERIFY(!out.contains(QStringLiteral("-MF")) && !out.contains(QStringLiteral("a.d")));
        QVERIFY(!out.contains(QStringLiteral("-Werror")));
        QVERIFY(out.contains(QStringLiteral("-working-directory=/p")));
        const QStringList minimal = Project::minimalArguments(out);
        QVERIFY(minimal.contains(QStringLiteral("-I/p/include")));
        QVERIFY(!minimal.contains(QStringLiteral("-fno-fat-lto-objects")));
    }

    void layered()
    {
        QVector<Layout::Node> nodes(6);
        QVector<Layout::Link> links = {{0, 1}, {1, 2}, {2, 0}, {0, 3}, {3, 4}, {1, 5}, {0, 4}};
        Layout::layered(nodes, links);
        // No two nodes overlap
        for (int i = 0; i < nodes.size(); ++i) {
            for (int j = i + 1; j < nodes.size(); ++j) {
                const QRectF a(nodes[i].x, nodes[i].y, nodes[i].w, nodes[i].h), b(nodes[j].x, nodes[j].y, nodes[j].w, nodes[j].h);
                QVERIFY(!a.intersects(b));
            }
        }
        // Every edge has a route, the cycle is broken once
        int reversed = 0;
        for (const Layout::Link &l : links) {
            QVERIFY(l.points.size() >= 2);
            reversed += l.reversed;
        }
        QCOMPARE(reversed, 1);
        // Links go forward through the layers
        QVERIFY(nodes[1].layer > nodes[0].layer);
        QVERIFY(nodes[4].layer > nodes[3].layer);
    }

    void squarify()
    {
        const QVector<double> values = {6, 6, 4, 3, 2, 2, 1};
        const QRectF r(0, 0, 600, 400);
        const QVector<QRectF> rects = Layout::squarify(values, r);
        double area = 0;
        for (int i = 0; i < rects.size(); ++i) {
            area += rects[i].width() * rects[i].height();
            QVERIFY(r.adjusted(-0.01, -0.01, 0.01, 0.01).contains(rects[i]));
            QVERIFY(std::max(rects[i].width(), rects[i].height()) / std::min(rects[i].width(), rects[i].height()) < 4);
        }
        QVERIFY(qAbs(area - 600 * 400) < 1);
    }
};

QTEST_GUILESS_MAIN(TestCore)
#include "tst_core.moc"
