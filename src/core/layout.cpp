#include "layout.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Unravel::Layout {

namespace {

struct LNode {
    double depth = 0;    // extent across layers (w for left to right)
    double breadth = 0;  // extent along a layer
    int layer = 0;
    double pos = 0;      // top (or left) along the layer
    bool dummy = false;
    QVector<int> preds, succs;
};

int crossings(const QVector<QVector<int>> &layers, const QVector<LNode> &n, const QVector<int> &order)
{
    int total = 0;
    for (int l = 0; l + 1 < layers.size(); ++l) {
        QVector<QPair<int, int>> e;
        for (int v : layers[l]) {
            for (int w : n[v].succs) {
                if (n[w].layer == l + 1)
                    e.append({order[v], order[w]});
            }
        }
        // Crossings are inversions of the targets' order: counted with a
        // Fenwick tree in O(E log V)
        std::sort(e.begin(), e.end());
        int maxSecond = 0;
        for (const auto &p : std::as_const(e))
            maxSecond = std::max(maxSecond, p.second);
        QVector<int> tree(maxSecond + 2, 0);
        int seen = 0;
        for (const auto &p : std::as_const(e)) {
            // Those seen with a larger target cross this one
            int notGreater = 0;
            for (int i = p.second + 1; i > 0; i -= i & -i)
                notGreater += tree[i];
            total += seen - notGreater;
            for (int i = p.second + 1; i < tree.size(); i += i & -i)
                tree[i]++;
            ++seen;
        }
    }
    return total;
}

} // namespace

