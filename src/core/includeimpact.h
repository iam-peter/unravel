#pragma once

#include "model.h"

#include <atomic>

namespace Unravel::IncludeImpact {

// Edges always point from the includer to the included file, regardless of
// traversal direction. A nonpositive line means the index has no source line.
struct Step {
    int from = -1;
    int to = -1;
    int line = 0;
    friend bool operator==(const Step &, const Step &) = default;
};

struct Coverage {
    int indexedUnits = 0; // distinct C/C++-family main file IDs, not compile commands
    int externalFiles = 0; // observed endpoints, not a count of missing paths
    int generatedFiles = 0;

    bool available() const { return indexedUnits > 0; }
    // The aggregate index cannot establish complete or per-configuration coverage.
    bool partial() const { return available(); }
    QString description() const;
};

struct HeaderRank {
    int file = -1;
    int affectedUnits = 0;
    bool inCycle = false;
};

struct Reach {
    int header = -1;
    QVector<int> units;
    QVector<Step> towardHeader; // indexed by file ID
    QVector<Step> witness(int unit) const;
};

enum class Direction { Includers, Includes };

struct Limits {
    int depth = 2;  // max hops from focus; -1 is unlimited
    int nodes = 40; // max files including focus; -1 is unlimited
};

struct Graph {
    QVector<int> files; // focus first, remaining files in BFS order
    QVector<Step> edges; // induced edges, including edges between boundary files
    bool depthTruncated = false;
    bool nodesTruncated = false;
};

struct Cycle {
    int componentSize = 0; // size of the strongly connected component
    QVector<Step> witness; // one closed include walk through the focus
};

// Snapshot of File::includes only; never traverses CodeModel::edges. The
// finalized CodeModel is needed only during construction.
class Index
{
public:
    explicit Index(const CodeModel &model);

    const Coverage &coverage() const { return m_coverage; }
    bool supportsFile(int file) const { return valid(file); }
    // Ranks ordinary project headers, including observed zero-impact headers.
    // If cancelled, returns no (potentially misleading) partial rankings.
    QVector<HeaderRank> ranking(const std::atomic_bool *cancel = nullptr) const;
    // Counts a main file once, with one shortest recorded chain to the focus.
    // The focus itself is not counted, even if indexed as a main file.
    Reach affected(int file) const;
    Graph graph(int focus, Direction direction, Limits limits = {}) const;
    Cycle cycle(int focus) const;

private:
    bool valid(int i) const { return i >= 0 && i < m_out.size() && m_supported[i]; }
    bool cyclic(int i) const;
    void buildComponents();

    Coverage m_coverage;
    QVector<QVector<Step>> m_out, m_in;
    QVector<int> m_component, m_componentSizes;
    QVector<bool> m_supported, m_units;
    QVector<int> m_headers; // path order for ties
};

} // namespace Unravel::IncludeImpact
