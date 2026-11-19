#include "focus.h"

#include <QDir>
#include <QJsonArray>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace Unravel::Focus {

Result rank(const CodeModel &model, const History::Activity *activity, const Options &options)
{
    Result result;
    result.changedOnly = options.changedOnly;
    result.impactDepth = options.changedOnly ? options.impactDepth : 0;
    if (options.limit < 0 || options.impactDepth < 0 || options.impactDepth > 12) {
        result.error = QStringLiteral("Review Focus requires a nonnegative limit and impact depth between 0 and 12");
        return result;
    }
    QSet<qint32> eligible = options.touched;
    QSet<qint32> frontier = eligible;
    for (int depth = 0; depth < result.impactDepth && !frontier.isEmpty(); ++depth) {
        QSet<qint32> next;
        for (qint32 symbol : frontier) {
            for (qint32 edge : model.inEdges.value(symbol)) {
                if (options.cancel && options.cancel->load()) {
                    result.error = QStringLiteral("Review Focus cancelled");
                    return result;
                }
                const int dependent = model.edges[edge].from;
                if (!eligible.contains(dependent)) {
                    eligible.insert(dependent);
                    next.insert(dependent);
                    if (eligible.size() > 10000) {
                        result.error = QStringLiteral("Potential dependency reach exceeds 10,000 symbols; reduce depth");
                        return result;
                    }
                }
            }
        }
        frontier = std::move(next);
    }
    result.functionHistory = options.functionHunks;
    result.historical = (activity && activity->ready()) || options.functionHunks;
    if (result.historical) {
        if (activity && activity->ready()) {
            result.partial = activity->shallow;
            result.followedRenames = activity->followedRenames;
            result.revision = activity->revision;
            result.since = activity->since;
            result.until = activity->until;
        } else if (options.functionHunks) {
            result.partial = options.functionHunks->shallow;
            result.revision = options.functionHunks->revision;
            result.since = options.functionHunks->since;
            result.until = options.functionHunks->until;
        }
        result.firstParentHistory = options.functionHunks && options.functionHunks->firstParentOnly;
        result.mappingCommits = options.functionHunks ? options.functionHunks->mappingCommits : 0;
        result.postRevisionChangedFiles = options.functionHunks
            ? options.functionHunks->postRevisionChangedFiles : 0;
    }
    if (options.functionHunks && !options.functionHunks->ready()) {
        result.error = options.functionHunks->error.isEmpty()
            ? QStringLiteral("Function hunk history is unavailable") : options.functionHunks->error;
        return result;
    }
    const QString historyRepository = options.functionHunks ? options.functionHunks->repository
        : activity ? activity->repository : QString();
    QVector<qint64> fileCommits(model.files.size(), -1);
    for (qint32 id = 0; id < model.symbols.size(); ++id) {
        if (options.cancel && options.cancel->load()) {
            result.entries.clear();
            result.error = QStringLiteral("Review Focus cancelled");
            return result;
        }
        const auto &symbol = model.symbols[id];
        if (!isFunction(symbol.kind) || !symbol.def.valid() || symbol.has(External) || symbol.has(Generated)
            || symbol.has(Implicit) || symbol.has(FromMacro) || !model.files[symbol.def.file].project
            || model.files[symbol.def.file].generated || (options.changedOnly && !eligible.contains(id)))
            continue;
        Entry entry;
        entry.symbol = id;
        entry.touched = options.touched.contains(id);
        entry.complexity = std::max({1.0, symbol.metrics.complexity / 15.0, symbol.metrics.cognitive / 25.0});
        QSet<qint32> neighbours;
        for (qint32 e : model.inEdges.value(id))
            neighbours.insert(model.edges[e].from);
        for (qint32 e : model.outEdges.value(id))
            neighbours.insert(model.edges[e].to);
        neighbours.remove(id);
        entry.neighbours = int(neighbours.size());
        entry.dependency = std::log2(1.0 + entry.neighbours);
        if (result.historical) {
            const qint32 file = symbol.def.file;
            if (fileCommits[file] < 0) {
                const QString path = History::relativeToRepository(historyRepository, model.files[file].path);
                if (QDir::isAbsolutePath(path) || path == QLatin1String("..") || path.startsWith(QLatin1String("../"))) {
                    result.error = QStringLiteral("Some functions are outside the history repository; use static priority");
                    result.entries.clear();
                    return result;
                }
                fileCommits[file] = activity && activity->ready() ? activity->files.value(path).commits : 0;
            }
            entry.commits = fileCommits[file];
            if (options.functionHunks) {
                const History::FunctionHunkStats stats = options.functionHunks->functions.value(id);
                entry.hunkCommits = stats.commits;
                entry.hunkLines = stats.lines;
                entry.ambiguousHunks = stats.ambiguousCommits;
                for (const QString &reason : stats.ambiguity)
                    entry.hunkAmbiguity.append(reason);
                entry.hunkAmbiguity.sort();
                entry.history = std::log2(1.0 + entry.hunkCommits);
            } else {
                entry.history = std::log2(1.0 + entry.commits);
            }
        }
        entry.priority = entry.complexity * (1.0 + entry.history) * (1.0 + 0.25 * entry.dependency);
        result.entries.append(entry);
    }
    const auto before = [&](const Entry &a, const Entry &b) {
        if (a.priority != b.priority)
            return a.priority > b.priority;
        const auto &x = model.symbols[a.symbol], &y = model.symbols[b.symbol];
        if (x.qualified != y.qualified)
            return x.qualified < y.qualified;
        const QString &xf = model.files[x.def.file].path, &yf = model.files[y.def.file].path;
        if (xf != yf)
            return xf < yf;
        if (x.def.line != y.def.line)
            return x.def.line < y.def.line;
        if (x.def.col != y.def.col)
            return x.def.col < y.def.col;
        if (x.usr != y.usr)
            return x.usr < y.usr;
        return a.symbol < b.symbol;
    };
    result.eligible = int(result.entries.size());
    if (options.limit > 0 && result.entries.size() > options.limit) {
        std::partial_sort(result.entries.begin(), result.entries.begin() + options.limit, result.entries.end(), before);
        result.entries.resize(options.limit);
    } else {
        std::sort(result.entries.begin(), result.entries.end(), before);
    }
    return result;
}

