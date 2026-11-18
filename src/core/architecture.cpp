#include "architecture.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <iterator>
#include <tuple>

namespace Unravel::Architecture {
namespace {

struct Layer {
    QString id;
    QString name;
    QVector<QRegularExpression> paths;
};

struct Rule {
    QString id;
    QString from;
    QString to;
    QString effect;
    QString scope;
};

struct Exception {
    QString id;
    QString ruleId;
    QString source;
    QString target;
    QString reason;
};

struct Config {
    QVector<Layer> layers;
    QVector<Rule> rules;
    QVector<Exception> exceptions;
};

void error(Result &result, const QString &where, const QString &message)
{
    result.errors.append(where + QStringLiteral(": ") + message);
}

bool validId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z][A-Za-z0-9._-]*$"));
    return pattern.match(id).hasMatch();
}

bool validRelativePath(const QString &path, bool allowGlob)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\')))
        return false;
    if (path.contains(QLatin1Char(':')))
        return false;
    const QStringList parts = path.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return false;
        if (!allowGlob && (part.contains(QLatin1Char('*')) || part.contains(QLatin1Char('?'))))
            return false;
        if (part.contains(QLatin1Char('[')) || part.contains(QLatin1Char(']'))
            || part.contains(QLatin1Char('{')) || part.contains(QLatin1Char('}')))
            return false;
    }
    return true;
}

QRegularExpression compileGlob(const QString &glob)
{
    QString pattern = QStringLiteral("^");
    for (qsizetype i = 0; i < glob.size(); ++i) {
        const QChar ch = glob.at(i);
        if (ch == QLatin1Char('*')) {
            const bool recursive = i + 1 < glob.size() && glob.at(i + 1) == QLatin1Char('*');
            if (recursive) {
                ++i;
                if (i + 1 < glob.size() && glob.at(i + 1) == QLatin1Char('/')) {
                    ++i;
                    pattern += QStringLiteral("(?:.*/)?");
                } else {
                    pattern += QStringLiteral(".*");
                }
            } else {
                pattern += QStringLiteral("[^/]*");
            }
        } else if (ch == QLatin1Char('?')) {
            pattern += QStringLiteral("[^/]");
        } else {
            pattern += QRegularExpression::escape(QString(ch));
        }
    }
    pattern += QLatin1Char('$');
    return QRegularExpression(pattern);
}

bool hasOnlyKeys(const QJsonObject &object, const QStringList &allowed,
                Result &result, const QString &where)
{
    bool ok = true;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowed.contains(it.key())) {
            error(result, where, QStringLiteral("unknown field '%1'").arg(it.key()));
            ok = false;
        }
    }
    return ok;
}

QString requiredString(const QJsonObject &object, const QString &key, Result &result,
                       const QString &where)
{
    const QJsonValue value = object.value(key);
    if (!value.isString() || value.toString().trimmed().isEmpty()) {
        error(result, where, QStringLiteral("'%1' must be a non-empty string").arg(key));
        return {};
    }
    return value.toString();
}

