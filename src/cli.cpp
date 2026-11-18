// unravel-cli: index a C/C++ project with clang and print what was found,
// or write the review report. For scripts, CI and the tests.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QTextStream>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDir>

#include <algorithm>

#include "analysis.h"
#include "architecture.h"
#include "focus.h"
#include "includeimpact.h"
#include "indexer.h"
#include "project.h"
#include "report.h"

using namespace Unravel;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("unravel-cli"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Index C/C++ code with clang and report on its architecture"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("path"), QStringLiteral("Project folder or compile_commands.json"));
    const QCommandLineOption reportOpt(QStringLiteral("report"), QStringLiteral("Write the Markdown review report to <file> (- for stdout)."), QStringLiteral("file"));
    const QCommandLineOption saveOpt(QStringLiteral("save"), QStringLiteral("Save the index to <file>."), QStringLiteral("file"));
    const QCommandLineOption loadOpt(QStringLiteral("load"), QStringLiteral("Load an index from <file> instead of indexing."), QStringLiteral("file"));
    const QCommandLineOption threadsOpt(QStringLiteral("threads"), QStringLiteral("Indexing threads."), QStringLiteral("n"));
    const QCommandLineOption dumpOpt(QStringLiteral("dump"), QStringLiteral("Print every project symbol and edge."));
    const QCommandLineOption argOpt(QStringLiteral("arg"), QStringLiteral("Extra compiler argument (repeatable)."), QStringLiteral("arg"));
    const QCommandLineOption focusOpt(QStringLiteral("focus"), QStringLiteral("Write Review Focus Markdown (- for stdout)."), QStringLiteral("file"));
    const QCommandLineOption focusJsonOpt(QStringLiteral("focus-json"), QStringLiteral("Write Review Focus JSON (- for stdout)."), QStringLiteral("file"));
    const QCommandLineOption daysOpt(QStringLiteral("history-days"), QStringLiteral("Review Focus history window in UTC calendar days."), QStringLiteral("days"), QStringLiteral("180"));
    const QCommandLineOption revisionOpt(QStringLiteral("revision"), QStringLiteral("Git revision for historical review priority."), QStringLiteral("revision"), QStringLiteral("HEAD"));
    const QCommandLineOption staticOpt(QStringLiteral("static-focus"), QStringLiteral("Use static review priority without Git history."));
    const QCommandLineOption followOpt(QStringLiteral("follow-renames"), QStringLiteral("Use Git --follow's file-lineage heuristic (slower)."));
    const QCommandLineOption contractsOpt(QStringLiteral("architecture"), QStringLiteral("Architecture contract JSON configuration."), QStringLiteral("file"));
    const QCommandLineOption contractsJsonOpt(QStringLiteral("architecture-json"), QStringLiteral("Write architecture contract results as JSON (- for stdout)."), QStringLiteral("file"));
    const QCommandLineOption failContractsOpt(QStringLiteral("fail-on-architecture"), QStringLiteral("Exit 3 for architecture violations; configuration errors exit 2."));
    const QCommandLineOption impactOpt(QStringLiteral("include-impact"), QStringLiteral("Write top 20 header-impact estimates and witnesses as JSON (- for stdout)."), QStringLiteral("file"));
    parser.addOptions({reportOpt, saveOpt, loadOpt, threadsOpt, dumpOpt, argOpt, focusOpt, focusJsonOpt, daysOpt, revisionOpt,
                       staticOpt, followOpt, contractsOpt, contractsJsonOpt, failContractsOpt, impactOpt});
    parser.process(app);

    QTextStream err(stderr);
    int stdoutReports = 0;
    for (const auto &option : {focusOpt, focusJsonOpt, contractsJsonOpt, impactOpt}) {
        if (parser.isSet(option) && parser.value(option) == QLatin1String("-"))
            ++stdoutReports;
    }
    if (stdoutReports > 1 || (stdoutReports && parser.isSet(dumpOpt))) {
        err << "Select only one stdout report, without --dump\n";
        return 2;
    }
    QTextStream standardOutput(stdout);
    QTextStream out(stdoutReports ? stderr : stdout);
    auto write = [&](const QCommandLineOption &option, const QByteArray &data) {
        if (!parser.isSet(option))
            return true;
        const QString path = parser.value(option);
        if (path == QLatin1String("-")) {
            standardOutput << QString::fromUtf8(data);
            return true;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
            err << "Cannot write " << path << ": " << file.errorString() << "\n";
            return false;
        }
        return true;
    };
    CodeModel model;
    if (parser.isSet(loadOpt)) {
        QElapsedTimer lt;
        lt.start();
        const bool loaded = model.load(parser.value(loadOpt));
        err << "Loaded in " << lt.elapsed() << " ms\n";
        if (!loaded) {
            err << "Could not load " << parser.value(loadOpt) << "\n";
            return 1;
        }
    } else {
        if (parser.positionalArguments().isEmpty())
            parser.showHelp(1);
        const ProjectSetup setup = Project::discover(parser.positionalArguments().first(), parser.values(argOpt));
        for (const QString &n : setup.notes)
            err << n << "\n";
        if (!setup.error.isEmpty()) {
            err << setup.error << "\n";
            return 1;
        }
        err << "Root: " << setup.root << "\n";
        err.flush();
        const IndexResult r = Indexer::run(setup, model, nullptr, [&](int done, int total, const QString &file) {
            static QMutex m;
            QMutexLocker l(&m);
            QTextStream e(stderr);
            e << "[" << done << "/" << total << "] " << file << "\n";
        }, parser.value(threadsOpt).toInt());
        if (!r.ok) {
            err << "Indexing failed: " << r.error << "\n";
            return 1;
        }
        if (parser.isSet(saveOpt) && !model.save(parser.value(saveOpt)))
            err << "Could not save the index\n";
    }

    const Stats st = model.stats();
    err << "Files " << st.projectFiles << "/" << st.files << ", units " << st.units << ", symbols " << st.symbols
        << " (types " << st.types << ", functions " << st.functions << ", variables " << st.variables << "), references "
        << st.references << ", edges " << model.edges.size() << ", errors " << st.errors << ", warnings " << st.warnings
        << ", " << st.indexMs << " ms\n";

    if (parser.isSet(focusOpt) || parser.isSet(focusJsonOpt)) {
        bool ok = false;
        const int days = parser.value(daysOpt).toInt(&ok);
        if (!ok || days < 1 || days > 3650) {
            err << "history-days must be between 1 and 3650\n";
            return 1;
        }
        History::Activity activity;
        if (!parser.isSet(staticOpt)) {
            const qint64 until = QDateTime::currentSecsSinceEpoch() / 86400 * 86400 + 86399;
            activity = History::fileActivity(model.root, parser.value(revisionOpt), until - qint64(days) * 86400 + 1, until,
                                              nullptr, {}, false, parser.isSet(followOpt));
            if (!activity.ready()) {
                err << (activity.error.isEmpty() ? QStringLiteral("No Git history; select --static-focus explicitly") : activity.error) << "\n";
                return 1;
            }
        }
        const auto result = Focus::rank(model, parser.isSet(staticOpt) ? nullptr : &activity);
        if (!result.error.isEmpty()) {
            err << result.error << "\n";
            return 1;
        }
        if (!write(focusOpt, Focus::markdown(model, result).toUtf8())
            || !write(focusJsonOpt, QJsonDocument(Focus::json(model, result)).toJson()))
            return 1;
    }

    bool contractViolation = false;
    if (parser.isSet(contractsOpt) || parser.isSet(contractsJsonOpt) || parser.isSet(failContractsOpt)) {
        const QString config = parser.isSet(contractsOpt) ? parser.value(contractsOpt)
            : QDir(model.root).filePath(QStringLiteral(".unravel/architecture.json"));
        const auto result = Architecture::check(model, config);
        if (!result.valid) {
            err << result.errors.join(QLatin1Char('\n')) << "\n";
            if (!write(contractsJsonOpt, result.toJson()))
                return 1;
            return 2;
        }
        err << result.violations.size() << " architecture violations\n";
        contractViolation = !result.violations.isEmpty() && parser.isSet(failContractsOpt);
        if (!write(contractsJsonOpt, result.toJson()))
            return 1;
    }

    if (parser.isSet(impactOpt)) {
        const IncludeImpact::Index index(model);
        const auto ranking = index.ranking();
        QJsonArray headers;
        for (int i = 0; i < std::min(20, int(ranking.size())); ++i) {
            const auto &header = ranking[i];
            const auto reach = index.affected(header.file);
            QJsonArray witnesses;
            for (int u = 0; u < std::min(50, int(reach.units.size())); ++u) {
                const int unit = reach.units[u];
                QJsonArray chain;
                for (const auto &step : reach.witness(unit))
                    chain.append(QJsonObject{{QStringLiteral("from"), model.files[step.from].relative},
                        {QStringLiteral("to"), model.files[step.to].relative}, {QStringLiteral("line"), step.line}});
                witnesses.append(QJsonObject{{QStringLiteral("unit"), model.files[unit].relative},
                    {QStringLiteral("chain"), chain}});
            }
            headers.append(QJsonObject{{QStringLiteral("header"), model.files[header.file].relative},
                {QStringLiteral("estimatedUnits"), header.affectedUnits}, {QStringLiteral("inCycle"), header.inCycle},
                {QStringLiteral("witnessesTruncated"), reach.units.size() > 50}, {QStringLiteral("witnesses"), witnesses}});
        }
        const QJsonObject report{{QStringLiteral("version"), 1}, {QStringLiteral("coverage"), index.coverage().description()},
            {QStringLiteral("indexedUnits"), index.coverage().indexedUnits},
            {QStringLiteral("partial"), index.coverage().partial()}, {QStringLiteral("headersTotal"), ranking.size()},
            {QStringLiteral("headersTruncated"), ranking.size() > 20}, {QStringLiteral("headers"), headers}};
        if (!write(impactOpt, QJsonDocument(report).toJson()))
            return 1;
    }

    if (parser.isSet(dumpOpt)) {
        for (qint32 i = 0; i < model.symbols.size(); ++i) {
            const Symbol &s = model.symbols[i];
            if (s.has(External))
                continue;
            out << kindName(s.kind) << " " << model.displayName(i) << " [" << s.signature << "] "
                << model.locationText(s.where()) << " cc=" << s.metrics.complexity << " lines=" << s.metrics.lines
                << " nest=" << s.metrics.nesting << " flags=" << Qt::hex << s.flags << Qt::dec << "\n";
            for (qint32 e : model.outEdges[i]) {
                const Edge &edge = model.edges[e];
                out << "    " << refKindName(edge.kind) << " -> " << model.displayName(edge.to) << " x" << edge.count << "\n";
            }
        }
        for (const Diagnostic &d : model.diagnostics)
            out << "diag " << d.severity << " " << model.locationText(d.loc) << ": " << d.message << "\n";
    }

    if (parser.isSet(reportOpt)) {
        QElapsedTimer t;
        t.start();
        const Analysis::Report rep = Analysis::analyze(model);
        const qint64 ta = t.elapsed();
        const QString md = Reporting::markdown(model, rep);
        err << "Analysis " << ta << " ms, report " << t.elapsed() - ta << " ms, " << rep.findings.size() << " findings\n";
        const QString path = parser.value(reportOpt);
        if (path == QLatin1String("-")) {
            out << md;
        } else {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly)) {
                err << "Could not write " << path << "\n";
                return 1;
            }
            f.write(md.toUtf8());
        }
    }
    return contractViolation ? 3 : 0;
}
