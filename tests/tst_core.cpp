// The engine on samples/warehouse, a small program with known flaws:
// clang must find its structure, and the analyses its problems.

#include <QFile>
#include <QProcess>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "analysis.h"
#include "history.h"
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
        QVERIFY(activity.files.value(QStringLiteral("binary.dat")).binary);
        QCOMPARE(activity.files.value(QStringLiteral("binary.dat")).added, 0);
        QVERIFY(!activity.files.contains(QStringLiteral(".mailmap")));
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