bool parseConfig(const QByteArray &json, Result &result, Config &config)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error(result, QStringLiteral("$"),
              parseError.error == QJsonParseError::NoError
                  ? QStringLiteral("top-level JSON value must be an object")
                  : QStringLiteral("invalid JSON at byte %1: %2")
                        .arg(parseError.offset).arg(parseError.errorString()));
        return false;
    }
    const QJsonObject root = document.object();
    hasOnlyKeys(root, {QStringLiteral("version"), QStringLiteral("layers"),
                       QStringLiteral("rules"), QStringLiteral("exceptions")}, result,
                QStringLiteral("$"));
    if (!root.value(QStringLiteral("version")).isDouble()
        || root.value(QStringLiteral("version")).toInt(-1) != 1)
        error(result, QStringLiteral("$.version"), QStringLiteral("expected integer 1"));
    if (!root.value(QStringLiteral("layers")).isArray())
        error(result, QStringLiteral("$.layers"), QStringLiteral("must be an array"));
    if (!root.value(QStringLiteral("rules")).isArray())
        error(result, QStringLiteral("$.rules"), QStringLiteral("must be an array"));
    if (root.contains(QStringLiteral("exceptions")) && !root.value(QStringLiteral("exceptions")).isArray())
        error(result, QStringLiteral("$.exceptions"), QStringLiteral("must be an array"));

    QSet<QString> layerIds;
    const QJsonArray layerArray = root.value(QStringLiteral("layers")).toArray();
    for (qsizetype i = 0; i < layerArray.size(); ++i) {
        const QString where = QStringLiteral("$.layers[%1]").arg(i);
        if (!layerArray.at(i).isObject()) {
            error(result, where, QStringLiteral("must be an object"));
            continue;
        }
        const QJsonObject object = layerArray.at(i).toObject();
        hasOnlyKeys(object, {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("paths")},
                    result, where);
        Layer layer;
        layer.id = requiredString(object, QStringLiteral("id"), result, where);
        layer.name = requiredString(object, QStringLiteral("name"), result, where);
        if (!validId(layer.id))
            error(result, where, QStringLiteral("'id' must start with a letter and contain only letters, digits, '.', '_' or '-'"));
        else if (layerIds.contains(layer.id))
            error(result, where, QStringLiteral("duplicate layer id '%1'").arg(layer.id));
        layerIds.insert(layer.id);

        if (!object.value(QStringLiteral("paths")).isArray()
            || object.value(QStringLiteral("paths")).toArray().isEmpty()) {
            error(result, where, QStringLiteral("'paths' must be a non-empty array"));
        } else {
            const QJsonArray paths = object.value(QStringLiteral("paths")).toArray();
            for (qsizetype p = 0; p < paths.size(); ++p) {
                if (!paths.at(p).isString()) {
                    error(result, where + QStringLiteral(".paths[%1]").arg(p),
                          QStringLiteral("must be a string"));
                    continue;
                }
                const QString glob = paths.at(p).toString();
                if (!validRelativePath(glob, true)) {
                    error(result, where + QStringLiteral(".paths[%1]").arg(p),
                          QStringLiteral("must be a root-relative glob without '.', '..', empty components, brackets or backslashes"));
                    continue;
                }
                layer.paths.append(compileGlob(glob));
            }
        }
        config.layers.append(layer);
    }

    QSet<QString> ruleIds;
    const QJsonArray ruleArray = root.value(QStringLiteral("rules")).toArray();
    for (qsizetype i = 0; i < ruleArray.size(); ++i) {
        const QString where = QStringLiteral("$.rules[%1]").arg(i);
        if (!ruleArray.at(i).isObject()) {
            error(result, where, QStringLiteral("must be an object"));
            continue;
        }
        const QJsonObject object = ruleArray.at(i).toObject();
        hasOnlyKeys(object, {QStringLiteral("id"), QStringLiteral("from"), QStringLiteral("to"),
                             QStringLiteral("effect"), QStringLiteral("scope")}, result, where);
        Rule rule;
        rule.id = requiredString(object, QStringLiteral("id"), result, where);
        rule.from = requiredString(object, QStringLiteral("from"), result, where);
        rule.to = requiredString(object, QStringLiteral("to"), result, where);
        rule.effect = requiredString(object, QStringLiteral("effect"), result, where);
        rule.scope = requiredString(object, QStringLiteral("scope"), result, where);
        if (!validId(rule.id))
            error(result, where, QStringLiteral("'id' must be a stable identifier"));
        else if (ruleIds.contains(rule.id))
            error(result, where, QStringLiteral("duplicate rule id '%1'").arg(rule.id));
        ruleIds.insert(rule.id);
        if (!layerIds.contains(rule.from))
            error(result, where, QStringLiteral("unknown source layer '%1'").arg(rule.from));
        if (!layerIds.contains(rule.to))
            error(result, where, QStringLiteral("unknown target layer '%1'").arg(rule.to));
        if (rule.effect != QLatin1String("allow") && rule.effect != QLatin1String("forbid"))
            error(result, where, QStringLiteral("'effect' must be 'allow' or 'forbid'"));
        if (rule.scope != QLatin1String("semantic") && rule.scope != QLatin1String("includes"))
            error(result, where, QStringLiteral("'scope' must be 'semantic' or 'includes'"));
        config.rules.append(rule);
    }

    QSet<QString> exceptionIds;
    const QJsonArray exceptionArray = root.value(QStringLiteral("exceptions")).toArray();
    for (qsizetype i = 0; i < exceptionArray.size(); ++i) {
        const QString where = QStringLiteral("$.exceptions[%1]").arg(i);
        if (!exceptionArray.at(i).isObject()) {
            error(result, where, QStringLiteral("must be an object"));
            continue;
        }
        const QJsonObject object = exceptionArray.at(i).toObject();
        hasOnlyKeys(object, {QStringLiteral("id"), QStringLiteral("rule"),
                             QStringLiteral("source"), QStringLiteral("target"),
                             QStringLiteral("reason")}, result, where);
        Exception exception;
        exception.id = requiredString(object, QStringLiteral("id"), result, where);
        exception.ruleId = requiredString(object, QStringLiteral("rule"), result, where);
        exception.source = requiredString(object, QStringLiteral("source"), result, where);
        exception.target = requiredString(object, QStringLiteral("target"), result, where);
        exception.reason = requiredString(object, QStringLiteral("reason"), result, where);
        if (!validId(exception.id))
            error(result, where, QStringLiteral("'id' must be a stable identifier"));
        else if (exceptionIds.contains(exception.id))
            error(result, where, QStringLiteral("duplicate exception id '%1'").arg(exception.id));
        exceptionIds.insert(exception.id);
        if (!ruleIds.contains(exception.ruleId))
            error(result, where, QStringLiteral("unknown rule '%1'").arg(exception.ruleId));
        else {
            const auto rule = std::find_if(config.rules.cbegin(), config.rules.cend(),
                [&exception](const Rule &candidate) { return candidate.id == exception.ruleId; });
            if (rule != config.rules.cend() && rule->effect != QLatin1String("forbid"))
                error(result, where, QStringLiteral("exceptions may reference only forbid rules"));
        }
        if (!validRelativePath(exception.source, false))
            error(result, where, QStringLiteral("'source' must be an exact root-relative file path"));
        if (!validRelativePath(exception.target, false))
            error(result, where, QStringLiteral("'target' must be an exact root-relative file path"));
        config.exceptions.append(exception);
    }

    result.valid = result.errors.isEmpty();
    return result.valid;
}

