#include "includeimpact.h"

#include <QtTest>

using namespace Unravel;
using namespace Unravel::IncludeImpact;

namespace {

struct Fixture : CodeModel {
    Fixture()
    {
        root = QStringLiteral("/project");
        buildDir = QStringLiteral("/project/build");
    }

    int file(const char *name, bool unit = false)
    {
        const QString path = QString::fromLatin1(name);
        const int id = addFile(path.startsWith(QLatin1Char('/')) ? path : root + QLatin1Char('/') + path);
        files[id].indexed = unit;
        return id;
    }
};

} // namespace

class TestIncludeImpact : public QObject
{
    Q_OBJECT

private slots:
    void diamondAndRank();
    void cycleAndSelfInclude();
    void includeOnlyAndLimits();
    void partialCoverage();
    void pythonOnlyUnavailable();
};

void TestIncludeImpact::diamondAndRank()
{
    Fixture m;
    const int unit = m.file("a.cpp", true);
    const int second = m.file("b.cpp", true);
    const int left = m.file("left.h");
    const int right = m.file("right.h");
    const int target = m.file("target.h");
    const int unused = m.file("unused.h");
    m.files[unit].includes = {{left, 22}, {left, 8}, {right, 4}, {left, 0}, {left, 8}};
    m.files[left].includes = {{target, 16}};
    m.files[right].includes = {{target, 12}};
    m.files[second].includes = {{target, 9}};
    m.finalize();

    const Index index(m);
    const Reach reach = index.affected(target);
    QCOMPARE(reach.units, (QVector<int>{unit, second}));
    QCOMPARE(reach.witness(unit), (QVector<Step>{{unit, left, 8}, {left, target, 16}}));
    QCOMPARE(reach.witness(second), (QVector<Step>{{second, target, 9}}));
    QVERIFY(reach.witness(left).isEmpty());
    const auto ranks = index.ranking();
    QCOMPARE(ranks.size(), 4);
    QCOMPARE(ranks.first().file, target);
    QCOMPARE(ranks.first().affectedUnits, 2);
    QCOMPARE(ranks[1].file, left);
    QCOMPARE(ranks[2].file, right);
    QCOMPARE(ranks.last().file, unused);
    QCOMPARE(ranks.last().affectedUnits, 0);
    const std::atomic_bool cancelled{true};
    QVERIFY(index.ranking(&cancelled).isEmpty());
}

void TestIncludeImpact::cycleAndSelfInclude()
{
    Fixture m;
    const int a = m.file("a.h", true);
    const int b = m.file("b.h");
    const int unit = m.file("u.cpp", true);
    const int self = m.file("self.h");
    m.files[unit].includes = {{b, 4}};
    m.files[a].includes = {{b, 7}};
    m.files[b].includes = {{a, 11}};
    m.files[self].includes = {{self, 2}};
    m.finalize();

    const Index index(m);
    const Reach reach = index.affected(a);
    QCOMPARE(reach.units, QVector<int>{unit});
    QCOMPARE(reach.witness(unit), (QVector<Step>{{unit, b, 4}, {b, a, 11}}));
    QCOMPARE(index.affected(b).units, (QVector<int>{a, unit}));
    QCOMPARE(index.cycle(a).componentSize, 2);
    QCOMPARE(index.cycle(a).witness, (QVector<Step>{{a, b, 7}, {b, a, 11}}));
    QCOMPARE(index.cycle(self).componentSize, 1);
    QCOMPARE(index.cycle(self).witness, (QVector<Step>{{self, self, 2}}));
    QVERIFY(index.cycle(unit).witness.isEmpty());
    const Graph loop = index.graph(a, Direction::Includes, {1, 2});
    QCOMPARE(loop.edges, (QVector<Step>{{a, b, 7}, {b, a, 11}}));
    QVERIFY(!loop.depthTruncated && !loop.nodesTruncated);
    const auto ranks = index.ranking();
    QCOMPARE(ranks.first().file, b);
    QCOMPARE(ranks.first().affectedUnits, 2);
    QCOMPARE(ranks[1].affectedUnits, 1);
    QVERIFY(ranks[0].inCycle && ranks[1].inCycle && ranks[2].inCycle);
}

