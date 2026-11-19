#include "project.h"
#include "model.h"
#include "python.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QVersionNumber>

#include <algorithm>

#include <clang-c/CXCompilationDatabase.h>

namespace Unravel::Project {

namespace {

const QStringList kSourceSuffixes = {"c", "cc", "cpp", "cxx", "c++", "cp", "m", "mm"};
const QStringList kHeaderSuffixes = {"h", "hh", "hpp", "hxx", "h++", "inl", "ipp", "tpp", "tcc", "inc", "def"};

bool skipDirectory(const QFileInfo &dir)
{
    const QString name = dir.fileName();
    if (name.startsWith(QLatin1Char('.')))
        return true;   // .git, .cache, .unravel, .vscode…
    static const QStringList names = {"node_modules", "_deps", "CMakeFiles", "__pycache__"};
    if (names.contains(name))
        return true;
    if (name.startsWith(QLatin1String("cmake-build-")) || name.endsWith(QLatin1String("_autogen")))
        return true;
    // A build tree, whatever it's called
    return QFileInfo::exists(dir.filePath() + QStringLiteral("/CMakeCache.txt"))
        || QFileInfo::exists(dir.filePath() + QStringLiteral("/build.ninja"))
        || QFileInfo::exists(dir.filePath() + QStringLiteral("/meson-info"));
}

void collect(const QString &dir, bool headersToo, QStringList &out, int depth)
{
    if (depth > 24)
        return;
    QDir d(dir);
    const QFileInfoList entries = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                                  QDir::Name);
    for (const QFileInfo &fi : entries) {
        if (fi.isDir()) {
            if (!skipDirectory(fi))
                collect(fi.filePath(), headersToo, out, depth + 1);
        } else if (isSource(fi.fileName()) || (headersToo && isHeader(fi.fileName()))) {
            out.append(QDir::cleanPath(fi.absoluteFilePath()));
        }
    }
}

QString absoluteIn(const QString &path, const QString &directory)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path))
        return QDir::cleanPath(path);
    return QDir::cleanPath(directory + QLatin1Char('/') + path);
}

QString toQString(CXString s)
{
    const QString out = QString::fromUtf8(clang_getCString(s));
    clang_disposeString(s);
    return out;
}

// Whether `dir` looks like the top of a source tree
bool isProjectTop(const QString &dir)
{
    return QFileInfo::exists(dir + QStringLiteral("/.git")) || QFileInfo::exists(dir + QStringLiteral("/.hg"))
        || QFileInfo::exists(dir + QStringLiteral("/.svn"));
}

} // namespace

bool isSource(const QString &path)
{
    return kSourceSuffixes.contains(QFileInfo(path).suffix().toLower());
}

bool isHeader(const QString &path)
{
    return kHeaderSuffixes.contains(QFileInfo(path).suffix().toLower());
}

QStringList sourceFiles(const QString &root, bool headersToo)
{
    QStringList out;
    collect(root, headersToo, out, 0);
    return out;
}

bool sourcesChanged(const CodeModel &model)
{
    for (const File &file : model.files) {
        if (!file.project)
            continue;
        const QFileInfo info(file.path);
        if (!info.exists() || info.lastModified().toMSecsSinceEpoch() > model.indexedAt)
            return true;
    }
    QStringList paths = sourceFiles(model.root, true);
    if (!Python::interpreter().isEmpty())
        paths += Python::sourceFiles(model.root);
    for (const QString &path : paths) {
        if (model.fileId(path) < 0)
            return true;
    }
    return false;
}

QString commonRoot(const QStringList &files)
{
    if (files.isEmpty())
        return QString();
    QStringList common = QFileInfo(files.first()).absolutePath().split(QLatin1Char('/'));
    for (const QString &f : files) {
        const QStringList parts = QFileInfo(f).absolutePath().split(QLatin1Char('/'));
        qsizetype n = 0;
        while (n < common.size() && n < parts.size() && common[n] == parts[n])
            ++n;
        common = common.mid(0, n);
    }
    const QString joined = common.join(QLatin1Char('/'));
    return joined.isEmpty() ? QStringLiteral("/") : joined;
}