QString exceptionFor(const Config &config, const QString &ruleId, const QString &source,
                     const QString &target, QString *reason)
{
    for (const Exception &item : config.exceptions) {
        if (item.ruleId == ruleId && item.source == source && item.target == target) {
            *reason = item.reason;
            return item.id;
        }
    }
    return {};
}

QJsonObject evidenceJson(const Evidence &evidence)
{
    QJsonObject object;
    object.insert(QStringLiteral("sourceFile"), evidence.sourceFile);
    object.insert(QStringLiteral("targetFile"), evidence.targetFile);
    object.insert(QStringLiteral("sourceLayer"), evidence.sourceLayer);
    object.insert(QStringLiteral("targetLayer"), evidence.targetLayer);
    object.insert(QStringLiteral("line"), evidence.line);
    object.insert(QStringLiteral("sourceSymbol"), evidence.sourceSymbol);
    object.insert(QStringLiteral("targetSymbol"), evidence.targetSymbol);
    object.insert(QStringLiteral("sourceFileId"), evidence.sourceFileId);
    object.insert(QStringLiteral("targetFileId"), evidence.targetFileId);
    object.insert(QStringLiteral("sourceSymbolId"), evidence.sourceSymbolId);
    object.insert(QStringLiteral("targetSymbolId"), evidence.targetSymbolId);
    object.insert(QStringLiteral("referenceId"), evidence.referenceId);
    return object;
}

} // namespace

