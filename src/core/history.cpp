#include "history.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QDateTime>
#include <QTimeZone>

#include <algorithm>
#include <functional>

namespace Unravel::History {

namespace {

bool runGit(const QString &repo, const QStringList &args, QByteArray *out, QString *error = nullptr,
            const std::atomic_bool *cancel = nullptr, int timeoutMs = 0, qsizetype maxBytes = 0)
{
    if (cancel && cancel->load()) {
        if (error)
            *error = QStringLiteral("Git history query cancelled");
        return false;
    }
    QProcess git;
    git.setProgram(QStringLiteral("git"));
    // Plain, stable output: no pager, colours, external diff tools or quoted paths
    git.setArguments(QStringList{QStringLiteral("-C"), repo, QStringLiteral("-c"), QStringLiteral("core.quotepath=off"),
                                 QStringLiteral("--no-pager")}
                     + args);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    git.setProcessEnvironment(env);
    git.start();
    if (!git.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("git could not be started");
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    QByteArray data;
    while (true) {
        const bool finished = git.waitForFinished(100);
        data += git.readAllStandardOutput();
        const bool tooLarge = maxBytes > 0 && data.size() > maxBytes;
        if ((cancel && cancel->load()) || (timeoutMs > 0 && timer.elapsed() >= timeoutMs) || tooLarge) {
            git.kill();
            git.waitForFinished();
            if (error) {
                if (cancel && cancel->load())
                    *error = QStringLiteral("Git history query cancelled");
                else if (tooLarge)
                    *error = QStringLiteral("Git history output is too large; choose a shorter time window");
                else
                    *error = QStringLiteral("Git history query timed out; choose a shorter time window");
            }
            return false;
        }
        if (finished || git.state() == QProcess::NotRunning)
            break;
    }
    data += git.readAllStandardOutput();
    if (git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0) {
        if (error) {
            *error = QString::fromLocal8Bit(git.readAllStandardError()).trimmed();
            if (error->isEmpty())
                *error = QStringLiteral("Git failed (exit code %1)").arg(git.exitCode());
        }
        return false;
    }
    *out = data;
    return true;
}

// The lines a hunk set touches on its new side; a pure deletion touches
// the line after which it was
void touchedLines(const FileDiff &d, QSet<qint32> &lines, QSet<qint32> &added, qint32 *plus, qint32 *minus)
{
    for (const Hunk &h : d.hunks) {
        *plus += h.newCount;
        *minus += h.oldCount;
        if (h.newCount == 0) {
            lines.insert(std::max(1, h.newStart));
            continue;
        }
        for (qint32 l = h.newStart; l < h.newStart + h.newCount; ++l) {
            lines.insert(l);
            if (h.oldCount == 0)
                added.insert(l);
        }
    }
}

qint32 lineCount(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray data = f.readAll();
    return qint32(data.count('\n') + (data.isEmpty() || data.endsWith('\n') ? 0 : 1));
}

QVector<FileDiff> parseDiffChecked(const QByteArray &diff, const std::function<bool()> &checkpoint);

} // namespace

QString repositoryRoot(const QString &dir)
{
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
        return {};
    QByteArray out;
    if (!runGit(dir, {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")}, &out))
        return {};
    return QDir::cleanPath(QString::fromUtf8(out).trimmed());
}

QVector<Commit> log(const QString &repo, int limit, const QString &path)
{
    QVector<Commit> commits;
    QStringList args = {QStringLiteral("log"), QStringLiteral("-n"), QString::number(limit),
                        QStringLiteral("--date=short"),
                        QStringLiteral("--format=%H%x1f%h%x1f%an%x1f%ad%x1f%at%x1f%P%x1f%s%x1e")};
    if (!path.isEmpty())
        args << QStringLiteral("--") << path;
    QByteArray out;
    if (!runGit(repo, args, &out))
        return commits;
    for (const QByteArray &record : out.split('\x1e')) {
        const QList<QByteArray> f = record.trimmed().split('\x1f');
        if (f.size() < 7)
            continue;
        Commit c;
        c.hash = QString::fromUtf8(f[0]);
        c.shortHash = QString::fromUtf8(f[1]);
        c.author = QString::fromUtf8(f[2]);
        c.date = QString::fromUtf8(f[3]);
        c.time = f[4].toLongLong();
        c.parents = QString::fromUtf8(f[5]).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        c.subject = QString::fromUtf8(f[6]);
        commits.append(c);
    }
    return commits;
}

Activity fileActivity(const QString &dir, const QString &revision, qint64 since, qint64 until,
                      const std::atomic_bool *cancel, const QString &cacheDirectory, bool refreshCache, bool followRenames)
{
    Activity result;
    result.since = since;
    result.until = until;
    result.followedRenames = followRenames;
    if (since < 0 || until < since) {
        result.error = QStringLiteral("Invalid Git history time window");
        return result;
    }
    const auto from = QDateTime::fromSecsSinceEpoch(since, QTimeZone::UTC);
    const auto to = QDateTime::fromSecsSinceEpoch(until, QTimeZone::UTC);
    if (!from.isValid() || !to.isValid()) {
        result.error = QStringLiteral("Git history window is outside the supported date range");
        return result;
    }
    const QString sinceArg = QStringLiteral("--since-as-filter=") + from.toString(Qt::ISODate);
    const QString untilArg = QStringLiteral("--until=") + to.toString(Qt::ISODate);
    QString workingDirectory = dir;
    auto query = [&](const QStringList &args, QByteArray *out) {
        const bool ok = runGit(workingDirectory, args, out, &result.error, cancel, 120000, 128 * 1024 * 1024);
        result.cancelled = cancel && cancel->load();
        return ok && !result.cancelled;
    };
    QByteArray out;
    if (!query({QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")}, &out)) {
        if (!result.cancelled && result.error.contains(QLatin1String("not a git repository")))
            result.error.clear();
        return result;
    }
    result.repository = QDir::cleanPath(QString::fromUtf8(out).trimmed());
    workingDirectory = result.repository;
    if (!query({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--end-of-options"),
                revision + QStringLiteral("^{commit}")}, &out))
        return result;
    result.revision = QString::fromUtf8(out).trimmed();
    if (!query({QStringLiteral("rev-parse"), QStringLiteral("--is-shallow-repository")}, &out))
        return result;
    result.shallow = out.trimmed() == "true";
    QString cachePath;
    if (!cacheDirectory.isEmpty()) {
        if (!query({QStringLiteral("--version")}, &out))
            return result;
        const QByteArray gitVersion = out;
        // A cheap metadata walk also invalidates replacements, changed graph
        // boundaries and mailmap resolutions, without diffing file contents.
        if (!query({QStringLiteral("log"), QStringLiteral("--no-merges"), QStringLiteral("--no-show-signature"),
                    QStringLiteral("--no-notes"), QStringLiteral("--format=%H %T %P %ct %aE"),
                    sinceArg, untilArg,
                    result.revision, QStringLiteral("--")}, &out))
            return result;
        const QByteArray key = result.repository.toUtf8() + '\0' + result.revision.toUtf8() + '\0'
            + QByteArray::number(since) + ':' + QByteArray::number(until) + ':' + QByteArray::number(result.shallow)
            + (followRenames ? ":git-follow-v3:" : ":pathname-v2:") + gitVersion + '\0' + out;
        cachePath = QDir(cacheDirectory).filePath(QString::fromLatin1(
            QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex()) + QStringLiteral(".json"));
        QFile cache(cachePath);
        if (!refreshCache && cache.exists()) {
            if (cache.size() > 128 * 1024 * 1024) {
                result.cacheWarning = QStringLiteral("History cache exceeds 128 MiB; recomputed from Git");
            } else if (!cache.open(QIODevice::ReadOnly)) {
                result.cacheWarning = QStringLiteral("Cannot read history cache: %1").arg(cache.errorString());
            } else {
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(cache.readAll(), &error);
                bool valid = error.error == QJsonParseError::NoError && document.isObject()
                    && document.object().value(QStringLiteral("version")).toInt() == 1
                    && document.object().value(QStringLiteral("files")).isArray();
                for (const auto &entry : document.object().value(QStringLiteral("files")).toArray()) {
                    if (!valid)
                        break;
                    const QJsonObject object = entry.toObject();
                    const QString path = object.value(QStringLiteral("path")).toString();
                    FileActivity file;
                    file.commits = object.value(QStringLiteral("commits")).toInteger(-1);
                    file.added = object.value(QStringLiteral("added")).toInteger(-1);
                    file.removed = object.value(QStringLiteral("removed")).toInteger(-1);
                    file.lastChanged = object.value(QStringLiteral("lastChanged")).toInteger(-1);
                    valid = !path.isEmpty() && file.commits > 0 && file.added >= 0 && file.removed >= 0
                        && file.lastChanged >= since && file.lastChanged <= until
                        && object.value(QStringLiteral("binary")).isBool()
                        && object.value(QStringLiteral("authors")).isArray() && !result.files.contains(path);
                    file.binary = object.value(QStringLiteral("binary")).toBool();
                    for (const auto &author : object.value(QStringLiteral("authors")).toArray()) {
                        valid &= author.isString() && !author.toString().isEmpty();
                        file.authors.insert(author.toString());
                    }
                    valid &= !file.authors.isEmpty();
                    result.files.insert(path, file);
                }
                if (valid) {
                    result.cached = true;
                    return result;
                }
                result.files.clear();
                result.cacheWarning = QStringLiteral("Invalid history cache; recomputed from Git");
            }
        }
    }
    QStringList logArgs{QStringLiteral("log"), QStringLiteral("--no-merges"),
                QStringLiteral("--no-show-signature"), QStringLiteral("--no-notes"), QStringLiteral("--no-color"),
                QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"), QStringLiteral("--numstat"),
                QStringLiteral("-z"), QStringLiteral("--format=%x00UNRAVEL-COMMIT%x00%H%x00%ct%x00%aE%x00"),
                sinceArg, untilArg};

    // NUL framing preserves spaces, tabs, and newlines in filenames. Only
    // the first two tabs of a numstat record separate fields.
    auto consume = [&](const QString &currentPath) {
        const QList<QByteArray> records = out.split('\0');
        QString author;
        qint64 time = 0;
        bool inCommit = false;
        QSet<QString> touched;
        for (qsizetype i = 0; i < records.size(); ++i) {
            if (cancel && cancel->load()) {
                result.cancelled = true;
                result.files.clear();
                return;
            }
            QByteArray record = records[i];
            if (record.isEmpty() || record == "\n")
                continue;
            if (record == "UNRAVEL-COMMIT") {
                if (i + 3 >= records.size()) {
                    result.error = QStringLiteral("Incomplete Git history commit record");
                    break;
                }
                ++i; // the commit hash
                bool ok = false;
                time = records[++i].toLongLong(&ok);
                author = QString::fromUtf8(records[++i]);
                if (!ok || author.isEmpty()) {
                    result.error = QStringLiteral("Invalid Git history commit record");
                    break;
                }
                touched.clear();
                inCommit = true;
                continue;
            }
            if (record.startsWith('\n'))
                record.remove(0, 1);
            const qsizetype first = record.indexOf('\t');
            const qsizetype second = record.indexOf('\t', first + 1);
            if (!inCommit || first <= 0 || second <= first) {
                result.error = QStringLiteral("Invalid Git history file record");
                break;
            }
            const QByteArray plus = record.left(first), minus = record.mid(first + 1, second - first - 1);
            const bool binary = plus == "-" && minus == "-";
            bool plusOk = false, minusOk = false;
            const qint64 added = plus.toLongLong(&plusOk), removed = minus.toLongLong(&minusOk);
            if (!binary && (!plusOk || !minusOk || added < 0 || removed < 0)) {
                result.error = QStringLiteral("Invalid Git history line counts");
                break;
            }
            QString path = QString::fromUtf8(record.mid(second + 1));
            if (path.isEmpty()) {
                if (i + 2 >= records.size() || currentPath.isEmpty()) {
                    result.error = QStringLiteral("Invalid Git rename record");
                    break;
                }
                ++i; // the old pathname
                path = QString::fromUtf8(records[++i]);
            }
            if (time < since || time > until)
                continue;
            if (!currentPath.isEmpty())
                path = currentPath;
            FileActivity &file = result.files[path];
            if (!touched.contains(path)) {
                ++file.commits;
                touched.insert(path);
            }
            file.added += binary ? 0 : added;
            file.removed += binary ? 0 : removed;
            file.binary |= binary;
            file.lastChanged = std::max(file.lastChanged, time);
            file.authors.insert(author);
        }
    };
    if (followRenames) {
        if (!query({QStringLiteral("ls-tree"), QStringLiteral("-r"), QStringLiteral("--name-only"),
                    QStringLiteral("-z"), result.revision, QStringLiteral("--")}, &out))
            return result;
        const QList<QByteArray> paths = out.split('\0');
        QElapsedTimer timer;
        timer.start();
        for (const QByteArray &path : paths) {
            if (path.isEmpty())
                continue;
            if (timer.elapsed() >= 120000) {
                result.error = QStringLiteral("Rename following exceeded two minutes; use pathname history or a smaller project");
                break;
            }
            if (!query(logArgs + QStringList{QStringLiteral("--follow"), QStringLiteral("--find-renames"),
                        result.revision, QStringLiteral("--"), QString::fromUtf8(path)}, &out))
                break;
            consume(QString::fromUtf8(path));
            if (!result.error.isEmpty() || result.cancelled)
                break;
        }
    } else {
        if (!query(logArgs + QStringList{QStringLiteral("--no-renames"), result.revision, QStringLiteral("--")}, &out))
            return result;
        consume({});
    }
    if (result.cancelled) {
        result.files.clear();
        return result;
    }
    if (!result.error.isEmpty())
        result.files.clear();
    else if (!cachePath.isEmpty()) {
        QJsonArray files;
        QStringList paths = result.files.keys();
        paths.sort();
        for (const QString &path : paths) {
            const auto &file = result.files[path];
            QJsonArray authors;
            QStringList identities(file.authors.begin(), file.authors.end());
            identities.sort();
            for (const QString &author : identities)
                authors.append(author);
            files.append(QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("commits"), file.commits},
                {QStringLiteral("added"), file.added}, {QStringLiteral("removed"), file.removed},
                {QStringLiteral("lastChanged"), file.lastChanged}, {QStringLiteral("binary"), file.binary},
                {QStringLiteral("authors"), authors}});
        }
        QSaveFile cache(cachePath);
        const QByteArray data = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
            {QStringLiteral("files"), files}}).toJson(QJsonDocument::Compact);
        if (!QDir().mkpath(cacheDirectory) || !cache.open(QIODevice::WriteOnly)
            || cache.write(data) != data.size() || !cache.commit())
            result.cacheWarning = QStringLiteral("Cannot save history cache: %1").arg(cache.errorString());
    }
    return result;
}

HunkActivity functionHunks(const QString &dir, const QString &revision, qint64 since, qint64 until,
                           const CodeModel &model, const std::atomic_bool *cancel)
{
    HunkActivity result;
    result.since = since;
    result.until = until;
    if (since < 0 || until < since) {
        result.error = QStringLiteral("Invalid Git history time window");
        return result;
    }
    if (!QDateTime::fromSecsSinceEpoch(since, QTimeZone::UTC).isValid()
        || !QDateTime::fromSecsSinceEpoch(until, QTimeZone::UTC).isValid()) {
        result.error = QStringLiteral("Git history window is outside the supported date range");
        return result;
    }

    QString workingDirectory = dir;
    QByteArray output;
    auto query = [&](const QStringList &args) {
        const bool ok = runGit(workingDirectory, args, &output, &result.error, cancel,
                               120000, 128 * 1024 * 1024);
        result.cancelled = cancel && cancel->load();
        return ok && !result.cancelled;
    };
    if (!query({QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")})) {
        if (!result.cancelled && result.error.contains(QLatin1String("not a git repository")))
            result.error = QStringLiteral("Function hunk history requires a Git repository");
        return result;
    }
    result.repository = QDir::cleanPath(QString::fromUtf8(output).trimmed());
    workingDirectory = result.repository;
    if (!query({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--end-of-options"),
                revision + QStringLiteral("^{commit}")}))
        return result;
    result.revision = QString::fromUtf8(output).trimmed();
    if (!query({QStringLiteral("rev-parse"), QStringLiteral("--is-shallow-repository")}))
        return result;
    result.shallow = output.trimmed() == "true";

    struct CommitDiff {
        QString hash;
        qint64 time = 0;
        bool merge = false;
        QVector<FileDiff> files;
    };
    constexpr int maxCommits = 10000;
    constexpr qint64 maxMetadataCommits = 1000000;
    constexpr qint64 maxMappingWork = 2000000;
    QElapsedTimer mappingTimer;
    mappingTimer.start();
    qint64 mappingWork = 0;
    auto consumeMappingWork = [&](qint64 amount = 1) {
        mappingWork += amount;
        if (cancel && cancel->load()) {
            result.cancelled = true;
            result.error = QStringLiteral("Git function hunk mapping cancelled");
            return false;
        }
        if (mappingTimer.elapsed() >= 120000) {
            result.error = QStringLiteral("Function hunk mapping exceeded two minutes; choose a shorter window or revision");
            return false;
        }
        if (mappingWork > maxMappingWork) {
            result.error = QStringLiteral("Function hunk mapping exceeded two million operations; choose a shorter window");
            return false;
        }
        return true;
    };

    // Walk commit metadata without patches first. Timestamps can be out of
    // order, so scan the bounded first-parent chain rather than stopping at
    // the first commit older than `since`.
    const QStringList metadataArgs{QStringLiteral("rev-list"), QStringLiteral("--first-parent"),
        QStringLiteral("--timestamp"), QStringLiteral("--parents"),
        QStringLiteral("--max-count=%1").arg(maxMetadataCommits + 1), result.revision};
    if (!query(metadataArgs)) {
        if (!result.cancelled && (result.error.contains(QLatin1String("output is too large"))
                                  || result.error.contains(QLatin1String("timed out"))))
            result.error = QStringLiteral("First-parent metadata walk exceeded its time/output bound; use a shallower history boundary");
        return result;
    }
    QElapsedTimer metadataTimer;
    metadataTimer.start();
    qint64 metadataCount = 0;
    qint64 oldestEligibleIndex = -1;
    for (qsizetype pos = 0; pos < output.size();) {
        if (cancel && cancel->load()) {
            result.cancelled = true;
            result.error = QStringLiteral("Git function hunk metadata walk cancelled");
            return result;
        }
        if (metadataTimer.elapsed() >= 120000) {
            result.error = QStringLiteral("First-parent metadata walk exceeded two minutes; use a shallower history boundary");
            return result;
        }
        const qsizetype end = output.indexOf('\n', pos);
        const QByteArray record = output.mid(pos, end < 0 ? output.size() - pos : end - pos);
        pos = end < 0 ? output.size() : end + 1;
        if (record.isEmpty())
            continue;
        const qsizetype firstSpace = record.indexOf(' ');
        const qsizetype secondSpace = firstSpace < 0 ? -1 : record.indexOf(' ', firstSpace + 1);
        if (firstSpace <= 0) {
            result.error = QStringLiteral("Invalid first-parent history metadata record");
            return result;
        }
        bool timeOk = false;
        const qint64 time = record.left(firstSpace).toLongLong(&timeOk);
        const QByteArray hash = record.mid(firstSpace + 1,
            (secondSpace < 0 ? record.size() : secondSpace) - firstSpace - 1);
        const QByteArray parents = secondSpace < 0 ? QByteArray() : record.mid(secondSpace + 1).trimmed();
        if (!timeOk || (hash.size() != 40 && hash.size() != 64)) {
            result.error = QStringLiteral("Invalid first-parent history metadata");
            return result;
        }
        const bool merge = parents.contains(' ');
        if (!merge && time >= since && time <= until)
            oldestEligibleIndex = metadataCount;
        ++metadataCount;
        if (metadataCount > maxMetadataCommits) {
            result.error = QStringLiteral("First-parent metadata walk exceeds one million commits; use a shallower history boundary");
            return result;
        }
    }
    if (oldestEligibleIndex < 0)
        return result;
    const qint64 mappingSpan = oldestEligibleIndex + 1;
    if (mappingSpan > maxCommits) {
        result.error = QStringLiteral("Required first-parent line-mapping span exceeds 10,000 commits; choose a shorter window or a revision closer to it");
        return result;
    }
    result.mappingCommits = mappingSpan;

    const QByteArray marker("UNRAVEL-FUNCTION-HUNK-COMMIT");
    const QStringList logArgs{
        QStringLiteral("log"), QStringLiteral("--first-parent"), QStringLiteral("--diff-merges=first-parent"),
        QStringLiteral("--no-show-signature"), QStringLiteral("--no-notes"), QStringLiteral("--no-color"),
        QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"), QStringLiteral("--find-renames"),
        QStringLiteral("-U0"),
        QStringLiteral("--format=%x00UNRAVEL-FUNCTION-HUNK-COMMIT%x00%H%x00%ct%x00%P%x00"),
        QStringLiteral("--max-count=%1").arg(mappingSpan), result.revision, QStringLiteral("--")
    };
    if (!query(logArgs))
        return result;

    const QList<QByteArray> records = output.split('\0');
    QVector<CommitDiff> commits;
    for (qsizetype i = 1; i < records.size();) {
        if (!consumeMappingWork())
            return result;
        if (records[i] != marker) {
            if (records[i].trimmed().isEmpty()) {
                ++i;
                continue;
            }
            result.error = QStringLiteral("Invalid batched Git function-diff record");
            return result;
        }
        if (i + 4 >= records.size()) {
            result.error = QStringLiteral("Incomplete batched Git function-diff record");
            return result;
        }
        CommitDiff commit;
        commit.hash = QString::fromLatin1(records[i + 1]);
        bool timeOk = false;
        commit.time = records[i + 2].toLongLong(&timeOk);
        if ((commit.hash.size() != 40 && commit.hash.size() != 64) || !timeOk) {
            result.error = QStringLiteral("Invalid batched Git function-diff metadata");
            return result;
        }
        commit.merge = !records[i + 3].trimmed().isEmpty()
            && records[i + 3].trimmed().contains(' ');
        commit.files = parseDiffChecked(records[i + 4], [&] { return consumeMappingWork(); });
        if (result.cancelled || !result.error.isEmpty())
            return result;
        commits.append(std::move(commit));
        i += 5;
    }
    if (commits.size() != mappingSpan) {
        result.error = QStringLiteral("Incomplete first-parent function-diff mapping span");
        return result;
    }

    // This final step maps selected-revision lines into the indexed working
    // tree. Changes after the revision are never counted as historical commits.
    if (!query({QStringLiteral("diff"), QStringLiteral("-U0"), QStringLiteral("--no-color"),
                QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"),
                QStringLiteral("--find-renames"), result.revision, QStringLiteral("--")}))
        return result;
    const QVector<FileDiff> workingTreeDiff = parseDiffChecked(output, [&] { return consumeMappingWork(); });
    if (result.cancelled || !result.error.isEmpty())
        return result;
    result.postRevisionChangedFiles = workingTreeDiff.size();
    if (!query({QStringLiteral("ls-files"), QStringLiteral("-z")}))
        return result;
    QSet<QString> trackedPaths;
    for (const QByteArray &path : output.split('\0')) {
        if (!consumeMappingWork())
            return result;
        if (!path.isEmpty())
            trackedPaths.insert(QString::fromUtf8(path));
    }

    struct Extent {
        qint32 symbol = -1, first = 0, last = 0;
    };
    QHash<QString, QVector<Extent>> extents;
    for (qint32 file = 0; file < model.files.size(); ++file) {
        if (!consumeMappingWork())
            return result;
        if (!model.files[file].project || model.files[file].generated)
            continue;
        const QString path = QDir(result.repository).relativeFilePath(model.files[file].path);
        if (QDir::isAbsolutePath(path) || path == QLatin1String("..") || path.startsWith(QLatin1String("../")))
            continue;
        auto &items = extents[path];
        for (qint32 symbol : model.files[file].symbols) {
            if (!consumeMappingWork())
                return result;
            const Symbol &function = model.symbols[symbol];
            if (!isFunction(function.kind) || !function.def.valid() || function.has(External)
                || function.has(Generated) || function.has(Implicit) || function.has(FromMacro)
                || function.def.file != file)
                continue;
            items.append({symbol, function.def.line, function.def.endLine});
        }
        if (!items.isEmpty() && !trackedPaths.contains(path)) {
            result.error = QStringLiteral("Function hunk history cannot estimate untracked project file '%1'; no zero-history estimate was produced")
                               .arg(path);
            return result;
        }
        std::sort(items.begin(), items.end(), [](const Extent &a, const Extent &b) {
            if (a.first != b.first)
                return a.first < b.first;
            return a.last < b.last;
        });
    }

    struct MappingResult {
        QString path;
        qint32 line = 0;
        QSet<QString> ambiguity;
        bool valid = true;
    };
    auto mapMappedLine = [&](const QVector<Hunk> &hunks, qint32 *line, bool *changed) {
        if (changed)
            *changed = false;
        const qint32 originalLine = *line;
        qint32 offset = 0;
        for (const Hunk &hunk : hunks) {
            if (!consumeMappingWork())
                return false;
            if (hunk.oldCount > 0 && originalLine >= hunk.oldStart
                && originalLine < hunk.oldStart + hunk.oldCount) {
                if (changed)
                    *changed = true;
                *line = std::max(1, hunk.newStart);
                return true;
            }
            const qint32 lastOld = hunk.oldCount > 0 ? hunk.oldStart + hunk.oldCount - 1 : hunk.oldStart;
            if (lastOld < originalLine)
                offset += hunk.newCount - hunk.oldCount;
            else
                break;
        }
        *line = std::max(1, originalLine + offset);
        return true;
    };
    auto advance = [&](const QVector<FileDiff> &diffs, MappingResult &mapped, bool postRevision = false) {
        const FileDiff *step = nullptr;
        for (const FileDiff &diff : diffs) {
            if (!consumeMappingWork())
                return false;
            if (diff.oldPath == mapped.path) {
                step = &diff;
                break;
            }
        }
        if (!step)
            return true;
        bool replaced = false;
        if (!mapMappedLine(step->hunks, &mapped.line, &replaced))
            return false;
        if (replaced) {
            mapped.ambiguity.insert(QStringLiteral("replacement"));
            if (postRevision)
                mapped.ambiguity.insert(QStringLiteral("post-revision"));
        }
        if (step->newPath.isEmpty()) {
            mapped.valid = false;
            mapped.ambiguity.insert(QStringLiteral("replacement"));
            return true;
        }
        if (step->newPath != mapped.path) {
            mapped.path = step->newPath;
            mapped.ambiguity.insert(QStringLiteral("rename"));
            if (postRevision)
                mapped.ambiguity.insert(QStringLiteral("post-revision"));
        }
        return true;
    };

    constexpr qint64 maxMappedLines = 2000000;
    qint64 mappedLineCount = 0;
    for (qsizetype index = 0; index < commits.size(); ++index) {
        if (!consumeMappingWork()) {
            result.functions.clear();
            return result;
        }
        const CommitDiff &commit = commits[index];
        const bool inWindow = commit.time >= since && commit.time <= until && !commit.merge;
        if (!inWindow)
            continue;

        QHash<qint32, QSet<qint32>> linesByFunction;
        QHash<qint32, QSet<QString>> ambiguityByFunction;
        for (const FileDiff &fileDiff : commit.files) {
            if (!consumeMappingWork()) {
                result.functions.clear();
                return result;
            }
            if (fileDiff.newPath.isEmpty())
                continue;
            const QString originalPath = fileDiff.newPath;
            for (const Hunk &hunk : fileDiff.hunks) {
                if (!consumeMappingWork()) {
                    result.functions.clear();
                    return result;
                }
                const qint32 count = hunk.newCount > 0 ? hunk.newCount : 1;
                for (qint32 offset = 0; offset < count; ++offset) {
                    if (++mappedLineCount > maxMappedLines) {
                        result.error = QStringLiteral("Function hunk history exceeds two million changed lines; choose a shorter time window");
                        result.functions.clear();
                        return result;
                    }
                    if (!consumeMappingWork()) {
                        result.functions.clear();
                        return result;
                    }
                    MappingResult mapped{originalPath, hunk.newCount > 0
                        ? hunk.newStart + offset : std::max(1, hunk.newStart), {}, true};
                    if (hunk.newCount == 0)
                        mapped.ambiguity.insert(QStringLiteral("replacement"));

                    // Commits are newest first. Apply newer transitions in
                    // chronological order, then carry into the work tree.
                    for (qsizetype newer = index - 1; newer >= 0 && mapped.valid; --newer) {
                        if (!consumeMappingWork() || !advance(commits[newer].files, mapped)) {
                            result.functions.clear();
                            return result;
                        }
                    }
                    if (mapped.valid && !advance(workingTreeDiff, mapped, true)) {
                        result.functions.clear();
                        return result;
                    }
                    if (!mapped.valid)
                        continue;

                    const auto functions = extents.constFind(mapped.path);
                    if (functions == extents.cend())
                        continue;
                    QVector<qint32> owners;
                    for (const Extent &extent : *functions) {
                        if (!consumeMappingWork()) {
                            result.functions.clear();
                            return result;
                        }
                        if (extent.first > mapped.line)
                            break;
                        if (mapped.line <= extent.last)
                            owners.append(extent.symbol);
                    }
                    if (owners.isEmpty())
                        continue;
                    if (owners.size() > 1)
                        mapped.ambiguity.insert(QStringLiteral("overlap"));
                    for (qint32 symbol : owners) {
                        if (!consumeMappingWork()) {
                            result.functions.clear();
                            return result;
                        }
                        if (mapped.ambiguity.contains(QStringLiteral("overlap"))) {
                            ambiguityByFunction[symbol].unite(mapped.ambiguity);
                        } else {
                            linesByFunction[symbol].insert(mapped.line);
                            ambiguityByFunction[symbol].unite(mapped.ambiguity);
                        }
                    }
                }
            }
        }
        QSet<qint32> affected;
        for (auto it = linesByFunction.cbegin(); it != linesByFunction.cend(); ++it) {
            if (!consumeMappingWork()) {
                result.functions.clear();
                return result;
            }
            affected.insert(it.key());
        }
        for (auto it = ambiguityByFunction.cbegin(); it != ambiguityByFunction.cend(); ++it) {
            if (!consumeMappingWork()) {
                result.functions.clear();
                return result;
            }
            affected.insert(it.key());
        }
        for (qint32 symbol : std::as_const(affected)) {
            if (!consumeMappingWork()) {
                result.functions.clear();
                return result;
            }
            FunctionHunkStats &stats = result.functions[symbol];
            if (!linesByFunction.value(symbol).isEmpty())
                ++stats.commits;
            stats.lines += linesByFunction.value(symbol).size();
            const QSet<QString> ambiguity = ambiguityByFunction.value(symbol);
            if (!ambiguity.isEmpty()) {
                ++stats.ambiguousCommits;
                stats.ambiguity.unite(ambiguity);
            }
        }
    }
    return result;
}

Blame blame(const QString &directory, const QString &path, int firstLine, const std::atomic_bool *cancel)
{
    Blame result;
    if (firstLine < 1) {
        result.error = QStringLiteral("Select a source line for blame");
        return result;
    }
    QByteArray output;
    QString workingDirectory = directory;
    auto query = [&](const QStringList &arguments) {
        const bool ok = runGit(workingDirectory, arguments, &output, &result.error, cancel, 30000, 4 * 1024 * 1024);
        result.cancelled = cancel && cancel->load();
        return ok && !result.cancelled;
    };
    if (!query({QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")}))
        return result;
    const QString root = QString::fromUtf8(output).trimmed();
    workingDirectory = root;
    const QString relative = QDir(root).relativeFilePath(path);
    if (QDir::isAbsolutePath(relative) || relative == QLatin1String("..") || relative.startsWith(QLatin1String("../"))) {
        result.error = QStringLiteral("Blame source is outside the Git repository");
        return result;
    }
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        result.error = source.errorString();
        return result;
    }
    if (source.size() > 32 * 1024 * 1024) {
        result.error = QStringLiteral("Blame source exceeds 32 MiB");
        return result;
    }
    const QByteArray contents = source.readAll();
    if (source.error() != QFileDevice::NoError) {
        result.error = source.errorString();
        return result;
    }
    const auto lines = contents.count('\n') + (!contents.isEmpty() && !contents.endsWith('\n') ? 1 : 0);
    if (firstLine > lines) {
        result.error = QStringLiteral("Selected blame line is outside the current file");
        return result;
    }
    const auto count = std::min(qsizetype(80), lines - firstLine + 1);
    if (!query({QStringLiteral("blame"), QStringLiteral("--line-porcelain"), QStringLiteral("--encoding=UTF-8"),
                QStringLiteral("--contents=") + path, QStringLiteral("-L"),
                QStringLiteral("%1,+%2").arg(firstLine).arg(count), QStringLiteral("--"), relative}))
        return result;
    QString commit, author, summary;
    int line = 0, emitted = 0;
    for (const QByteArray &record : output.split('\n')) {
        if (record.startsWith('\t')) {
            if (commit.isEmpty() || author.isEmpty() || line < firstLine) {
                result.error = QStringLiteral("Incomplete Git blame record");
                result.text.clear();
                return result;
            }
            const bool uncommitted = std::all_of(commit.cbegin(), commit.cend(), [](QChar c) { return c == QLatin1Char('0'); });
            result.text += QStringLiteral("%1 | %2 | %3 | %4\n").arg(line)
                .arg(uncommitted ? QStringLiteral("worktree") : commit.left(9), author, summary);
            commit.clear();
            ++emitted;
        } else if (record.startsWith("author ")) {
            author = QString::fromUtf8(record.mid(7));
        } else if (record.startsWith("summary ")) {
            summary = QString::fromUtf8(record.mid(8));
        } else if (commit.isEmpty()) {
            const auto header = record.split(' ');
            if (header.size() < 3)
                continue;
            bool ok = false;
            line = header[2].toInt(&ok);
            if (!ok || (header[0].size() != 40 && header[0].size() != 64)) {
                result.error = QStringLiteral("Invalid Git blame record");
                result.text.clear();
                return result;
            }
            commit = QString::fromLatin1(header[0]);
            author.clear();
            summary.clear();
        }
    }
    if (emitted != count) {
        result.error = QStringLiteral("Incomplete Git blame output");
        result.text.clear();
    }
    return result;
}

namespace {

QVector<FileDiff> parseDiffChecked(const QByteArray &diff, const std::function<bool()> &checkpoint)
{
    QVector<FileDiff> files;
    FileDiff *cur = nullptr;
    auto path = [](const QByteArray &rest) -> QString {
        // "a/dir/file" or "/dev/null"
        const QByteArray p = rest.trimmed();
        if (p == "/dev/null")
            return {};
        return QString::fromUtf8(p.size() > 2 && (p.startsWith("a/") || p.startsWith("b/")) ? p.mid(2) : p);
    };
    auto number = [](const QByteArray &s, qint32 *start, qint32 *count) {
        const qsizetype comma = s.indexOf(',');
        *start = (comma < 0 ? s : s.left(comma)).toInt();
        *count = comma < 0 ? 1 : s.mid(comma + 1).toInt();
    };
    for (qsizetype pos = 0; pos <= diff.size();) {
        if (checkpoint && !checkpoint())
            return {};
        const qsizetype end = diff.indexOf('\n', pos);
        const QByteArray line = diff.mid(pos, end < 0 ? diff.size() - pos : end - pos);
        if (end < 0)
            pos = diff.size() + 1;
        else
            pos = end + 1;
        if (line.startsWith("diff --git ")) {
            files.append(FileDiff());
            cur = &files.last();
            // Paths from the header, for diffs without ---/+++ (pure renames, binary)
            const QList<QByteArray> parts = line.mid(11).split(' ');
            if (parts.size() >= 2) {
                cur->oldPath = path(parts.first());
                cur->newPath = path(parts.last());
            }
        } else if (!cur) {
            continue;
        } else if (line.startsWith("--- ")) {
            cur->oldPath = path(line.mid(4));
        } else if (line.startsWith("+++ ")) {
            cur->newPath = path(line.mid(4));
        } else if (line.startsWith("rename to ")) {
            cur->newPath = QString::fromUtf8(line.mid(10));
        } else if (line.startsWith("rename from ")) {
            cur->oldPath = QString::fromUtf8(line.mid(12));
        } else if (line.startsWith("new file mode")) {
            cur->oldPath.clear();
        } else if (line.startsWith("deleted file mode")) {
            cur->newPath.clear();
        } else if (line.startsWith("@@ ")) {
            // @@ -a,b +c,d @@
            const QList<QByteArray> parts = line.split(' ');
            if (parts.size() < 3)
                continue;
            Hunk h;
            number(parts[1].mid(1), &h.oldStart, &h.oldCount);
            number(parts[2].mid(1), &h.newStart, &h.newCount);
            cur->hunks.append(h);
        }
    }
    return files;
}

} // namespace

QVector<FileDiff> parseDiff(const QByteArray &diff)
{
    return parseDiffChecked(diff, {});
}

qint32 mapLine(const QVector<Hunk> &hunks, qint32 line, bool *changedSince)
{
    if (changedSince)
        *changedSince = false;
    qint32 offset = 0;
    for (const Hunk &h : hunks) {
        if (h.oldCount > 0 && line >= h.oldStart && line < h.oldStart + h.oldCount) {
            if (changedSince)
                *changedSince = true;
            return std::max(1, h.newStart);
        }
        const qint32 lastOld = h.oldCount > 0 ? h.oldStart + h.oldCount - 1 : h.oldStart;
        if (lastOld < line)
            offset += h.newCount - h.oldCount;
        else
            break;
    }
    return std::max(1, line + offset);
}

ChangeSet changes(const QString &repo, const QStringList &commits, const QVector<Commit> &known)
{
    ChangeSet set;
    set.commits = commits;
    const QString top = QDir::cleanPath(repo) + QLatin1Char('/');
    for (const QString &hash : commits) {
        QByteArray out;
        QString error;
        if (hash == kWorkingTree) {
            // Staged and unstaged, against the last commit
            if (!runGit(repo, {QStringLiteral("diff"), QStringLiteral("-U0"), QStringLiteral("--no-color"),
                               QStringLiteral("--no-ext-diff"), QStringLiteral("-M"), QStringLiteral("HEAD")}, &out, &error)) {
                set.error = error;
                continue;
            }
            for (const FileDiff &d : parseDiff(out)) {
                if (d.newPath.isEmpty())
                    continue;
                FileChange &fc = set.files[top + d.newPath];
                touchedLines(d, fc.lines, fc.addedLines, &fc.added, &fc.removed);
                fc.newFile = fc.newFile || d.oldPath.isEmpty();
                fc.commits << QStringLiteral("work tree");
            }
            // New files git doesn't know yet
            QByteArray untracked;
            if (runGit(repo, {QStringLiteral("ls-files"), QStringLiteral("--others"), QStringLiteral("--exclude-standard"),
                              QStringLiteral("-z")}, &untracked)) {
                for (const QByteArray &p : untracked.split('\0')) {
                    if (p.isEmpty())
                        continue;
                    const QString abs = top + QString::fromUtf8(p);
                    FileChange &fc = set.files[abs];
                    const qint32 n = lineCount(abs);
                    for (qint32 l = 1; l <= n; ++l) {
                        fc.lines.insert(l);
                        fc.addedLines.insert(l);
                    }
                    fc.added += n;
                    fc.newFile = true;
                    fc.commits << QStringLiteral("untracked");
                }
            }
            continue;
        }
        // The commit against its first parent (the root commit against nothing)
        QStringList parents;
        for (const Commit &c : known) {
            if (c.hash == hash || c.shortHash == hash)
                parents = c.parents;
        }
        if (parents.isEmpty()) {
            QByteArray p;
            if (runGit(repo, {QStringLiteral("rev-list"), QStringLiteral("--parents"), QStringLiteral("-n1"), hash}, &p))
                parents = QString::fromUtf8(p).trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts).mid(1);
        }
        const QStringList common = {QStringLiteral("-U0"), QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"),
                                    QStringLiteral("-M")};
        const bool ok = parents.isEmpty()
            ? runGit(repo, QStringList{QStringLiteral("show"), QStringLiteral("--format=")} + common + QStringList{hash}, &out, &error)
            : runGit(repo, QStringList{QStringLiteral("diff")} + common + QStringList{parents.first(), hash}, &out, &error);
        if (!ok) {
            set.error = error;
            continue;
        }
        const QVector<FileDiff> diffs = parseDiff(out);
        // Then from the commit to the work tree, for the files it touched,
        // so its lines are where they are now
        QStringList paths;
        for (const FileDiff &d : diffs) {
            if (!d.newPath.isEmpty())
                paths << d.newPath;
        }
        QHash<QString, FileDiff> since;
        if (!paths.isEmpty()) {
            QByteArray forward;
            if (runGit(repo, QStringList{QStringLiteral("diff")} + common + QStringList{hash, QStringLiteral("--")} + paths, &forward)) {
                for (const FileDiff &d : parseDiff(forward))
                    since.insert(d.oldPath.isEmpty() ? d.newPath : d.oldPath, d);
            }
        }
        const QString shortHash = hash.left(9);
        for (const FileDiff &d : diffs) {
            if (d.newPath.isEmpty())
                continue;   // deleted by the commit: nothing of it is left
            QString now = d.newPath;
            const FileDiff *forward = nullptr;
            const auto it = since.constFind(d.newPath);
            if (it != since.cend()) {
                if (it->newPath.isEmpty())
                    continue;   // deleted since
                now = it->newPath;
                forward = &*it;
            }
            QSet<qint32> lines, added;
            qint32 plus = 0, minus = 0;
            touchedLines(d, lines, added, &plus, &minus);
            FileChange &fc = set.files[top + now];
            for (qint32 l : std::as_const(lines)) {
                const qint32 m = forward ? mapLine(forward->hunks, l) : l;
                fc.lines.insert(m);
                if (added.contains(l))
                    fc.addedLines.insert(m);
            }
            fc.added += plus;
            fc.removed += minus;
            fc.newFile = fc.newFile || d.oldPath.isEmpty();
            if (!fc.commits.contains(shortHash))
                fc.commits << shortHash;
        }
    }
    return set;
}

Mapping touchedSymbols(const CodeModel &m, const ChangeSet &changes)
{
    Mapping out;
    QHash<qint32, Touched> bySymbol;
    QHash<qint32, qint32> extentLines;
    for (auto it = changes.files.cbegin(); it != changes.files.cend(); ++it) {
        const qint32 fid = m.fileId(it.key());
        if (fid < 0) {
            out.outside << it.key();
            continue;
        }
        out.lineCount[fid] = qint32(it->lines.size());
        // What is declared on each line here: fields, variables, macros,
        // declarations of functions defined elsewhere
        QHash<qint32, QVector<qint32>> declaredOn;
        for (qint32 s : m.files[fid].symbols) {
            const Symbol &sym = m.symbols[s];
            if (sym.has(FromMacro) || sym.has(Implicit) || sym.kind == SymbolKind::Namespace)
                continue;
            if (sym.decl.file == fid)
                declaredOn[sym.decl.line].append(s);
            if (sym.def.file == fid && sym.def.line != sym.decl.line && !isFunction(sym.kind) && !isType(sym.kind))
                declaredOn[sym.def.line].append(s);
        }
        for (qint32 line : it->lines) {
            QVector<qint32> hit;
            const qint32 inner = m.innermostAt(fid, line);
            // A declaration on the line, unless it is the start of a definition spanning it
            for (qint32 s : declaredOn.value(line)) {
                if (s != inner)
                    hit.append(s);
            }
            if (hit.isEmpty() && inner >= 0)
                hit.append(inner);
            for (qint32 s : std::as_const(hit)) {
                Touched &t = bySymbol[s];
                t.symbol = s;
                t.lines++;
                if (it->addedLines.contains(line))
                    extentLines[s]++;
            }
        }
    }
    for (auto it = bySymbol.begin(); it != bySymbol.end(); ++it) {
        const Symbol &s = m.symbols[it.key()];
        const qint32 span = s.def.valid() && s.def.endLine >= s.def.line ? s.def.endLine - s.def.line + 1 : 1;
        it->added = extentLines.value(it.key()) >= std::max(1, span * 9 / 10);
        out.symbols.append(*it);
    }
    std::sort(out.symbols.begin(), out.symbols.end(), [&](const Touched &a, const Touched &b) {
        const Location x = m.symbols[a.symbol].where(), y = m.symbols[b.symbol].where();
        return std::tie(x.file, x.line) < std::tie(y.file, y.line);
    });
    return out;
}

} // namespace Unravel::History