CMakeChoice cmakeFor(const QString &root)
{
    CMakeChoice choice;
    // A build of this project, configured before: its CMakeCache.txt names
    // the CMake that did it (build*/ or a kit's folder below it, as Qt
    // Creator makes them), the most recent first
    QFileInfoList caches;
    const QDir top(root);
    for (const QString &b : top.entryList({QStringLiteral("build*"), QStringLiteral("cmake-build-*"), QStringLiteral("out")},
                                          QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString dir = top.filePath(b);
        caches << QFileInfo(dir + QStringLiteral("/CMakeCache.txt"));
        for (const QString &kit : QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            caches << QFileInfo(dir + QLatin1Char('/') + kit + QStringLiteral("/CMakeCache.txt"));
    }
    std::sort(caches.begin(), caches.end(), [](const QFileInfo &a, const QFileInfo &b) {
        return a.lastModified() > b.lastModified();
    });
    const QString home = QDir::cleanPath(root);
    // What made the project configure there, and is not about that build
    static const QStringList kept = {QStringLiteral("CMAKE_C_COMPILER"), QStringLiteral("CMAKE_CXX_COMPILER"),
                                     QStringLiteral("CMAKE_PREFIX_PATH"), QStringLiteral("CMAKE_TOOLCHAIN_FILE")};
    for (const QFileInfo &cache : std::as_const(caches)) {
        QFile f(cache.filePath());
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QString command, source;
        QStringList settings;
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.startsWith(QLatin1String("CMAKE_COMMAND:INTERNAL="))) {
                command = line.mid(23);
            } else if (line.startsWith(QLatin1String("CMAKE_HOME_DIRECTORY:INTERNAL="))) {
                source = line.mid(30);
            } else {
                const qsizetype colon = line.indexOf(QLatin1Char(':')), equals = line.indexOf(QLatin1Char('='));
                if (colon > 0 && equals > colon && equals + 1 < line.size() && kept.contains(line.left(colon)))
                    settings << QStringLiteral("-D") + line.left(colon) + QLatin1Char('=') + line.mid(equals + 1);
            }
        }
        if (!command.isEmpty() && QDir::cleanPath(source) == home && QFileInfo(command).isExecutable()) {
            choice.executable = command;
            choice.arguments = settings;
            choice.basedOn = cache.absolutePath();
            return choice;
        }
    }

    // Else the newest of the ones around
    const QString qt = QLibraryInfo::path(QLibraryInfo::PrefixPath);
    QStringList candidates = {QStandardPaths::findExecutable(QStringLiteral("cmake"))};
    for (const QString &dir : {qt + QStringLiteral("/../../Tools/CMake/bin"),
                               qt + QStringLiteral("/../../Tools/CMake/CMake.app/Contents/bin"),
                               QStringLiteral("/snap/bin"), QStringLiteral("/opt/homebrew/bin"),
                               QStringLiteral("/usr/local/bin")}) {
        candidates << QStandardPaths::findExecutable(QStringLiteral("cmake"), {QDir::cleanPath(dir)});
    }
    QVersionNumber bestVersion;
    QSet<QString> seen;
    for (const QString &c : std::as_const(candidates)) {
        const QString real = QFileInfo(c).canonicalFilePath();
        if (c.isEmpty() || seen.contains(real.isEmpty() ? c : real))
            continue;
        seen.insert(real.isEmpty() ? c : real);
        QProcess p;
        p.start(c, {QStringLiteral("--version")});
        if (!p.waitForFinished(5000) || p.exitCode() != 0)
            continue;
        const QRegularExpressionMatch v = QRegularExpression(QStringLiteral("version (\\d+\\.\\d+(\\.\\d+)?)"))
                                              .match(QString::fromLocal8Bit(p.readAllStandardOutput()));
        const QVersionNumber version = v.hasMatch() ? QVersionNumber::fromString(v.captured(1)) : QVersionNumber();
        if (choice.executable.isEmpty() || version > bestVersion) {
            choice.executable = c;
            bestVersion = version;
        }
    }
    return choice;
}

