#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include "focus.h"
#include "history.h"
#include "indexer.h"
#include "project.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <thread>

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

namespace {
bool write(const QString &path, const QByteArray &data, QString &error) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() ||
      !file.commit()) {
    error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
    return false;
  }
  return true;
}

bool generate(const QString &path, int functions, QString &error) {
  QDir dir(path);
  if (dir.exists() &&
      !dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
           .isEmpty()) {
    error = QStringLiteral("Fixture generation requires an empty destination");
    return false;
  }
  if (!dir.mkpath(QStringLiteral(".")) ||
      !dir.mkpath(QStringLiteral("empty-hooks"))) {
    error = QStringLiteral("Cannot create fixture directories");
    return false;
  }
  auto git = [&](const QStringList &args, int commit = 0) {
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("GIT_CONFIG_NOSYSTEM", "1");
    environment.insert(
        "GIT_CONFIG_GLOBAL",
        QDir::toNativeSeparators(dir.filePath("no-global-config")));
    environment.insert("GIT_AUTHOR_NAME", "Synthetic Fixture");
    environment.insert("GIT_AUTHOR_EMAIL", "fixture@example.test");
    environment.insert("GIT_COMMITTER_NAME", "Synthetic Fixture");
    environment.insert("GIT_COMMITTER_EMAIL", "fixture@example.test");
    const auto date =
        QStringLiteral("@%1 +0000").arg(1704067200LL + commit * 86400LL);
    environment.insert("GIT_AUTHOR_DATE", date);
    environment.insert("GIT_COMMITTER_DATE", date);
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(path);
    process.start("git",
                  QStringList{"-c",
                              "core.hooksPath=" + dir.filePath("empty-hooks"),
                              "-c", "commit.gpgsign=false"} +
                      args);
    if (!process.waitForFinished(120000) || process.exitCode() != 0 ||
        process.exitStatus() != QProcess::NormalExit) {
      error = "Fixture Git failed: " +
              QString::fromUtf8(process.readAllStandardError());
      return false;
    }
    return true;
  };
  if (!write(dir.filePath(".gitignore"),
             "compile_commands.json\nreview.index\nhistory-cache/\n", error) ||
      !write(dir.filePath("common.h"),
             "#pragma once\ninline int common(int x) { return x * 3; }\n",
             error))
    return false;
  QJsonArray database;
  const int units = std::min(functions, 100);
  for (int file = 0, id = 0; file < units; ++file) {
    QByteArray text("#include \"common.h\"\n");
    const int end = (file + 1) * functions / units;
    const int start = id;
    for (; id < end; ++id) {
      text +=
          QStringLiteral("int f%1(int x) {\n  if (x < %2) return common(x);\n"
                         "  if (x & 1) x += %3;\n  return %4;\n}\n")
              .arg(id)
              .arg(id % 31)
              .arg(id % 7)
              .arg(id == start ? QStringLiteral("common(x)")
                               : QStringLiteral("f%1(x - 1)").arg(id - 1))
              .toUtf8();
    }
    const auto source = dir.filePath(QStringLiteral("unit%1.cpp").arg(file));
    if (!write(source, text, error))
      return false;
    database.append(QJsonObject{
        {"directory", path},
        {"file", source},
        {"arguments", QJsonArray{"clang++", "-std=c++20", "-c", source}}});
  }
  if (!write(dir.filePath("compile_commands.json"),
             QJsonDocument(database).toJson(), error) ||
      !git({"init", "-q", "--object-format=sha1"}) || !git({"add", "."}) ||
      !git({"commit", "-qm", "Synthetic baseline"}))
    return false;
  for (int commit = 1; commit <= 100; ++commit) {
    const auto source =
        dir.filePath(QStringLiteral("unit%1.cpp").arg(commit % units));
    QFile file(source);
    if (!file.open(QIODevice::Append)) {
      error = file.errorString();
      return false;
    }
    const auto text =
        QStringLiteral("// synthetic change %1\n").arg(commit).toUtf8();
    if (file.write(text) != text.size()) {
      error = file.errorString();
      return false;
    }
    file.close();
    if (!git({"add", source}) ||
        !git({"commit", "-qm",
              QStringLiteral("Synthetic change %1").arg(commit)},
             commit))
      return false;
  }
  return true;
}

