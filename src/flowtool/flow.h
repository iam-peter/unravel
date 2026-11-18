#pragma once

#include <QByteArray>
#include <QString>

namespace Unravel::Flow {

QByteArray generate(const QString &projectPath, const QString &sourceFile,
                    int definitionLine, const QString &qualifiedName,
                    QString *error);

} // namespace Unravel::Flow
