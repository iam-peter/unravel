#include "sgutil.h"

#include <QFontMetricsF>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGTextNode>
#include <QSGVertexColorMaterial>
#include <QTextLayout>

#include <cmath>

namespace Sg {

namespace {

// The vertex colour material wants premultiplied colours
QSGGeometry::ColoredPoint2D vertex(const QPointF &p, const QColor &c)
{
    QSGGeometry::ColoredPoint2D v;
    const float a = c.alphaF();
    v.set(float(p.x()), float(p.y()), uchar(c.red() * a), uchar(c.green() * a), uchar(c.blue() * a), uchar(c.alpha()));
    return v;
}

QColor mix(const QColor &a, const QColor &b)
{
    return QColor::fromRgbF((a.redF() + b.redF()) / 2, (a.greenF() + b.greenF()) / 2, (a.blueF() + b.blueF()) / 2,
                            (a.alphaF() + b.alphaF()) / 2);
}

} // namespace

void Mesh::triangle(const QPointF &a, const QPointF &b, const QPointF &c, const QColor &ca, const QColor &cb,
                    const QColor &cc)
{
    v.append(vertex(a, ca));
    v.append(vertex(b, cb));
    v.append(vertex(c, cc));
}

void Mesh::quad(const QPointF &tl, const QPointF &tr, const QPointF &br, const QPointF &bl, const QColor &ctl,
                const QColor &ctr, const QColor &cbr, const QColor &cbl)
{
    triangle(tl, tr, bl, ctl, ctr, cbl);
    triangle(tr, br, bl, ctr, cbr, cbl);
}

void Mesh::rect(const QRectF &r, const QColor &c)
{
    if (r.width() <= 0 || r.height() <= 0)
        return;
    quad(r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft(), c, c, c, c);
}

void Mesh::gradient(const QRectF &r, const QColor &from, const QColor &to)
{
    if (r.width() <= 0 || r.height() <= 0)
        return;
    const QColor m = mix(from, to);
    quad(r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft(), from, m, to, m);
}

void Mesh::frame(const QRectF &r, double w, const QColor &c)
{
    if (r.width() <= 0 || r.height() <= 0)
        return;
    w = std::min({w, r.width() / 2, r.height() / 2});
    rect(QRectF(r.left(), r.top(), r.width(), w), c);
    rect(QRectF(r.left(), r.bottom() - w, r.width(), w), c);
    rect(QRectF(r.left(), r.top() + w, w, r.height() - 2 * w), c);
    rect(QRectF(r.right() - w, r.top() + w, w, r.height() - 2 * w), c);
}

void Mesh::segment(const QPointF &a, const QPointF &b, double halfWidth, const QColor &ca, const QColor &cb)
{
    const QPointF d = b - a;
    const double len = std::hypot(d.x(), d.y());
    if (len < 1e-6)
        return;
    const QPointF n(-d.y() / len * halfWidth, d.x() / len * halfWidth);
    quad(a + n, b + n, b - n, a - n, ca, cb, cb, ca);
}

QSGGeometryNode *Mesh::node() const
{
    if (v.isEmpty())
        return nullptr;
    auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), int(v.size()));
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    std::copy(v.cbegin(), v.cend(), geometry->vertexDataAsColoredPoint2D());
    auto *node = new QSGGeometryNode;
    node->setGeometry(geometry);
    node->setMaterial(new QSGVertexColorMaterial);
    node->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
    return node;
}

QSGTextNode *textNode(QQuickWindow *window, const QColor &color)
{
    QSGTextNode *node = window->createTextNode();
    node->setColor(color);
    node->setRenderType(QSGTextNode::QtRendering);
    return node;
}

void addText(QSGTextNode *node, const QString &text, const QFont &font, const QRectF &box, Qt::Alignment alignment,
             bool elide)
{
    if (text.isEmpty() || box.width() <= 1)
        return;
    const QFontMetricsF fm(font);
    const QString shown = elide ? fm.elidedText(text, Qt::ElideRight, box.width()) : text;
    if (shown.isEmpty())
        return;
    QTextLayout layout(shown, font);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (!line.isValid()) {
        layout.endLayout();
        return;
    }
    line.setLineWidth(1e6);
    layout.endLayout();
    const double w = line.naturalTextWidth(), h = line.height();
    double x = box.left(), y = box.top();
    if (alignment & Qt::AlignRight)
        x = box.right() - w;
    else if (alignment & Qt::AlignHCenter)
        x = box.left() + (box.width() - w) / 2;
    if (alignment & Qt::AlignBottom)
        y = box.bottom() - h;
    else if (alignment & Qt::AlignVCenter)
        y = box.top() + (box.height() - h) / 2;
    node->addTextLayout(QPointF(x, y), &layout);
}

} // namespace Sg
