#pragma once

#include <QString>
#include <QStringList>

#include <atomic>

#include "model.h"

class QMutex;

// Python sources into the same code model as C and C++: a script run with
// the system's python3 reads them with the standard library's ast (it
// never imports or runs them) and writes JSON, which is merged here.
namespace Unravel::Python {

// The interpreter to run, or "" when there is none
QString interpreter();

// The Python sources under a folder, without virtual environments, caches
// and build trees
QStringList sourceFiles(const QString &root);

// Indexes `files` into `model` (locking `mutex` while it merges, if given).
// False with `error` set when Python can't be run or fails.
bool run(const QString &root, const QStringList &files, CodeModel &model, QString *error,
         std::atomic<bool> *cancel = nullptr, QMutex *mutex = nullptr);

} // namespace Unravel::Python