double percentile(QVector<double> values, double fraction) {
  std::sort(values.begin(), values.end());
  return values[std::max(0, int(std::ceil(values.size() * fraction)) - 1)];
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addPositionalArgument("project", "Project root or compile database");
  parser.addOptions(
      {{"generate",
        "Generate a synthetic C++/Git fixture in an empty project path"},
       {"synthetic",
        "Label a rerun of an existing generated fixture as synthetic"},
       {"functions", "Synthetic function count", "n", "100000"},
       {"runs", "Cached ranking repetitions (at least 20)", "n", "30"},
       {"load-index",
        "Explicitly reuse an index made for these unchanged sources", "path"},
       {"save-index", "Save the benchmark index for inspection", "path"},
       {"output", "Write benchmark JSON", "path"}});
  parser.process(app);
  const auto paths = parser.positionalArguments();
  bool functionsOk = false, runsOk = false;
  const auto functions = parser.value("functions").toInt(&functionsOk);
  const auto runs = parser.value("runs").toInt(&runsOk);
  if (paths.size() != 1 || !functionsOk || functions < 1 ||
      functions > 1000000 || !runsOk || runs < 20 || runs > 1000)
    parser.showHelp(2);
  const auto project = QFileInfo(paths[0]).absoluteFilePath();
  QString error;
  auto fail = [&](const QString &message) {
    QTextStream(stderr) << message << '\n';
    return 1;
  };
  if (parser.isSet("generate") && !generate(project, functions, error))
    return fail(error);
  const auto setup = Unravel::Project::discover(project);
  if (!setup.error.isEmpty())
    return fail(setup.error);
  Unravel::CodeModel model;
  QElapsedTimer timer;
  timer.start();
  const bool indexCached = parser.isSet("load-index");
  if (indexCached && !model.load(parser.value("load-index")))
    return fail("Cannot load the requested benchmark index");
  if (indexCached && model.root != setup.root)
    return fail("The benchmark index belongs to a different project");
  if (!indexCached) {
    const auto indexed = Unravel::Indexer::run(setup, model, nullptr, {}, 2);
    if (!indexed.ok)
      return fail(indexed.error);
  }
  if (parser.isSet("save-index") && !model.save(parser.value("save-index")))
    return fail("Cannot save benchmark index");
  const auto indexMs = timer.elapsed();
  if (model.stats().errors)
    return fail(QStringLiteral("Benchmark has %1 parse errors")
                    .arg(model.stats().errors));
  QTemporaryDir storage;
  if (!storage.isValid())
    return fail("Cannot create the benchmark history-cache directory");
  const auto cache = storage.path();
  const auto until =
      2145916799LL; // inclusive end of 2037, fixed for reproducibility
  timer.restart();
  const auto cold = Unravel::History::fileActivity(setup.root, "HEAD", 0, until,
                                                   nullptr, cache, true);
  const auto coldMs = timer.elapsed();
  if (!cold.ready())
    return fail(cold.error.isEmpty()
                    ? QStringLiteral("Benchmark requires a Git repository")
                    : cold.error);
  timer.restart();
  const auto warm = Unravel::History::fileActivity(setup.root, "HEAD", 0, until,
                                                   nullptr, cache);
  const auto warmMs = timer.elapsed();
  if (!warm.ready() || !warm.cached || cold.files.size() != warm.files.size())
    return fail("Warm history cache did not match the cold result");
  for (auto it = cold.files.cbegin(); it != cold.files.cend(); ++it) {
    const auto cached = warm.files.value(it.key());
    const auto &fresh = it.value();
    if (cached.commits != fresh.commits || cached.added != fresh.added ||
        cached.removed != fresh.removed ||
        cached.lastChanged != fresh.lastChanged ||
        cached.authors != fresh.authors || cached.binary != fresh.binary)
      return fail("Warm history cache disagrees with cold metrics");
  }
  QVector<double> projectTimes, touchedTimes;
  Unravel::Focus::Options options;
  for (int i = 0; i < model.symbols.size(); ++i)
    if (Unravel::isFunction(model.symbols[i].kind) && i % 100 == 0)
      options.touched.insert(i);
  for (int i = 0; i < runs; ++i) {
    options.changedOnly = false;
    timer.restart();
    const auto result = Unravel::Focus::rank(model, &warm, options);
    projectTimes.append(timer.nsecsElapsed() / 1000000.0);
    if (!result.error.isEmpty() ||
        result.entries.size() != std::min(20, result.eligible))
      return fail("Invalid project ranking: " + result.error);
    options.changedOnly = true;
    timer.restart();
    const auto filtered = Unravel::Focus::rank(model, &warm, options);
    touchedTimes.append(timer.nsecsElapsed() / 1000000.0);
    if (!filtered.error.isEmpty())
      return fail(filtered.error);
  }
  std::atomic_bool cancelled{false}, started{false};
  auto pending = std::async(std::launch::async, [&] {
    started.store(true);
    return Unravel::History::fileActivity(setup.root, "HEAD", 0, until,
                                          &cancelled, {}, true, true);
  });
  while (!started.load())
    std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  timer.restart();
  cancelled.store(true);
  const auto cancellation = pending.get();
  const auto cancelMs = timer.elapsed();
  if (!cancellation.cancelled)
    return fail("History finished before cancellation; fixture insufficient to "
                "measure cancellation");
  qint64 cacheBytes = 0;
  QDirIterator entries(cache, QDir::Files, QDirIterator::Subdirectories);
  while (entries.hasNext()) {
    entries.next();
    cacheBytes += entries.fileInfo().size();
  }
  qint64 peakRss = -1;
#ifdef Q_OS_UNIX
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
#ifdef Q_OS_MACOS
    peakRss = usage.ru_maxrss;
#else
    peakRss = usage.ru_maxrss * 1024LL;
#endif
  }