QString findDatabase(const QString &dir)
{
    const QString direct = dir + QStringLiteral("/compile_commands.json");
    if (QFileInfo::exists(direct))
        return direct;
    // build/, build-debug/, out/…: the most recently written one
    QString best;
    QDateTime bestTime;
    const QFileInfoList subdirs = QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &sub : subdirs) {
        QStringList candidates = {sub.filePath() + QStringLiteral("/compile_commands.json")};
        // build/Debug/compile_commands.json and the like
        const QFileInfoList deeper = QDir(sub.filePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &d : deeper)
            candidates.append(d.filePath() + QStringLiteral("/compile_commands.json"));
        for (const QString &c : std::as_const(candidates)) {
            const QFileInfo fi(c);
            if (fi.exists() && (best.isEmpty() || fi.lastModified() > bestTime)) {
                best = fi.absoluteFilePath();
                bestTime = fi.lastModified();
            }
        }
    }
    return best;
}

QStringList cleanArguments(const QStringList &argv, const QString &file, const QString &directory)
{
    static const QSet<QString> dropAlone = {
        "-c", "-S", "-E", "-M", "-MM", "-MD", "-MMD", "-MP", "-MG", "-Winvalid-pch", "-fno-fat-lto-objects",
        "-ffat-lto-objects", "-fconcepts", "-fmodules-ts", "-fno-gnu-unique", "-fext-numeric-literals",
        "-fno-keep-inline-dllexport", "-fcompare-debug-second", "-fvar-tracking", "-fno-var-tracking",
        "-fvar-tracking-assignments", "-fno-var-tracking-assignments", "-flto=auto", "-pipe", "-fsanitize=address",
    };
    static const QSet<QString> dropWithValue = {"-o", "-MF", "-MT", "-MQ", "--param", "-Xlinker"};
    static const QSet<QString> pathWithValue = {"-I", "-isystem", "-iquote", "-idirafter", "-include",
                                                "-imacros", "-isysroot", "--sysroot", "-F"};
    const QString cleanFile = QDir::cleanPath(file);
    QStringList out;
    for (qsizetype i = 1; i < argv.size(); ++i) {   // argv[0] is the compiler
        const QString &a = argv[i];
        if (dropAlone.contains(a))
            continue;
        if (dropWithValue.contains(a)) {
            ++i;
            continue;
        }
        if (a.startsWith(QLatin1String("-o")) && a.size() > 2 && !a.startsWith(QLatin1String("-objc")))
            continue;
        if (a.startsWith(QLatin1String("-Werror")) || a.startsWith(QLatin1String("-fipa-"))
            || a.startsWith(QLatin1String("-fno-ipa-")) || a.startsWith(QLatin1String("-ftree-"))
            || a.startsWith(QLatin1String("-fno-tree-")) || a.startsWith(QLatin1String("-fdump-"))
            || a.startsWith(QLatin1String("-flto")) || a.startsWith(QLatin1String("-fprofile-"))
            || a.startsWith(QLatin1String("-Wl,")) || a.startsWith(QLatin1String("-fsanitize"))
            || a.startsWith(QLatin1String("-fdiagnostics-")) || a.startsWith(QLatin1String("-fmodule-mapper")))
            continue;
        if (pathWithValue.contains(a) && i + 1 < argv.size()) {
            out << a << absoluteIn(argv[i + 1], directory);
            ++i;
            continue;
        }
        if (a.startsWith(QLatin1String("-I")) && a.size() > 2) {
            out << QStringLiteral("-I") + absoluteIn(a.mid(2), directory);
            continue;
        }
        if (a.startsWith(QLatin1String("-isystem")) && a.size() > 8) {
            out << QStringLiteral("-isystem") << absoluteIn(a.mid(8), directory);
            continue;
        }
        if (!a.startsWith(QLatin1Char('-'))) {
            // The source file, or an input we can't use
            if (absoluteIn(a, directory) == cleanFile)
                continue;
            if (isSource(a))
                continue;
        }
        out << a;
    }
    out << QStringLiteral("-working-directory=") + directory;
    return out;
}