void TestIncludeImpact::includeOnlyAndLimits()
{
    Fixture m;
    const int main = m.file("main.cpp", true);
    const int semanticOnly = m.file("semantic.cpp", true);
    const int wrapper = m.file("wrapper.h");
    const int target = m.file("target.h");
    m.files[main].includes = {{wrapper, 3}};
    m.files[wrapper].includes = {{target, 10}};
    const int f = m.addSymbol(QStringLiteral("fn"));
    const int t = m.addSymbol(QStringLiteral("type"));
    m.symbols[f].kind = SymbolKind::Function;
    m.symbols[f].def = {semanticOnly, 3, 1, 5, 1};
    m.symbols[t].kind = SymbolKind::Class;
    m.symbols[t].def = {target, 2, 1, 2, 10};
    m.refs.append({f, t, RefKind::TypeUse, 0, {semanticOnly, 4, 1}});
    m.finalize();
    QVERIFY(!m.edges.isEmpty());

    const Index index(m);
    QCOMPARE(index.affected(target).units, QVector<int>{main});
    const Graph depth = index.graph(target, Direction::Includers, {1, 10});
    QCOMPARE(depth.files, (QVector<int>{target, wrapper}));
    QCOMPARE(depth.edges, (QVector<Step>{{wrapper, target, 10}}));
    QVERIFY(depth.depthTruncated);
    QVERIFY(!depth.nodesTruncated);
    const Graph nodes = index.graph(target, Direction::Includers, {5, 2});
    QVERIFY(!nodes.depthTruncated);
    QVERIFY(nodes.nodesTruncated);
    const Graph full = index.graph(target, Direction::Includers, {-1, -1});
    QCOMPARE(full.files, (QVector<int>{target, wrapper, main}));
    QVERIFY(!full.depthTruncated && !full.nodesTruncated);
    const Graph forward = index.graph(main, Direction::Includes, {-1, -1});
    QCOMPARE(forward.files, (QVector<int>{main, wrapper, target}));
    QCOMPARE(forward.edges, (QVector<Step>{{main, wrapper, 3}, {wrapper, target, 10}}));
    const Graph empty = index.graph(target, Direction::Includers, {0, 0});
    QVERIFY(empty.files.isEmpty());
    QVERIFY(empty.nodesTruncated);
}

void TestIncludeImpact::partialCoverage()
{
    Fixture m;
    const int observed = m.file("observed.cpp", true);
    const int outside = m.file("outside.cpp", true);
    const int generated = m.file("build/generated.h");
    const int external = m.file("/external/vendor.h");
    const int target = m.file("target.h");
    m.files[observed].includes = {{generated, 3}};
    m.files[generated].includes = {{target, 5}};
    // The indexer only records outgoing includes for project sources: the
    // vendor header is observed but its outgoing edges are absent.
    m.files[outside].includes = {{external, 2}};
    m.finalize();

    const Index index(m);
    QCOMPARE(index.coverage().indexedUnits, 2);
    QCOMPARE(index.coverage().externalFiles, 1);
    QCOMPARE(index.coverage().generatedFiles, 1);
    QVERIFY(index.coverage().partial());
    const QString scope = index.coverage().description();
    QVERIFY(scope.contains(QStringLiteral("configurations")));
    QVERIFY(scope.contains(QStringLiteral("external or generated")));
    QVERIFY(scope.contains(QStringLiteral("not a rebuild-time estimate")));
    const Reach reach = index.affected(target);
    QCOMPARE(reach.units, QVector<int>{observed});
    QCOMPARE(reach.witness(observed), (QVector<Step>{{observed, generated, 3}, {generated, target, 5}}));
    QVERIFY(reach.witness(outside).isEmpty());
    QCOMPARE(index.ranking().size(), 1);
}

void TestIncludeImpact::pythonOnlyUnavailable()
{
    Fixture m;
    const int a = m.file("a.py", true);
    const int b = m.file("b.py", true);
    m.files[a].includes = {{b, 1}};
    m.finalize();
    const Index index(m);
    QVERIFY(!index.coverage().available());
    QVERIFY(!index.coverage().partial());
    QVERIFY(index.graph(a, Direction::Includes).files.isEmpty());
    QVERIFY(index.ranking().isEmpty());
}

QTEST_APPLESS_MAIN(TestIncludeImpact)
#include "tst_includeimpact.moc"