QJsonObject json(const CodeModel &model, const Result &result)
{
    QJsonArray entries;
    for (const Entry &entry : result.entries) {
        const auto &symbol = model.symbols[entry.symbol];
        entries.append(QJsonObject{{QStringLiteral("usr"), symbol.usr}, {QStringLiteral("function"), model.displayName(entry.symbol)},
            {QStringLiteral("file"), model.files[symbol.def.file].relative}, {QStringLiteral("line"), symbol.def.line},
            {QStringLiteral("priority"), entry.priority}, {QStringLiteral("complexityFactor"), entry.complexity},
            {QStringLiteral("historyFactor"), entry.history}, {QStringLiteral("dependencyFactor"), entry.dependency},
            {QStringLiteral("distinctNeighbours"), entry.neighbours}, {QStringLiteral("fileCommits"), entry.commits},
            {QStringLiteral("touched"), entry.touched},
            {QStringLiteral("historyAttribution"), result.functionHistory
                    ? QStringLiteral("first-parent non-merge function-hunk estimate; side-branch and merge commits are not counted; merge diffs only map lines")
                    : result.historical ? (result.followedRenames
                        ? QStringLiteral("file-level Git --follow heuristic proxy; all reachable non-merge commits")
                        : QStringLiteral("file-level pathname proxy; all reachable non-merge commits"))
                                        : QStringLiteral("static priority")},
            {QStringLiteral("functionHunkCommits"), entry.hunkCommits},
            {QStringLiteral("functionHunkLines"), entry.hunkLines},
            {QStringLiteral("ambiguousHunkCommits"), entry.ambiguousHunks},
            {QStringLiteral("hunkAmbiguity"), QJsonArray::fromStringList(entry.hunkAmbiguity)}});
    }
    return {{QStringLiteral("heuristicVersion"), 1}, {QStringLiteral("error"), result.error},
        {QStringLiteral("scope"), result.impactDepth > 0 ? QStringLiteral("touched-plus-potential-dependents")
            : result.changedOnly ? QStringLiteral("touched-functions") : QStringLiteral("project")},
        {QStringLiteral("potentialImpactDepth"), result.impactDepth},
        {QStringLiteral("mode"), result.functionHistory ? QStringLiteral("historical-function-hunk-estimate")
            : result.historical ? QStringLiteral("historical-file-proxy") : QStringLiteral("static")},
        {QStringLiteral("coverage"), result.partial ? QStringLiteral("partial-shallow") : QStringLiteral("indexed-project")},
        {QStringLiteral("firstParentOnly"), result.firstParentHistory},
        {QStringLiteral("lineMappingCommits"), result.mappingCommits},
        {QStringLiteral("historySemantics"), result.functionHistory
            ? QStringLiteral("Function hunk counts include only non-merge commits on the first-parent chain; side-branch and merge commits are not counted (first-parent merge diffs only map lines). The file-level proxy counts all reachable non-merge commits.")
            : result.historical ? QStringLiteral("File-level proxy counts all reachable non-merge commits; working-tree changes are not counted.")
                                : QStringLiteral("Static priority; no Git history used.")},
        {QStringLiteral("postRevisionChangedFiles"), result.postRevisionChangedFiles},
        {QStringLiteral("postRevisionPolicy"), result.functionHistory
            ? QStringLiteral("Changes from the selected revision to the current working tree are used only to map lines; they are not counted as historical commits.")
            : QString()},
        {QStringLiteral("revision"), result.revision}, {QStringLiteral("since"), result.since},
        {QStringLiteral("until"), result.until}, {QStringLiteral("eligible"), result.eligible},
        {QStringLiteral("functions"), entries}};
}