#endif
  const double projectP95 = percentile(projectTimes, .95);
  const double touchedP95 = percentile(touchedTimes, .95);
  QString compiler;
#ifdef _MSC_VER
  compiler = QStringLiteral("MSVC %1").arg(_MSC_VER);
#else
  compiler = QStringLiteral(__VERSION__);
#endif
  const QJsonObject report{
      {"schemaVersion", 1},
      {"fixture", parser.isSet("generate") || parser.isSet("synthetic")
                      ? "synthetic-generated-cpp-git"
                      : "local-project"},
      {"root", setup.root},
      {"revision", cold.revision},
      {"since", 0},
      {"until", until},
      {"host", QSysInfo::prettyProductName()},
      {"architecture", QSysInfo::currentCpuArchitecture()},
      {"logicalCpus", QThread::idealThreadCount()},
      {"indexThreads", 2},
      {"compiler", compiler},
      {"qt", qVersion()},
      {"indexCached", indexCached},
      {"indexOrLoadMs", indexMs},
      {"files", model.files.size()},
      {"functions", model.stats().functions},
      {"symbols", model.symbols.size()},
      {"edges", model.edges.size()},
      {"runs", runs},
      {"projectTop20P95Ms", projectP95},
      {"touchedTop20P95Ms", touchedP95},
      {"coldHistoryMs", coldMs},
      {"warmHistoryMs", warmMs},
      {"cacheBytes", cacheBytes},
      {"peakRssBytes", peakRss},
      {"cancelMs", cancelMs},
      {"cancellationPolicy",
       "active Git rename-follow query, cancellation requested after 20ms"},
      {"coverage",
       cold.shallow ? "partial-shallow" : "non-merge-pathname-file-proxy"},
      {"rankingTargetPass", projectP95 <= 200 && touchedP95 <= 200},
      {"cancellationTargetPass", cancelMs < 1000},
      {"panOverlay",
       "Measure separately with UNRAVEL_PAN_BENCH; not a core-loop proxy"}};
  const auto json = QJsonDocument(report).toJson();
  if (parser.isSet("output") && !write(parser.value("output"), json, error))
    return fail(error);
  QTextStream(stdout) << json;
  return projectP95 <= 200 && touchedP95 <= 200 && cancelMs < 1000 ? 0 : 3;
}
