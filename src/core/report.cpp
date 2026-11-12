#include "report.h"

#include <QDateTime>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>

namespace Unravel::Reporting {

namespace {

QString esc(QString s)
{
    s.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    s.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    return s;
}

QString where(const CodeModel &m, const Location &loc)
{
    if (!loc.valid())
        return QString();
    return QStringLiteral("`%1`").arg(m.locationText(loc));
}

QString severityMark(int s)
{
    return s >= 2 ? QStringLiteral("🔴") : s == 1 ? QStringLiteral("🟠") : QStringLiteral("⚪");
}

} // namespace

QString markdown(const CodeModel &m, const Analysis::Report &report, const Options &o)
{
    const Stats st = m.stats();
    QString out;
    QTextStream t(&out);
    t << "# Code review: " << QFileInfo(m.root).fileName() << "\n\n";
    t << "Indexed by unravel (clang for C and C++, Python's ast for Python) on "
      << QDateTime::fromMSecsSinceEpoch(m.indexedAt).toString(QStringLiteral("yyyy-MM-dd hh:mm")) << ". Root: `"
      << m.root << "`.\n\n";

    t << "## In numbers\n\n";
    t << "| | |\n|---|---|\n";
    t << "| Files | " << st.projectFiles << " (" << st.units << " translation units) |\n";
    t << "| Lines | " << st.lines << " (" << st.codeLines << " of code) |\n";
    t << "| Types | " << st.types << " |\n";
    t << "| Functions | " << st.functions << " |\n";
    t << "| Variables and fields | " << st.variables << " |\n";
    t << "| References | " << st.references << " |\n";
    t << "| Compiler errors / warnings | " << st.errors << " / " << st.warnings << " |\n";
    t << "| Findings | " << report.serious << " serious, " << report.warnings << " warnings, " << report.notes
      << " notes |\n\n";

    if (st.errors > 0) {
        t << "> **Note:** clang reported " << st.errors
          << " errors, so parts of the code may be missing from this report. Index with the project's "
             "compile_commands.json for complete results.\n\n";
    }

    // Complexity distribution
    {
        QVector<qint32> cc;
        for (const Symbol &s : m.symbols) {
            if (isFunction(s.kind) && s.def.valid() && !s.has(External) && !s.has(Generated) && s.metrics.statements > 0)
                cc.append(s.metrics.complexity);
        }
        if (!cc.isEmpty()) {
            std::sort(cc.begin(), cc.end());
            double sum = 0;
            for (qint32 c : cc)
                sum += c;
            t << "Function complexity: median " << cc[cc.size() / 2] << ", 90th percentile "
              << cc[std::min(qsizetype(cc.size() - 1), qsizetype(cc.size() * 9 / 10))] << ", max " << cc.last()
              << ", mean " << QString::number(sum / cc.size(), 'f', 1) << ".\n\n";
        }
    }

    if (o.withModules) {
        const QVector<Analysis::ModuleMetrics> mods = Analysis::moduleMetrics(m);
        if (mods.size() > 1) {
            t << "## Modules\n\n";
            t << "Folders as modules. Ca: modules depending on it; Ce: modules it depends on; I: instability "
                 "Ce/(Ca+Ce); A: abstractness; D: distance from the main sequence |A+I−1|.\n\n";
            t << "| Module | Files | Lines | Classes | Functions | Ca | Ce | I | A | D |\n";
            t << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
            for (const Analysis::ModuleMetrics &mm : mods) {
                if (mm.name == QLatin1String("<external>"))
                    continue;
                t << "| " << esc(mm.name) << " | " << mm.files << " | " << mm.lines << " | " << mm.classes << " | "
                  << mm.functions << " | " << mm.afferent << " | " << mm.efferent << " | "
                  << QString::number(mm.instability, 'f', 2) << " | " << QString::number(mm.abstractness, 'f', 2)
                  << " | " << QString::number(mm.distance, 'f', 2) << " |\n";
            }
            t << "\n";
        }
    }

    if (o.withHotspots) {
        QVector<qint32> fns;
        for (qint32 i = 0; i < m.symbols.size(); ++i) {
            const Symbol &s = m.symbols[i];
            if (isFunction(s.kind) && s.def.valid() && !s.has(External) && !s.has(Generated))
                fns.append(i);
        }
        std::sort(fns.begin(), fns.end(), [&](qint32 a, qint32 b) {
            const auto &x = m.symbols[a], &y = m.symbols[b];
            return x.metrics.complexity * std::max(1, x.fanIn) > y.metrics.complexity * std::max(1, y.fanIn);
        });
        if (!fns.isEmpty()) {
            t << "## Hotspots\n\nComplex functions many others depend on: where a mistake costs most.\n\n";
            t << "| Function | Complexity | Lines | Callers | Callees | Where |\n|---|---:|---:|---:|---:|---|\n";
            for (qint32 i = 0; i < std::min<qsizetype>(15, fns.size()); ++i) {
                const Symbol &s = m.symbols[fns[i]];
                t << "| `" << esc(m.displayName(fns[i])) << "` | " << s.metrics.complexity << " | " << s.metrics.lines
                  << " | " << s.fanIn << " | " << s.fanOut << " | " << where(m, s.def) << " |\n";
            }
            t << "\n";
        }
    }

    t << "## Findings\n\n";
    if (report.findings.isEmpty())
        t << "Nothing found.\n\n";
    for (int k = 0; k < int(Analysis::FindingKind::Count); ++k) {
        const auto kind = Analysis::FindingKind(k);
        QVector<const Analysis::Finding *> list;
        for (const Analysis::Finding &f : report.findings) {
            if (f.kind == kind)
                list.append(&f);
        }
        if (list.isEmpty())
            continue;
        t << "### " << Analysis::findingKindName(kind) << " (" << list.size() << ")\n\n";
        t << Analysis::findingKindHint(kind) << "\n\n";
        for (qsizetype i = 0; i < std::min<qsizetype>(o.maxPerKind, list.size()); ++i) {
            const Analysis::Finding &f = *list[i];
            t << "- " << severityMark(f.severity) << " **" << esc(f.title) << "** — " << esc(f.detail);
            if (f.loc.valid())
                t << " " << where(m, f.loc);
            t << "\n";
            if (f.kind == Analysis::FindingKind::Duplicate) {
                for (qint32 s : f.symbols)
                    t << "  - `" << esc(m.displayName(s)) << "` " << where(m, m.symbols[s].def) << "\n";
            }
        }
        if (list.size() > o.maxPerKind)
            t << "- … and " << (list.size() - o.maxPerKind) << " more\n";
        t << "\n";
    }
    return out;
}

} // namespace Unravel::Reporting