QString markdown(const CodeModel &model, const Result &result)
{
    QString text;
    QTextStream out(&text);
    out << "# Review Focus\n\nHeuristic v1; "
        << (result.changedOnly ? "touched functions" : "whole project") << "; "
        << (result.functionHistory
            ? "first-parent non-merge function-hunk estimate (side-branch and merge commits are not counted; first-parent merge diffs only map lines; file proxy counts all reachable non-merge commits)"
            : result.historical ? (result.followedRenames ? "file-level Git --follow heuristic proxy over all reachable non-merge commits"
                                                          : "file-level pathname proxy over all reachable non-merge commits") : "static priority")
        << (result.firstParentHistory ? "; first-parent history only" : "")
        << (result.partial ? "; partial shallow history" : "") << ". Not a defect probability.\n\n";
    if (result.functionHistory) {
        out << "The line-mapping span traverses " << result.mappingCommits
            << " first-parent commits (merge diffs only map lines). Changes from the selected revision to the current working tree are used only for line mapping, not counted as historical commits";
        if (result.postRevisionChangedFiles > 0)
            out << " (" << result.postRevisionChangedFiles << " changed files)";
        out << ".\n\n";
    } else if (result.historical) {
        out << "Working-tree changes are not included in file commit counts.\n\n";
    }
    if (!result.error.isEmpty()) {
        out << "Error: " << result.error << "\n";
        return text;
    }
    if (result.historical)
        out << "Revision: " << result.revision << "; UTC epoch window: " << result.since << " to " << result.until << ".\n\n";
    if (result.impactDepth > 0)
        out << "Includes potential dependency reach to depth " << result.impactDepth
            << " (static dependencies, not proven execution impact).\n\n";
    out << "Showing " << result.entries.size() << " of " << result.eligible << " eligible functions.\n\n";
    for (const Entry &entry : result.entries) {
        QString name = model.displayName(entry.symbol);
        name.replace(QLatin1Char('`'), QLatin1Char('\''));
        out << "- `" << name << "` at " << model.locationText(model.symbols[entry.symbol].def)
            << " - priority " << QString::number(entry.priority, 'f', 2)
            << "; C=" << entry.complexity << ", H=" << entry.history << ", D=" << entry.dependency;
        if (result.functionHistory)
            out << "; function hunk commits=" << entry.hunkCommits << ", mapped lines=" << entry.hunkLines
                << ", ambiguous commits=" << entry.ambiguousHunks;
        else if (result.historical)
            out << "; file commits=" << entry.commits;
        if (!entry.hunkAmbiguity.isEmpty())
            out << " (" << entry.hunkAmbiguity.join(QStringLiteral(", ")) << ")";
        if (result.changedOnly)
            out << (entry.touched ? "; touched" : "; potential dependent");
        out << "\n";
    }
    return text;
}

} // namespace Unravel::Focus
