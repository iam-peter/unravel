#include "includeimpact.h"

#include <QCoreApplication>
#include <QSet>

#include <algorithm>
#include <tuple>

namespace Unravel::IncludeImpact {

QString Coverage::description() const
{
    if (!available())
        return QCoreApplication::translate("IncludeImpact", "No indexed C/C++ translation units. Python imports are not analyzed here.");
    return QCoreApplication::translate(
               "IncludeImpact",
               "Estimated impact over %n distinct indexed translation-unit file(s). "
               "Coverage is partial: the index merges include facts across configurations, so a recorded chain may not occur in any one configuration. "
               "Paths through external or generated files, precompiled headers, header units, or modules may be missing. "
               "This is not a rebuild-time estimate.", nullptr, indexedUnits);
}

QVector<Step> Reach::witness(int unit) const
{
    QVector<Step> steps;
    if (!std::binary_search(units.cbegin(), units.cend(), unit))
        return steps;
    for (int file = unit; file != header; file = towardHeader[file].to)
        steps.append(towardHeader[file]);
    return steps;
}

Index::Index(const CodeModel &model)
    : m_out(model.files.size()), m_in(model.files.size()),
      m_supported(model.files.size()), m_units(model.files.size())
{
    for (int id = 0; id < model.files.size(); ++id) {
        const File &f = model.files[id];
        // The shared model stores Python imports in File::includes as well.
        m_supported[id] = !f.path.endsWith(QLatin1String(".py"), Qt::CaseInsensitive)
                          && !f.path.endsWith(QLatin1String(".pyi"), Qt::CaseInsensitive)
                          && !f.path.endsWith(QLatin1String(".pyw"), Qt::CaseInsensitive);
        if (!m_supported[id])
            continue;
        m_units[id] = f.indexed;
        m_coverage.indexedUnits += f.indexed;
        if (f.project && !f.generated && !f.system && f.header)
            m_headers.append(id);
    }

    for (int id = 0; id < model.files.size(); ++id) {
        if (!m_supported[id])
            continue;
        QVector<Step> &out = m_out[id];
        for (const Include &inc : model.files[id].includes) {
            if (valid(inc.file))
                out.append({id, inc.file, qMax(0, inc.line)});
        }
        std::sort(out.begin(), out.end(), [](const Step &a, const Step &b) {
            return std::tuple(a.to, a.line == 0, a.line) < std::tuple(b.to, b.line == 0, b.line);
        });
        out.erase(std::unique(out.begin(), out.end(), [](const Step &a, const Step &b) {
                      return a.to == b.to;
                  }), out.end());
        for (const Step &edge : std::as_const(out))
            m_in[edge.to].append(edge);
    }
    for (int id = 0; id < model.files.size(); ++id) {
        if (m_out[id].isEmpty() && m_in[id].isEmpty())
            continue;
        const File &f = model.files[id];
        m_coverage.externalFiles += !f.project || f.system;
        m_coverage.generatedFiles += f.generated;
    }
    std::sort(m_headers.begin(), m_headers.end(), [&model](int a, int b) {
        return model.files[a].path < model.files[b].path;
    });
    buildComponents();
}

void Index::buildComponents()
{
    // Iterative Kosaraju: a long chain should not exhaust the call stack.
    struct Frame { int file; qsizetype next = 0; };
    QVector<Frame> stack;
    QVector<int> order;
    QVector<bool> seen(m_out.size());
    for (int file = 0; file < m_out.size(); ++file) {
        if (seen[file] || !m_supported[file])
            continue;
        seen[file] = true;
        stack.append(Frame{file});
        while (!stack.isEmpty()) {
            Frame &frame = stack.last();
            if (frame.next == m_out[frame.file].size()) {
                order.append(frame.file);
                stack.removeLast();
            } else {
                const int next = m_out[frame.file][frame.next++].to;
                if (!seen[next]) {
                    seen[next] = true;
                    stack.append(Frame{next});
                }
            }
        }
    }
    m_component.fill(-1, m_out.size());
    QVector<int> members;
    for (auto it = order.crbegin(); it != order.crend(); ++it) {
        if (m_component[*it] >= 0)
            continue;
        const int component = m_componentSizes.size();
        members.clear();
        members.append(*it);
        m_component[*it] = component;
        for (qsizetype i = 0; i < members.size(); ++i) {
            for (const Step &edge : m_in[members[i]]) {
                if (m_component[edge.from] < 0) {
                    members.append(edge.from);
                    m_component[edge.from] = component;
                }
            }
        }
        m_componentSizes.append(members.size());
    }
}

bool Index::cyclic(int i) const
{
    return m_componentSizes[m_component[i]] > 1
           || std::any_of(m_out[i].cbegin(), m_out[i].cend(), [i](const Step &edge) { return edge.to == i; });
}

QVector<HeaderRank> Index::ranking(const std::atomic_bool *cancel) const
{
    QVector<HeaderRank> result;
    if (!m_coverage.available())
        return result;

    // When multiple headers form a cycle they share the same reachable units.
    QVector<int> counts(m_componentSizes.size(), -1);
    QVector<int> seen(m_out.size(), -1);
    QVector<int> queue;
    for (const int header : m_headers) {
        if (cancel && cancel->load(std::memory_order_relaxed))
            return {};
        const int component = m_component[header];
        int &count = counts[component];
        if (count < 0) {
            count = 0;
            queue.clear();
            queue.append(header);
            seen[header] = component;
            for (qsizetype i = 0; i < queue.size(); ++i) {
                if (cancel && cancel->load(std::memory_order_relaxed))
                    return {};
                const int file = queue[i];
                count += m_units[file];
                for (const Step &edge : m_in[file]) {
                    if (seen[edge.from] != component) {
                        seen[edge.from] = component;
                        queue.append(edge.from);
                    }
                }
            }
        }
        result.append({header, count - int(m_units[header]), cyclic(header)});
    }
    std::stable_sort(result.begin(), result.end(), [](const HeaderRank &a, const HeaderRank &b) {
        return a.affectedUnits > b.affectedUnits;
    });
    return result;
}

Reach Index::affected(int file) const
{
    Reach result;
    if (!valid(file) || !m_coverage.available())
        return result;
    result.header = file;
    QVector<int> queue{file};
    result.towardHeader.resize(m_out.size());
    for (qsizetype i = 0; i < queue.size(); ++i) {
        for (const Step &edge : m_in[queue[i]]) {
            if (edge.from == file || result.towardHeader[edge.from].to >= 0)
                continue;
            result.towardHeader[edge.from] = edge;
            queue.append(edge.from);
            if (m_units[edge.from])
                result.units.append(edge.from);
        }
    }
    std::sort(result.units.begin(), result.units.end());
    return result;
}

Graph Index::graph(int focus, Direction direction, Limits limits) const
{
    Graph result;
    if (!valid(focus))
        return result;
    if (limits.nodes == 0) {
        result.nodesTruncated = true;
        return result;
    }
    QSet<int> seen{focus};
    QVector<int> depths{0};
    result.files.append(focus);
    const bool reverse = direction == Direction::Includers;
    // Depth is checked after traversal to avoid reporting cross-links or
    // cycles back into the visible subgraph as omitted nodes.
    for (qsizetype i = 0; i < result.files.size(); ++i) {
        const int file = result.files[i];
        const int depth = depths[i];
        if (limits.depth >= 0 && depth >= limits.depth)
            continue;
        for (const Step &edge : reverse ? m_in[file] : m_out[file]) {
            const int next = reverse ? edge.from : edge.to;
            if (seen.contains(next))
                continue;
            if (limits.nodes >= 0 && result.files.size() >= limits.nodes) {
                result.nodesTruncated = true;
                continue;
            }
            seen.insert(next);
            result.files.append(next);
            depths.append(depth + 1);
        }
    }
    for (qsizetype i = 0; i < result.files.size(); ++i) {
        const int file = result.files[i];
        for (const Step &edge : m_out[file]) {
            if (seen.contains(edge.to))
                result.edges.append(edge);
        }
        if (depths[i] == limits.depth) {
            for (const Step &edge : reverse ? m_in[file] : m_out[file])
                result.depthTruncated |= !seen.contains(reverse ? edge.from : edge.to);
        }
    }
    return result;
}

Cycle Index::cycle(int focus) const
{
    if (!valid(focus))
        return {};
    const int component = m_component[focus];
    Cycle result{m_componentSizes[component], {}};
    if (!cyclic(focus))
        return result;

    QVector<int> queue{focus};
    QVector<Step> parent(m_out.size());
    for (qsizetype i = 0; i < queue.size(); ++i) {
        for (const Step &edge : m_out[queue[i]]) {
            if (edge.to == focus) {
                result.witness.append(edge);
                for (int prev = edge.from; prev != focus; prev = parent[prev].from)
                    result.witness.append(parent[prev]);
                std::reverse(result.witness.begin(), result.witness.end());
                return result;
            }
            if (m_component[edge.to] == component && parent[edge.to].from < 0) {
                queue.append(edge.to);
                parent[edge.to] = edge;
            }
        }
    }
    return result;
}

} // namespace Unravel::IncludeImpact
