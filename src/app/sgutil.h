#pragma once

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QSGGeometry>
#include <QVector>

class QQuickWindow;
class QSGGeometryNode;
class QSGTextNode;

// Drawing for the scene graph, so the painted views run on the GPU: meshes
// of coloured triangles (a colour per vertex, so gradients come free) and
// text nodes from the window's glyph cache.
namespace Sg {

struct Mesh {
    QVector<QSGGeometry::ColoredPoint2D> v;

    void triangle(const QPointF &a, const QPointF &b, const QPointF &c, const QColor &ca, const QColor &cb,
                  const QColor &cc);
    // Corners: top left, top right, bottom right, bottom left
    void quad(const QPointF &tl, const QPointF &tr, const QPointF &br, const QPointF &bl, const QColor &ctl,
              const QColor &ctr, const QColor &cbr, const QColor &cbl);
    void rect(const QRectF &r, const QColor &c);
    // From one colour at the top left to another at the bottom right
    void gradient(const QRectF &r, const QColor &from, const QColor &to);
    // The outline inside a rectangle
    void frame(const QRectF &r, double width, const QColor &c);
    // A segment of a line, its colour along it from `ca` to `cb`
    void segment(const QPointF &a, const QPointF &b, double halfWidth, const QColor &ca, const QColor &cb);
    void segment(const QPointF &a, const QPointF &b, double halfWidth, const QColor &c) { segment(a, b, halfWidth, c, c); }
    bool isEmpty() const { return v.isEmpty(); }
    void clear() { v.clear(); }

    // A node that owns its geometry and its material; nullptr when empty
    QSGGeometryNode *node() const;
};

// A text node in a colour, from the window, for addText
QSGTextNode *textNode(QQuickWindow *window, const QColor &color);
// One line of text in a box: aligned, elided to the box's width
void addText(QSGTextNode *node, const QString &text, const QFont &font, const QRectF &box,
             Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter, bool elide = true);

} // namespace Sg
