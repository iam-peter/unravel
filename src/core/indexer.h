#pragma once

#include "model.h"
#include "project.h"

#include <atomic>
#include <functional>

// Builds a CodeModel with libclang: every translation unit of a project
// is parsed by clang (on several threads, each with its own index), and
// what clang's indexer reports — declarations, references, includes,
// diagnostics, plus macros and measurements of function bodies — is
// merged into one model.
namespace Unravel {

struct IndexResult {
    bool ok = false;
    bool cancelled = false;
    qint32 units = 0;
    qint32 failedUnits = 0;
    QString error;
};

class Indexer
{
public:
    using Progress = std::function<void(int done, int total, const QString &file)>;

    // Fills `model` (cleared first) from `setup`. Blocks; `progress` is
    // called from the worker threads. Threads: 0 = one fewer than cores.
    static IndexResult run(const ProjectSetup &setup, CodeModel &model, std::atomic<bool> *cancel = nullptr,
                           const Progress &progress = {}, int threads = 0);

    // Counts lines and lines with code (not blank, not only a comment)
    static void countLines(const QString &path, qint32 *lines, qint32 *codeLines);
};

} // namespace Unravel
