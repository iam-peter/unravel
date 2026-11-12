// unravel-cli: index a C/C++ project with clang and print what was found,
// or write the review report. For scripts, CI and the tests.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QTextStream>

#include "analysis.h"
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
    parser.addOptions({reportOpt, saveOpt, loadOpt, threadsOpt, dumpOpt, argOpt});
    parser.process(app);

    QTextStream err(stderr);
    QTextStream out(stdout);
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
    return 0;
}
