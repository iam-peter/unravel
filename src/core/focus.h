#pragma once

#include "history.h"
#include "model.h"

#include <QJsonObject>

namespace Unravel::Focus {

struct Options {
    int limit = 20; // zero means all
    bool changedOnly = false;
    const History::HunkActivity *functionHunks = nullptr;
    int impactDepth = 0;
    QSet<qint32> touched;
    const std::atomic_bool *cancel = nullptr;
};

struct Entry {
    qint32 symbol = -1;
    double complexity = 0, history = 0, dependency = 0, priority = 0;
    qint64 commits = 0;
    qint64 hunkCommits = 0, hunkLines = 0, ambiguousHunks = 0;
    QStringList hunkAmbiguity;
    int neighbours = 0;
    bool touched = false;
};

struct Result {
    QVector<Entry> entries;
    QString error;
    bool historical = false, partial = false, changedOnly = false;
    bool functionHistory = false, firstParentHistory = false;
    qint64 mappingCommits = 0;
    qint64 postRevisionChangedFiles = 0;
    bool followedRenames = false;
    QString revision;
    qint64 since = 0, until = 0;
    int eligible = 0;
    int impactDepth = 0;
};

Result rank(const CodeModel &model, const History::Activity *activity = nullptr, const Options &options = {});
QJsonObject json(const CodeModel &model, const Result &result);
QString markdown(const CodeModel &model, const Result &result);

} // namespace Unravel::Focus