QStringList minimalArguments(const QStringList &args)
{
    QStringList out;
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString &a = args[i];
        if ((a == QLatin1String("-I") || a == QLatin1String("-isystem") || a == QLatin1String("-iquote")
             || a == QLatin1String("-idirafter") || a == QLatin1String("-D") || a == QLatin1String("-U")
             || a == QLatin1String("-x")) && i + 1 < args.size()) {
            out << a << args[i + 1];
            ++i;
            continue;
        }
        if (a.startsWith(QLatin1String("-I")) || a.startsWith(QLatin1String("-D")) || a.startsWith(QLatin1String("-U"))
            || a.startsWith(QLatin1String("-std=")) || a.startsWith(QLatin1String("-stdlib="))
            || a.startsWith(QLatin1String("--target")) || a.startsWith(QLatin1String("-working-directory"))
            || a == QLatin1String("-m32") || a == QLatin1String("-m64") || a == QLatin1String("-pthread")
            || a.startsWith(QLatin1String("-nostdinc")) || a.startsWith(QLatin1String("-fms-")))
            out << a;
    }
    return out;
}

QList<CompileUnit> readDatabase(const QString &databaseDir, QString *error)
{
    QList<CompileUnit> units;
    CXCompilationDatabase_Error err = CXCompilationDatabase_NoError;
    const QByteArray dir = QFile::encodeName(databaseDir);
    CXCompilationDatabase db = clang_CompilationDatabase_fromDirectory(dir.constData(), &err);
    if (err != CXCompilationDatabase_NoError || !db) {
        if (error)
            *error = QStringLiteral("Could not read %1/compile_commands.json").arg(databaseDir);
        return units;
    }
    CXCompileCommands commands = clang_CompilationDatabase_getAllCompileCommands(db);
    const unsigned n = clang_CompileCommands_getSize(commands);
    QSet<QString> seen;
    for (unsigned i = 0; i < n; ++i) {
        CXCompileCommand cmd = clang_CompileCommands_getCommand(commands, i);
        CompileUnit u;
        u.directory = QDir::cleanPath(toQString(clang_CompileCommand_getDirectory(cmd)));
        u.file = absoluteIn(toQString(clang_CompileCommand_getFilename(cmd)), u.directory);
        if (seen.contains(u.file) || !isSource(u.file))
            continue;
        seen.insert(u.file);
        QStringList argv;
        const unsigned na = clang_CompileCommand_getNumArgs(cmd);
        for (unsigned a = 0; a < na; ++a)
            argv << toQString(clang_CompileCommand_getArg(cmd, a));
        // Expand response files (@file), as ninja writes for long commands
        QStringList expanded;
        for (const QString &arg : std::as_const(argv)) {
            if (arg.startsWith(QLatin1Char('@'))) {
                QFile rsp(absoluteIn(arg.mid(1), u.directory));
                if (rsp.open(QIODevice::ReadOnly)) {
                    const QString text = QString::fromUtf8(rsp.readAll());
                    expanded << QProcess::splitCommand(text.simplified());
                    continue;
                }
            }
            expanded << arg;
        }
        u.args = cleanArguments(expanded, u.file, u.directory);
        units.append(u);
    }
    clang_CompileCommands_dispose(commands);
    clang_CompilationDatabase_dispose(db);
    if (units.isEmpty() && error)
        *error = QStringLiteral("The compilation database in %1 lists no C or C++ sources").arg(databaseDir);
    return units;
}

