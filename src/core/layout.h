#pragma once

#include <QPointF>
#include <QRectF>
#include <QVector>

// Graph and area layouts for the views, in plain geometry.
namespace Unravel::Layout {

struct Node {
    double w = 100, h = 30;   // in
    double x = 0, y = 0;      // out: top left
    int layer = 0;            // out
    int order = 0;            // out: position within the layer
};

struct Link {
    int from = 0, to = 0;     // in
    double weight = 1;        // in
    QVector<QPointF> points;  // out: from the source's side to the target's, through bends
    bool reversed = false;    // out: drawn against the layering to break a cycle
};

enum class Direction { LeftToRight, TopToBottom };

struct Options {
    Direction direction = Direction::LeftToRight;
    double layerGap = 120;    // between layers
    double nodeGap = 18;      // between nodes in a layer
    int sweeps = 12;          // crossing reduction passes
};

// A layered (Sugiyama-style) drawing: cycles broken by reversing edges,
// layers by longest path, long edges routed through dummy nodes, crossings
// reduced by barycentres, then nodes pulled towards their neighbours.
void layered(QVector<Node> &nodes, QVector<Link> &links, const Options &options = {});

// Squarified treemap (Bruls, Huizing, van Wijk) of values into a rectangle;
// the rectangles come in the order of the values
QVector<QRectF> squarify(const QVector<double> &values, const QRectF &rect);

} // namespace Unravel::Layout