QByteArray Result::toJson() const
{
    QJsonObject root;
    root.insert(QStringLiteral("valid"), valid);
    root.insert(QStringLiteral("configured"), configured);
    root.insert(QStringLiteral("configPath"), configPath);
    root.insert(QStringLiteral("defaultSemantics"), QStringLiteral("allow"));
    root.insert(QStringLiteral("allowPrecedence"),
                QStringLiteral("allow overrides forbid for the same directed pair and scope"));
    QJsonArray errorArray;
    for (const QString &item : errors)
        errorArray.append(item);
    root.insert(QStringLiteral("errors"), errorArray);
    QJsonArray unassigned;
    for (const QString &path : unassignedFiles)
        unassigned.append(path);
    root.insert(QStringLiteral("unassignedFiles"), unassigned);
    QJsonArray violationArray;
    for (const Violation &violation : violations) {
        QJsonObject item;
        item.insert(QStringLiteral("ruleId"), violation.ruleId);
        item.insert(QStringLiteral("exceptionId"), violation.exceptionId);
        item.insert(QStringLiteral("scope"), violation.scope);
        item.insert(QStringLiteral("sourceLayer"), violation.sourceLayer);
        item.insert(QStringLiteral("targetLayer"), violation.targetLayer);
        item.insert(QStringLiteral("effect"), violation.effect);
        item.insert(QStringLiteral("reason"), violation.reason);
        QJsonArray evidenceArray;
        for (const Evidence &evidence : violation.evidence)
            evidenceArray.append(evidenceJson(evidence));
        item.insert(QStringLiteral("evidence"), evidenceArray);
        violationArray.append(item);
    }
    root.insert(QStringLiteral("violations"), violationArray);
    QJsonArray exceptionArray;
    for (const AcceptedException &exception : acceptedExceptions) {
        QJsonObject item;
        item.insert(QStringLiteral("exceptionId"), exception.exceptionId);
        item.insert(QStringLiteral("ruleId"), exception.ruleId);
        item.insert(QStringLiteral("sourceFile"), exception.sourceFile);
        item.insert(QStringLiteral("targetFile"), exception.targetFile);
        item.insert(QStringLiteral("reason"), exception.reason);
        exceptionArray.append(item);
    }
    root.insert(QStringLiteral("acceptedExceptions"), exceptionArray);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

QString Result::toMarkdown() const
{
    QString out = QStringLiteral("# Architecture contracts\n\n");
    out += QStringLiteral("- Configuration: %1\n- Status: %2\n- Default dependency policy: allow\n"
                          "- An explicit allow overrides a forbid for the same directed pair and scope.\n")
               .arg(configured ? (configPath.isEmpty() ? QStringLiteral("provided JSON") : configPath)
                               : QStringLiteral("not configured"),
                    valid ? QStringLiteral("valid") : QStringLiteral("invalid"));
    if (!valid) {
        out += QStringLiteral("\n## Configuration errors\n");
        for (const QString &item : errors)
            out += QStringLiteral("- %1\n").arg(item);
        return out;
    }
    out += QStringLiteral("\n## Violations\n");
    if (violations.isEmpty())
        out += QStringLiteral("\nNo forbidden dependencies found.\n");
    for (const Violation &violation : violations) {
        out += QStringLiteral("\n### %1 — %2 → %3 (%4)\n")
                   .arg(violation.ruleId, violation.sourceLayer, violation.targetLayer,
                        violation.scope);
        if (!violation.exceptionId.isEmpty())
            out += QStringLiteral("Exception `%1`: %2\n").arg(violation.exceptionId, violation.reason);
        for (const Evidence &evidence : violation.evidence) {
            out += QStringLiteral("- `%1:%2` → `%3`")
                       .arg(evidence.sourceFile).arg(evidence.line).arg(evidence.targetFile);
            if (!evidence.sourceSymbol.isEmpty() || !evidence.targetSymbol.isEmpty())
                out += QStringLiteral(" (%1 → %2)").arg(evidence.sourceSymbol, evidence.targetSymbol);
            out += QLatin1Char('\n');
        }
    }
    out += QStringLiteral("\n## Accepted exceptions\n");
    if (acceptedExceptions.isEmpty())
        out += QStringLiteral("\nNone.\n");
    for (const AcceptedException &exception : acceptedExceptions)
        out += QStringLiteral("- `%1` for `%2`: `%3` → `%4` — %5\n")
                   .arg(exception.exceptionId, exception.ruleId, exception.sourceFile,
                        exception.targetFile, exception.reason);
    out += QStringLiteral("\n## Unassigned project files\n");
    if (unassignedFiles.isEmpty())
        out += QStringLiteral("\nNone.\n");
    for (const QString &path : unassignedFiles)
        out += QStringLiteral("- `%1`\n").arg(path);
    return out;
}

Result check(const CodeModel &model, const QString &configPath)
{
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        Result result;
        result.configured = true;
        result.configPath = configPath;
        result.valid = false;
        error(result, QStringLiteral("$"), QStringLiteral("cannot read configuration '%1': %2")
                  .arg(configPath, file.errorString()));
        return result;
    }
    Result result = check(model, file.readAll());
    result.configPath = configPath;
    return result;
}