ProjectSetup discover(const QString &path, const QStringList &extraArgs, const QString &buildDir)
{
    ProjectSetup setup;
    const QFileInfo fi(path);
    QString chosenRoot;
    if (fi.isFile() && fi.fileName() == QLatin1String("compile_commands.json")) {
        setup.database = fi.absoluteFilePath();
    } else if (fi.isDir()) {
        chosenRoot = QDir::cleanPath(fi.absoluteFilePath());
        setup.database = buildDir.isEmpty() ? findDatabase(chosenRoot)
                                            : (QFileInfo::exists(buildDir + QStringLiteral("/compile_commands.json"))
                                                   ? buildDir + QStringLiteral("/compile_commands.json")
                                                   : QString());
    } else {
        setup.error = QStringLiteral("%1 is neither a folder nor a compile_commands.json").arg(path);
        return setup;
    }

    if (!setup.database.isEmpty()) {
        setup.source = ProjectSetup::CompilationDatabase;
        // Through symlinks (a root's compile_commands.json pointing into build/)
        const QString real = QFileInfo(setup.database).canonicalFilePath();
        if (!real.isEmpty())
            setup.database = real;
        setup.buildDir = QDir::cleanPath(QFileInfo(setup.database).absolutePath());
        QString error;
        setup.units = readDatabase(setup.buildDir, &error);
        if (setup.units.isEmpty()) {
            setup.notes << error;
        } else {
            setup.notes << QStringLiteral("Compilation database: %1 (%2 translation units)")
                               .arg(setup.database).arg(setup.units.size());
        }
        if (chosenRoot.isEmpty()) {
            QStringList sources;
            for (const CompileUnit &u : std::as_const(setup.units)) {
                if (!u.file.startsWith(setup.buildDir + QLatin1Char('/')))
                    sources << u.file;
            }
            // An in-source build (Bear, make): the database sits in the sources
            if (sources.isEmpty()) {
                for (const CompileUnit &u : std::as_const(setup.units))
                    sources << u.file;
            }
            QString root = sources.isEmpty() ? setup.buildDir : commonRoot(sources);
            // Up to the top of the repository, so headers in include/ count too
            QString up = root;
            for (int i = 0; i < 5 && !up.isEmpty() && up != QLatin1String("/"); ++i) {
                if (isProjectTop(up)) {
                    root = up;
                    break;
                }
                up = QFileInfo(up).absolutePath();
            }
            const QString buildParent = QFileInfo(setup.buildDir).absolutePath();
            // build/ beside src/: the folder holding both. Not for an in-source
            // build, where the database already sits in the sources
            const bool inSource = withSlash(root).startsWith(withSlash(setup.buildDir));
            if (!inSource && !isProjectTop(root) && withSlash(root).startsWith(withSlash(buildParent)))
                root = buildParent;
            chosenRoot = root.isEmpty() ? setup.buildDir : root;
        }
    }
    setup.root = chosenRoot;

    // Units outside the root are the build's own (moc, QML caches, resources):
    // nothing in them is the project's, so parsing them only costs time
    if (!setup.units.isEmpty() && !setup.root.isEmpty()) {
        const QString rootSlash = withSlash(setup.root);
        const qsizetype before = setup.units.size();
        setup.units.erase(std::remove_if(setup.units.begin(), setup.units.end(), [&](const CompileUnit &u) {
                              return !u.file.startsWith(rootSlash);
                          }), setup.units.end());
        if (setup.units.size() < before)
            setup.notes << QStringLiteral("Left out %1 generated units outside %2").arg(before - setup.units.size()).arg(setup.root);
    }

    if (setup.units.isEmpty()) {
        setup.source = ProjectSetup::Guessed;
        const QStringList all = sourceFiles(setup.root, true);
        QStringList includeDirs = {setup.root};
        QSet<QString> seenDirs = {setup.root};
        bool usesQt = false;
        QStringList sources;
        // <lib/sub/header.h> needs the folder above lib/: from the includes
        // that name a folder and the headers whose paths end that way
        static const QRegularExpression includeRe(QStringLiteral("^\\s*#\\s*include\\s*[<\"]([^>\"]+/[^>\"]+)[>\"]"),
                                                  QRegularExpression::MultilineOption);
        QHash<QString, QStringList> headersByName;
        QSet<QString> folderIncludes;
        for (const QString &f : all) {
            QFile file(f);
            if (file.open(QIODevice::ReadOnly)) {
                const QByteArray head = file.read(16384);
                usesQt = usesQt || head.contains("#include <Q") || head.contains("#include <qt") || head.contains("Q_OBJECT");
                auto it = includeRe.globalMatch(QString::fromUtf8(head));
                while (it.hasNext()) {
                    const QString inc = QDir::cleanPath(it.next().captured(1));
                    if (!inc.startsWith(QLatin1String("..")) && !QDir::isAbsolutePath(inc))
                        folderIncludes.insert(inc);
                }
            }
            if (isHeader(f)) {
                headersByName[QFileInfo(f).fileName()] << f;
                const QString d = QFileInfo(f).absolutePath();
                if (!seenDirs.contains(d)) {
                    seenDirs.insert(d);
                    includeDirs << d;
                }
                // An include/ folder above lets <lib/header.h> resolve
                const qsizetype inc = d.lastIndexOf(QStringLiteral("/include/"));
                if (inc >= 0) {
                    const QString top = d.left(inc + 8);
                    if (!seenDirs.contains(top)) {
                        seenDirs.insert(top);
                        includeDirs << top;
                    }
                }
            } else {
                sources << f;
            }
        }
        for (const QString &inc : std::as_const(folderIncludes)) {
            const QString suffix = QLatin1Char('/') + inc;
            for (const QString &h : headersByName.value(inc.mid(inc.lastIndexOf(QLatin1Char('/')) + 1))) {
                if (!h.endsWith(suffix))
                    continue;
                const QString top = h.left(h.size() - suffix.size());
                if (!seenDirs.contains(top)) {
                    seenDirs.insert(top);
                    includeDirs << top;
                }
            }
        }
        QStringList common;
        for (const QString &d : std::as_const(includeDirs))
            common << QStringLiteral("-I") + d;
        if (usesQt) {
            const QString qtHeaders = QLibraryInfo::path(QLibraryInfo::HeadersPath);
            if (QFileInfo(qtHeaders + QStringLiteral("/QtCore")).isDir()) {
                common << QStringLiteral("-isystem") << qtHeaders;
                const QStringList modules = QDir(qtHeaders).entryList({QStringLiteral("Qt*")}, QDir::Dirs);
                for (const QString &m : modules)
                    common << QStringLiteral("-isystem") << qtHeaders + QLatin1Char('/') + m;
                common << QStringLiteral("-fPIC");
                setup.notes << QStringLiteral("Uses Qt: added the headers in %1").arg(qtHeaders);
            }
        }
        for (const QString &f : std::as_const(sources)) {
            CompileUnit u;
            u.file = f;
            u.directory = QFileInfo(f).absolutePath();
            const bool c = QFileInfo(f).suffix() == QLatin1String("c");
            u.args << QStringLiteral("-x") << (c ? QStringLiteral("c") : QStringLiteral("c++"))
                   << (c ? QStringLiteral("-std=c17") : QStringLiteral("-std=c++20")) << common;
            setup.units.append(u);
        }
        setup.notes << QStringLiteral("No compilation database: compiling %1 sources with %2 include folders found "
                                      "next to the headers. A compile_commands.json gives exact results.")
                           .arg(sources.size()).arg(includeDirs.size());
        if (sources.isEmpty())
            setup.error = QStringLiteral("No C or C++ sources in %1").arg(setup.root);
    }

    // Python sources under the root, indexed beside the C and C++ ones
    if (!setup.root.isEmpty() && setup.root != QLatin1String("/")) {
        setup.pythonFiles = Python::sourceFiles(setup.root);
        if (!setup.pythonFiles.isEmpty()) {
            setup.notes << (Python::interpreter().isEmpty()
                                ? QStringLiteral("%1 Python files, but no python3 to read them with").arg(setup.pythonFiles.size())
                                : QStringLiteral("Python: %1 files, read with the standard library's ast").arg(setup.pythonFiles.size()));
            if (setup.units.isEmpty()) {
                // Python only: not an error, and no note about missing C sources
                setup.error.clear();
                setup.notes.erase(std::remove_if(setup.notes.begin(), setup.notes.end(), [](const QString &n) {
                                      return n.startsWith(QLatin1String("No compilation database"));
                                  }), setup.notes.end());
            }
        }
    }

    for (CompileUnit &u : setup.units) {
        u.args << QStringLiteral("-ferror-limit=0") << QStringLiteral("-Wno-unknown-warning-option")
               << QStringLiteral("-Wno-unused-command-line-argument")
               << QStringLiteral("-Wno-ignored-optimization-argument") << extraArgs;
    }
    return setup;
}

} // namespace Unravel::Project
