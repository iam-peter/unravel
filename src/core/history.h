#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

#include "model.h"

// What commits changed, in terms of the indexed code: git's history read
// with the git command line, the lines each selected commit touched
// carried forward to the files as they are now (which is what clang
// indexed), and those lines mapped to the classes, functions, fields and
// macros they are in.
namespace Unravel::History {

struct Commit {
    QString hash, shortHash;
    QString author, date, subject;
    QStringList parents;
    qint64 time = 0;   // seconds since the epoch
};

// The pseudo-commit for what isn't committed yet
inline const QString kWorkingTree = QStringLiteral("WORKTREE");

// The top of the work tree `dir` is in, or "" if it is in none (or git is missing)
QString repositoryRoot(const QString &dir);
// `path` relative to `repository` (as git rev-parse --show-toplevel gives
// it), through symlinks too: on macOS a temporary folder is /var/... while
// Git resolves it to /private/var/.... Starts with ../ when outside.
QString relativeToRepository(const QString &repository, const QString &path);

// The latest commits (first parent first), newest first; `path` limits
// them to those touching it
QVector<Commit> log(const QString &repo, int limit = 500, const QString &path = {});

struct FileActivity {
    qint64 commits = 0, added = 0, removed = 0;
    qint64 lastChanged = 0; // committer timestamp, seconds since epoch
    QSet<QString> authors; // mailmap-resolved author identities
    bool binary = false;  // at least one change has no line counts
};

struct Activity {
    QString repository, revision, error;
    QString cacheWarning;
    qint64 since = 0, until = 0;
    QHash<QString, FileActivity> files; // repository-relative keys; current paths in follow mode
    bool shallow = false;
    bool cancelled = false;
    bool cached = false;
    bool followedRenames = false; // Git --follow's heuristic, not proven identity

    bool ready() const { return !repository.isEmpty() && !revision.isEmpty() && error.isEmpty() && !cancelled; }
};

// Non-merge commits reachable from revision, inclusive committer-time bounds.
// Default: pathname counts. Optional --follow mode uses Git's rename heuristic.
// An empty repository with no error means dir is not in a Git worktree.
Activity fileActivity(const QString &dir, const QString &revision, qint64 since, qint64 until,
                      const std::atomic_bool *cancel = nullptr, const QString &cacheDirectory = {},
                      bool refreshCache = false, bool followRenames = false);

struct FunctionHunkStats {
    qint64 commits = 0, lines = 0, ambiguousCommits = 0;
    QSet<QString> ambiguity; // replacement, overlap, rename, post-revision
};

struct HunkActivity {
    QString repository, revision, error;
    qint64 since = 0, until = 0;
    qint64 mappingCommits = 0; // first-parent commits diffed, including merge diffs used only for mapping
    qint64 postRevisionChangedFiles = 0; // used for line mapping, never counted as historical activity
    QHash<qint32, FunctionHunkStats> functions; // symbol ids in the supplied model
    bool shallow = false, cancelled = false, firstParentOnly = true;

    bool ready() const { return !repository.isEmpty() && !revision.isEmpty() && error.isEmpty() && !cancelled; }
};

// Batch zero-context diffs along the revision's first-parent line, carrying
// changed lines to the current working tree and current function extents.
HunkActivity functionHunks(const QString &dir, const QString &revision, qint64 since, qint64 until,
                           const CodeModel &model, const std::atomic_bool *cancel = nullptr);

struct Blame {
    QString text, error;
    bool cancelled = false;
};
Blame blame(const QString &directory, const QString &path, int firstLine,
            const std::atomic_bool *cancel = nullptr);

// One hunk of a unified diff with no context: `oldCount` lines from
// `oldStart` became `newCount` lines from `newStart`
struct Hunk {
    qint32 oldStart = 0, oldCount = 0, newStart = 0, newCount = 0;
};
struct FileDiff {
    QString oldPath, newPath;   // relative to the repository, "" for /dev/null
    QVector<Hunk> hunks;
};
// Parses `git diff -U0` output
QVector<FileDiff> parseDiff(const QByteArray &diff);

// A line of a file's old version in its new version, through the hunks
// between them; inside a hunk (changed again since), the hunk's start
qint32 mapLine(const QVector<Hunk> &hunks, qint32 line, bool *changedSince = nullptr);

// Per absolute file path as it is now: the lines the commits changed (and
// the line before each deletion), and how many lines they added and removed
struct FileChange {
    QSet<qint32> lines;
    QSet<qint32> addedLines;   // of those, the ones that are new (not edits of old lines)
    qint32 added = 0, removed = 0;
    bool newFile = false, deleted = false;
    QStringList commits;   // short hashes
};
struct ChangeSet {
    QHash<QString, FileChange> files;
    QStringList commits;
    QString error;
};

// What `commits` changed, lines carried forward to the work tree; the
// pseudo-commit kWorkingTree adds what isn't committed (and untracked files)
ChangeSet changes(const QString &repo, const QStringList &commits, const QVector<Commit> &known = {});

// The symbols the changed lines are in
struct Touched {
    qint32 symbol = -1;
    qint32 lines = 0;      // changed lines in it
    bool added = false;    // (nearly) all of it is new
};
struct Mapping {
    QVector<Touched> symbols;          // sorted by file and line
    QHash<qint32, qint32> lineCount;   // per file id, changed lines
    QStringList outside;               // changed files the index doesn't know (not C/C++, new)
};
Mapping touchedSymbols(const CodeModel &m, const ChangeSet &changes);

} // namespace Unravel::History