void layered(QVector<Node> &nodes, QVector<Link> &links, const Options &o)
{
    const int n0 = int(nodes.size());
    if (n0 == 0)
        return;
    const bool lr = o.direction == Direction::LeftToRight;
    QVector<LNode> n(n0);
    for (int i = 0; i < n0; ++i) {
        n[i].depth = lr ? nodes[i].w : nodes[i].h;
        n[i].breadth = lr ? nodes[i].h : nodes[i].w;
    }

    // 1. Break cycles: edges back to a node on the DFS stack are reversed
    QVector<QVector<int>> out(n0);
    for (int e = 0; e < links.size(); ++e) {
        Link &l = links[e];
        l.points.clear();
        l.reversed = false;
        if (l.from == l.to || l.from < 0 || l.to < 0 || l.from >= n0 || l.to >= n0)
            continue;
        out[l.from].append(e);
    }
    {
        QVector<int> state(n0, 0);   // 0 new, 1 on stack, 2 done
        // Start from nodes without incoming edges so the drawing flows from them
        QVector<int> indeg(n0, 0);
        for (const Link &l : links) {
            if (l.from != l.to && l.from >= 0 && l.to >= 0 && l.from < n0 && l.to < n0)
                indeg[l.to]++;
        }
        QVector<int> starts(n0);
        std::iota(starts.begin(), starts.end(), 0);
        std::stable_sort(starts.begin(), starts.end(), [&](int a, int b) { return indeg[a] < indeg[b]; });
        for (int s : std::as_const(starts)) {
            if (state[s])
                continue;
            QVector<QPair<int, int>> stack = {{s, 0}};
            state[s] = 1;
            while (!stack.isEmpty()) {
                auto &[v, i] = stack.last();
                if (i < out[v].size()) {
                    const int e = out[v][i++];
                    const int w = links[e].to;
                    if (state[w] == 1)
                        links[e].reversed = true;
                    else if (state[w] == 0) {
                        state[w] = 1;
                        stack.append({w, 0});
                    }
                } else {
                    state[v] = 2;
                    stack.removeLast();
                }
            }
        }
    }
    QSet<quint64> seenPairs;
    for (const Link &l : std::as_const(links)) {
        if (l.from == l.to || l.from < 0 || l.to < 0 || l.from >= n0 || l.to >= n0)
            continue;
        const int a = l.reversed ? l.to : l.from;
        const int b = l.reversed ? l.from : l.to;
        const quint64 k = (quint64(a) << 32) | quint64(b);
        if (seenPairs.contains(k))
            continue;
        seenPairs.insert(k);
        n[a].succs.append(b);
        n[b].preds.append(a);
    }

    // 2. Layers: longest path from the sources, then sources moved up to
    // just before their first successor
    QVector<int> indeg(n0);
    for (int i = 0; i < n0; ++i)
        indeg[i] = int(n[i].preds.size());
    QVector<int> topo;
    QVector<int> ready;
    for (int i = 0; i < n0; ++i) {
        if (indeg[i] == 0)
            ready.append(i);
    }
    while (!ready.isEmpty()) {
        const int v = ready.takeFirst();
        topo.append(v);
        for (int w : std::as_const(n[v].succs)) {
            n[w].layer = std::max(n[w].layer, n[v].layer + 1);
            if (--indeg[w] == 0)
                ready.append(w);
        }
    }
    for (int i = int(topo.size()) - 1; i >= 0; --i) {
        LNode &v = n[topo[i]];
        if (v.preds.isEmpty() && !v.succs.isEmpty()) {
            int minLayer = std::numeric_limits<int>::max();
            for (int w : std::as_const(v.succs))
                minLayer = std::min(minLayer, n[w].layer);
            v.layer = std::max(0, minLayer - 1);
        }
    }

    // 3. Dummy nodes along edges that span several layers
    struct Chain {
        int link;
        QVector<int> path;   // node ids from (layered) source to target
    };
    QVector<Chain> chains;
    // Rebuild adjacency with dummies
    for (LNode &v : n) {
        v.succs.clear();
        v.preds.clear();
    }
    seenPairs.clear();
    QHash<quint64, int> chainOfPair;
    for (int e = 0; e < links.size(); ++e) {
        const Link &l = links[e];
        if (l.from == l.to || l.from < 0 || l.to < 0 || l.from >= n0 || l.to >= n0)
            continue;
        const int a = l.reversed ? l.to : l.from;
        const int b = l.reversed ? l.from : l.to;
        const quint64 k = (quint64(a) << 32) | quint64(b);
        if (chainOfPair.contains(k)) {
            chains.append({e, chains[chainOfPair.value(k)].path});
            continue;
        }
        Chain c;
        c.link = e;
        c.path.append(a);
        int prev = a;
        for (int layer = n[a].layer + 1; layer < n[b].layer; ++layer) {
            LNode d;
            d.dummy = true;
            d.layer = layer;
            d.breadth = 2;
            d.depth = 0;
            n.append(d);
            const int id = int(n.size() - 1);
            n[prev].succs.append(id);
            n[id].preds.append(prev);
            c.path.append(id);
            prev = id;
        }
        if (n[b].layer <= n[a].layer) {
            // Same layer (can happen after moving sources): connect directly
        }
        n[prev].succs.append(b);
        n[b].preds.append(prev);
        c.path.append(b);
        chainOfPair.insert(k, int(chains.size()));
        chains.append(c);
    }

    // 4. Order within layers: barycentres, sweeping down and up
    int layerCount = 0;
    for (const LNode &v : std::as_const(n))
        layerCount = std::max(layerCount, v.layer + 1);
    QVector<QVector<int>> layers(layerCount);
    for (int v : std::as_const(topo))
        layers[n[v].layer].append(v);
    for (int v = n0; v < n.size(); ++v)
        layers[n[v].layer].append(v);
    QVector<int> order(n.size(), 0);
    auto renumber = [&] {
        for (const QVector<int> &layer : std::as_const(layers)) {
            for (int i = 0; i < layer.size(); ++i)
                order[layer[i]] = i;
        }
    };
    renumber();
    QVector<QVector<int>> best = layers;
    int bestCrossings = crossings(layers, n, order);
    for (int sweep = 0; sweep < o.sweeps && bestCrossings > 0; ++sweep) {
        const bool down = sweep % 2 == 0;
        for (int li = 0; li < layerCount; ++li) {
            const int l = down ? li : layerCount - 1 - li;
            QVector<int> &layer = layers[l];
            QHash<int, double> bary;
            for (int v : std::as_const(layer)) {
                const QVector<int> &adj = down ? n[v].preds : n[v].succs;
                if (adj.isEmpty()) {
                    bary.insert(v, order[v]);
                    continue;
                }
                double sum = 0;
                for (int w : adj)
                    sum += order[w];
                bary.insert(v, sum / adj.size());
            }
            std::stable_sort(layer.begin(), layer.end(), [&](int a, int b) { return bary.value(a) < bary.value(b); });
            for (int i = 0; i < layer.size(); ++i)
                order[layer[i]] = i;
        }
        const int c = crossings(layers, n, order);
        if (c < bestCrossings) {
            bestCrossings = c;
            best = layers;
        }
    }
    layers = best;
    renumber();

    // 5. Positions along the layers: packed, then pulled towards the
    // neighbours a few times, keeping the order and the gaps
    for (const QVector<int> &layer : std::as_const(layers)) {
        double p = 0;
        for (int v : layer) {
            n[v].pos = p;
            p += n[v].breadth + o.nodeGap;
        }
    }
    auto center = [&](int v) { return n[v].pos + n[v].breadth / 2; };
    for (int iter = 0; iter < 16; ++iter) {
        const bool down = iter % 2 == 0;
        for (int li = 0; li < layerCount; ++li) {
            const int l = down ? li : layerCount - 1 - li;
            QVector<int> &layer = layers[l];
            QVector<double> want(layer.size());
            for (int i = 0; i < layer.size(); ++i) {
                const int v = layer[i];
                const QVector<int> &adj1 = down ? n[v].preds : n[v].succs;
                const QVector<int> &adj2 = down ? n[v].succs : n[v].preds;
                const QVector<int> &adj = adj1.isEmpty() ? adj2 : adj1;
                if (adj.isEmpty()) {
                    want[i] = n[v].pos;
                    continue;
                }
                double sum = 0;
                for (int w : adj)
                    sum += center(w);
                want[i] = sum / adj.size() - n[v].breadth / 2;
            }
            // Keep order and gaps: forward and backward passes, averaged
            QVector<double> fwd(layer.size()), bwd(layer.size());
            for (int i = 0; i < layer.size(); ++i) {
                fwd[i] = want[i];
                if (i > 0)
                    fwd[i] = std::max(fwd[i], fwd[i - 1] + n[layer[i - 1]].breadth + o.nodeGap);
            }
            for (int i = int(layer.size()) - 1; i >= 0; --i) {
                bwd[i] = want[i];
                if (i + 1 < layer.size())
                    bwd[i] = std::min(bwd[i], bwd[i + 1] - n[layer[i]].breadth - o.nodeGap);
            }
            for (int i = 0; i < layer.size(); ++i)
                n[layer[i]].pos = (fwd[i] + bwd[i]) / 2;
            // The average can overlap again: one more forward pass
            for (int i = 1; i < layer.size(); ++i) {
                const double minPos = n[layer[i - 1]].pos + n[layer[i - 1]].breadth + o.nodeGap;
                if (n[layer[i]].pos < minPos)
                    n[layer[i]].pos = minPos;
            }
        }
    }

    // Layer offsets
    QVector<double> layerDepth(layerCount, 0), layerStart(layerCount, 0);
    for (const LNode &v : std::as_const(n))
        layerDepth[v.layer] = std::max(layerDepth[v.layer], v.depth);
    for (int l = 1; l < layerCount; ++l)
        layerStart[l] = layerStart[l - 1] + layerDepth[l - 1] + o.layerGap;
    double minPos = std::numeric_limits<double>::max();
    for (const LNode &v : std::as_const(n))
        minPos = std::min(minPos, v.pos);

    auto place = [&](int v, double *x, double *y) {
        const double along = n[v].pos - minPos;
        const double across = layerStart[n[v].layer] + (layerDepth[n[v].layer] - n[v].depth) / 2;
        *x = lr ? across : along;
        *y = lr ? along : across;
    };
    for (int i = 0; i < n0; ++i) {
        place(i, &nodes[i].x, &nodes[i].y);
        nodes[i].layer = n[i].layer;
        nodes[i].order = order[i];
    }

    // 6. Edge routes: out of the source's far side, through the dummies,
    // into the target's near side
    for (const Chain &c : std::as_const(chains)) {
        Link &l = links[c.link];
        QVector<QPointF> pts;
        for (int k = 0; k < c.path.size(); ++k) {
            const int v = c.path[k];
            double x, y;
            place(v, &x, &y);
            if (v < n0) {
                const Node &nd = nodes[v];
                const bool first = k == 0;
                if (lr)
                    pts.append(QPointF(first ? nd.x + nd.w : nd.x, nd.y + nd.h / 2));
                else
                    pts.append(QPointF(nd.x + nd.w / 2, first ? nd.y + nd.h : nd.y));
            } else {
                pts.append(lr ? QPointF(x, y + 1) : QPointF(x + 1, y));
            }
        }
        if (l.reversed)
            std::reverse(pts.begin(), pts.end());
        l.points = pts;
    }
}