Result check(const CodeModel &model, const QByteArray &configJson)
{
    Result result;
    result.configured = true;
    Config config;
    if (!parseConfig(configJson, result, config))
        return result;

    QVector<QString> fileLayers(model.files.size());
    for (qint32 fileId = 0; fileId < model.files.size(); ++fileId) {
        const File &file = model.files[fileId];
        if (!file.project)
            continue;
        QVector<int> assignments;
        for (int layerId = 0; layerId < config.layers.size(); ++layerId) {
            for (const QRegularExpression &glob : config.layers[layerId].paths) {
                if (glob.match(QDir::fromNativeSeparators(file.relative)).hasMatch()) {
                    assignments.append(layerId);
                    break;
                }
            }
        }
        if (assignments.size() > 1) {
            error(result, QStringLiteral("file '%1'").arg(file.relative),
                  QStringLiteral("matches multiple layers: %1")
                      .arg([&] {
                          QStringList names;
                          for (int layerId : assignments)
                              names.append(config.layers[layerId].id);
                          return names.join(QStringLiteral(", "));
                      }()));
        } else if (assignments.isEmpty()) {
            result.unassignedFiles.append(QDir::fromNativeSeparators(file.relative));
        } else {
            fileLayers[fileId] = config.layers[assignments.first()].id;
        }
    }
    if (!result.errors.isEmpty()) {
        result.valid = false;
        result.violations.clear();
        return result;
    }
    std::sort(result.unassignedFiles.begin(), result.unassignedFiles.end());

    auto addEvidence = [&](const QString &scope, qint32 sourceId, qint32 targetId, qint32 line,
                           const QString &sourceSymbol, const QString &targetSymbol,
                           qint32 sourceSymbolId = -1, qint32 targetSymbolId = -1,
                           qint32 referenceId = -1) {
        if (sourceId < 0 || sourceId >= fileLayers.size() || targetId < 0 || targetId >= fileLayers.size())
            return;
        const QString fromLayer = fileLayers[sourceId];
        const QString toLayer = fileLayers[targetId];
        if (fromLayer.isEmpty() || toLayer.isEmpty())
            return;
        const File &source = model.files[sourceId];
        const File &target = model.files[targetId];
        const QString sourcePath = QDir::fromNativeSeparators(source.relative);
        const QString targetPath = QDir::fromNativeSeparators(target.relative);
        for (const Rule &rule : config.rules) {
            if (rule.scope != scope || rule.from != fromLayer || rule.to != toLayer
                || rule.effect != QLatin1String("forbid"))
                continue;
            const bool explicitlyAllowed = std::any_of(config.rules.cbegin(), config.rules.cend(),
                [&rule](const Rule &candidate) {
                    return candidate.scope == rule.scope && candidate.from == rule.from
                        && candidate.to == rule.to && candidate.effect == QLatin1String("allow");
                });
            if (explicitlyAllowed)
                continue;
            QString reason;
            const QString exceptionId = exceptionFor(config, rule.id, sourcePath, targetPath, &reason);
            if (!exceptionId.isEmpty()) {
                const bool recorded = std::any_of(result.acceptedExceptions.cbegin(),
                    result.acceptedExceptions.cend(), [&exceptionId, &sourcePath, &targetPath](const AcceptedException &item) {
                        return item.exceptionId == exceptionId && item.sourceFile == sourcePath
                            && item.targetFile == targetPath;
                    });
                if (!recorded)
                    result.acceptedExceptions.append({exceptionId, rule.id, sourcePath, targetPath, reason});
                continue;
            }
            auto violation = std::find_if(result.violations.begin(), result.violations.end(),
                [&rule, &exceptionId, &sourcePath, &targetPath](const Violation &item) {
                    return item.ruleId == rule.id && item.exceptionId == exceptionId
                        && !item.evidence.isEmpty()
                        && item.evidence.first().sourceFile == sourcePath
                        && item.evidence.first().targetFile == targetPath;
                });
            if (violation == result.violations.end()) {
                Violation item;
                item.ruleId = rule.id;
                item.exceptionId = exceptionId;
                item.scope = scope;
                item.sourceLayer = fromLayer;
                item.targetLayer = toLayer;
                item.effect = rule.effect;
                item.reason = reason;
                result.violations.append(item);
                violation = std::prev(result.violations.end());
            }
            Evidence evidence;
            evidence.sourceFile = sourcePath;
            evidence.targetFile = targetPath;
            evidence.sourceLayer = fromLayer;
            evidence.targetLayer = toLayer;
            evidence.line = line;
            evidence.sourceSymbol = sourceSymbol;
            evidence.targetSymbol = targetSymbol;
            evidence.sourceFileId = sourceId;
            evidence.targetFileId = targetId;
            evidence.sourceSymbolId = sourceSymbolId;
            evidence.targetSymbolId = targetSymbolId;
            evidence.referenceId = referenceId;
            violation->evidence.append(evidence);
        }
    };

    for (qint32 sourceId = 0; sourceId < model.files.size(); ++sourceId) {
        const File &source = model.files[sourceId];
        if (!source.project)
            continue;
        for (const Include &include : source.includes) {
            if (include.file < 0 || include.file >= model.files.size())
                continue;
            addEvidence(QStringLiteral("includes"), sourceId, include.file, include.line, {}, {});
        }
    }
    for (qint32 refId = 0; refId < model.refs.size(); ++refId) {
        const Reference &reference = model.refs[refId];
        if (reference.kind == RefKind::Declaration || reference.kind == RefKind::Include
            || reference.to < 0 || reference.to >= model.symbols.size())
            continue;
        const qint32 sourceId = reference.loc.file;
        const qint32 targetId = model.symbols[reference.to].where().file;
        if (sourceId < 0 || sourceId >= model.files.size() || targetId < 0 || targetId >= model.files.size())
            continue;
        if (!model.files[sourceId].project || !model.files[targetId].project)
            continue;
        QString sourceName;
        qint32 sourceSymbolId = -1;
        if (reference.from >= 0 && reference.from < model.symbols.size()) {
            sourceName = model.displayName(reference.from);
            sourceSymbolId = reference.from;
        }
        const QString targetName = model.displayName(reference.to);
        addEvidence(QStringLiteral("semantic"), sourceId, targetId, reference.loc.line,
                    sourceName, targetName, sourceSymbolId, reference.to, refId);
    }

    std::sort(result.violations.begin(), result.violations.end(),
              [](const Violation &a, const Violation &b) {
                  return std::tie(a.ruleId, a.exceptionId, a.sourceLayer, a.targetLayer)
                      < std::tie(b.ruleId, b.exceptionId, b.sourceLayer, b.targetLayer);
              });
    for (Violation &violation : result.violations) {
        std::sort(violation.evidence.begin(), violation.evidence.end(),
                  [](const Evidence &a, const Evidence &b) {
                      return std::tie(a.sourceFile, a.targetFile, a.line, a.sourceSymbol, a.targetSymbol)
                          < std::tie(b.sourceFile, b.targetFile, b.line, b.sourceSymbol, b.targetSymbol);
                  });
    }
    std::sort(result.acceptedExceptions.begin(), result.acceptedExceptions.end(),
              [](const AcceptedException &a, const AcceptedException &b) {
                  return std::tie(a.exceptionId, a.sourceFile, a.targetFile)
                      < std::tie(b.exceptionId, b.sourceFile, b.targetFile);
              });
    return result;
}

Result noConfig()
{
    Result result;
    return result;
}

} // namespace Unravel::Architecture
