#pragma once

#include "model.h"

#include <QByteArray>
#include <QStringList>
#include <QVector>

namespace Unravel::Architecture {

struct Evidence {
    QString sourceFile;
    QString targetFile;
    QString sourceLayer;
    QString targetLayer;
    qint32 line = 0;
    QString sourceSymbol;
    QString targetSymbol;
    qint32 sourceFileId = -1;
    qint32 targetFileId = -1;
    qint32 sourceSymbolId = -1;
    qint32 targetSymbolId = -1;
    qint32 referenceId = -1;
};

struct Violation {
    QString ruleId;
    QString exceptionId;
    QString scope;
    QString sourceLayer;
    QString targetLayer;
    QString effect;
    QString reason;
    QVector<Evidence> evidence;
};

struct AcceptedException {
    QString exceptionId;
    QString ruleId;
    QString sourceFile;
    QString targetFile;
    QString reason;
};

struct Result {
    bool valid = true;
    bool configured = false;
    QString configPath;
    QStringList errors;
    QStringList unassignedFiles;
    QVector<Violation> violations;
    QVector<AcceptedException> acceptedExceptions;

    // Unlisted dependencies are allowed; an explicit allow rule takes
    // precedence over a forbid rule for the same directed pair and scope.
    QByteArray toJson() const;
    QString toMarkdown() const;
};

Result check(const CodeModel &model, const QString &configPath);
Result check(const CodeModel &model, const QByteArray &configJson);
Result noConfig();

} // namespace Unravel::Architecture