namespace {

double worst(const QVector<double> &row, double side)
{
    if (row.isEmpty() || side <= 0)
        return std::numeric_limits<double>::max();
    const double sum = std::accumulate(row.begin(), row.end(), 0.0);
    const double mx = *std::max_element(row.begin(), row.end());
    const double mn = *std::min_element(row.begin(), row.end());
    if (mn <= 0 || sum <= 0)
        return std::numeric_limits<double>::max();
    const double s2 = side * side, sum2 = sum * sum;
    return std::max(s2 * mx / sum2, sum2 / (s2 * mn));
}

} // namespace

QVector<QRectF> squarify(const QVector<double> &values, const QRectF &rect)
{
    QVector<QRectF> out(values.size());
    if (values.isEmpty() || rect.width() <= 0 || rect.height() <= 0)
        return out;
    QVector<int> idx(values.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return values[a] > values[b]; });
    double total = 0;
    for (double v : values)
        total += std::max(0.0, v);
    if (total <= 0)
        return out;
    const double scale = rect.width() * rect.height() / total;
    QRectF free = rect;
    int i = 0;
    while (i < idx.size()) {
        const double side = std::min(free.width(), free.height());
        QVector<double> row;
        QVector<int> rowIdx;
        while (i < idx.size()) {
            const double a = std::max(0.0, values[idx[i]]) * scale;
            if (a <= 0) {
                ++i;
                continue;
            }
            QVector<double> next = row;
            next.append(a);
            if (!row.isEmpty() && worst(next, side) > worst(row, side))
                break;
            row = next;
            rowIdx.append(idx[i]);
            ++i;
        }
        if (row.isEmpty())
            break;
        const double sum = std::accumulate(row.begin(), row.end(), 0.0);
        if (free.width() >= free.height()) {
            const double w = sum / free.height();
            double y = free.top();
            for (int k = 0; k < row.size(); ++k) {
                const double h = row[k] / w;
                out[rowIdx[k]] = QRectF(free.left(), y, w, h);
                y += h;
            }
            free.setLeft(free.left() + w);
        } else {
            const double h = sum / free.width();
            double x = free.left();
            for (int k = 0; k < row.size(); ++k) {
                const double w = row[k] / h;
                out[rowIdx[k]] = QRectF(x, free.top(), w, h);
                x += w;
            }
            free.setTop(free.top() + h);
        }
    }
    return out;
}

} // namespace Unravel::Layout
