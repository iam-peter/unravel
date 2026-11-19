#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// What to index and how: the translation units of a project with the
// arguments its build compiles them with. They come from a compilation
// database (compile_commands.json, as CMake, Meson, Bear and others
// write it); without one, every source file under the root is compiled
// with include paths guessed from where the headers are.
namespace Unravel {

class CodeModel;

struct CompileUnit {
    QString file;         // absolute
    QString directory;    // the working directory of the compiler
    QStringList args;     // without the compiler, the source file and output options
};

struct ProjectSetup {
    enum Source { CompilationDatabase, Guessed };
    QString root;          // what counts as the project's own code
    QString buildDir;      // the compilation database's folder
    QString database;      // path of compile_commands.json, if any
    Source source = Guessed;
    QList<CompileUnit> units;
    QStringList pythonFiles;   // read by the Python indexer
    QStringList notes;     // what the discovery did, for the user
    QString error;
};

namespace Project {

// compile_commands.json in `dir`, `dir/build*` or one level below
QString findDatabase(const QString &dir);

// How to let CMake configure `root`: like a build of it that was already
// configured (its CMake, which was new enough, and its compilers and
// prefix path, which suited the project), else with the newest CMake on
// PATH, from Qt's installer, Snap or Homebrew. No executable without any.
struct CMakeChoice {
    QString executable;
    QStringList arguments;   // -D settings taken from the existing build
    QString basedOn;         // that build's folder, if any
};
CMakeChoice cmakeFor(const QString &root);

// From a folder (the project root) or a compile_commands.json. `extraArgs`
// go to every unit. `buildDir`, when set, is where to look for the database.
ProjectSetup discover(const QString &path, const QStringList &extraArgs = {}, const QString &buildDir = {});

// The units of a compilation database, with cleaned arguments
QList<CompileUnit> readDatabase(const QString &databaseDir, QString *error);

// Every C and C++ source under `root`, skipping build trees and VCS folders
QStringList sourceFiles(const QString &root, bool headersToo = false);

// Changed, deleted or newly discovered project sources since this index.
bool sourcesChanged(const CodeModel &model);

// The common folder of files
QString commonRoot(const QStringList &files);

// Arguments clang should drop or that a parse can't use: outputs, dependency
// files, gcc-only switches
QStringList cleanArguments(const QStringList &argv, const QString &file, const QString &directory);

// Only include paths, defines and the language standard
QStringList minimalArguments(const QStringList &args);

bool isSource(const QString &path);
bool isHeader(const QString &path);

} // namespace Project
} // namespace Unravel
