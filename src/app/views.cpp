#include "views.h"

#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPathStroker>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGTextNode>
#include <QSGTransformNode>
#include <QSGOpacityNode>
#include <QtMath>

#include <algorithm>
#include <cmath>

#include "layout.h"
#include "session.h"
#include "sgutil.h"

using namespace Unravel;

namespace {

QColor colorOf(const QVariantMap &colors, const QString &key, const QColor &fallback = QColor(128, 128, 128))
{
    const QVariant v = colors.value(key);
    if (!v.isValid())
        return fallback;
    const QColor c = v.value<QColor>();
    return c.isValid() ? c : QColor(v.toString());
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t), float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

QColor withAlpha(QColor c, double a)
{
    c.setAlphaF(float(std::clamp(a, 0.0, 1.0)));
    return c;
}

QString kindKey(int kind)
{
    switch (RefKind(kind)) {
    case RefKind::Call: return QStringLiteral("call");
    case RefKind::TypeUse: return QStringLiteral("type");
    case RefKind::VarUse: return QStringLiteral("var");
    case RefKind::Inherit: return QStringLiteral("inherit");
    case RefKind::Override: return QStringLiteral("override");
    case RefKind::MacroUse: return QStringLiteral("macro");
    case RefKind::Include: return QStringLiteral("include");
    default: return QStringLiteral("type");
    }
}

} // namespace

// --- EdgeLayer -----------------------------------------------------------------------

namespace {

using Vertex = QSGGeometry::Point2D;

// Lines as triangles: two per segment, the width in scene units
void addSegment(QVector<Vertex> &v, const QPointF &a, const QPointF &b, double halfWidth)
{
    const QPointF d = b - a;
    const double len = std::hypot(d.x(), d.y());
    if (len < 1e-6)
        return;
    const QPointF n(-d.y() / len * halfWidth, d.x() / len * halfWidth);
    const QPointF p1 = a + n, p2 = a - n, p3 = b + n, p4 = b - n;
    v.append({float(p1.x()), float(p1.y())});
    v.append({float(p2.x()), float(p2.y())});
    v.append({float(p3.x()), float(p3.y())});
    v.append({float(p3.x()), float(p3.y())});
    v.append({float(p2.x()), float(p2.y())});
    v.append({float(p4.x()), float(p4.y())});
}

void addTriangle(QVector<Vertex> &v, const QPointF &a, const QPointF &b, const QPointF &c)
{
    v.append({float(a.x()), float(a.y())});
    v.append({float(b.x()), float(b.y())});
    v.append({float(c.x()), float(c.y())});
}

// The polyline without its last `cut` units, where the arrow sits
QPolygonF shortened(const QPolygonF &poly, double cut)
{
    QPolygonF out = poly;
    while (out.size() >= 2 && cut > 0) {
        const QPointF a = out[out.size() - 2], b = out.last();
        const double len = std::hypot(b.x() - a.x(), b.y() - a.y());
        if (len > cut) {
            out.last() = b + (a - b) * (cut / len);
            break;
        }
        cut -= len;
        out.removeLast();
    }
    return out;
}

void addPolyline(QVector<Vertex> &v, const QPolygonF &poly, double halfWidth, bool dashed)
{
    if (!dashed) {
        for (qsizetype i = 1; i < poly.size(); ++i)
            addSegment(v, poly[i - 1], poly[i], halfWidth);
        return;
    }
    // 6 on, 4 off, carried over the bends
    const double on = 6, off = 4;
    double phase = 0;
    for (qsizetype i = 1; i < poly.size(); ++i) {
        const QPointF a = poly[i - 1], b = poly[i];
        const double len = std::hypot(b.x() - a.x(), b.y() - a.y());
        double t = 0;
        while (t < len) {
            const bool drawing = phase < on;
            const double left = drawing ? on - phase : on + off - phase;
            const double step = std::min(left, len - t);
            if (drawing)
                addSegment(v, a + (b - a) * (t / len), a + (b - a) * ((t + step) / len), halfWidth);
            t += step;
            phase += step;
            if (phase >= on + off)
                phase = 0;
        }
    }
}

// One edge: its line and its arrow
void addEdge(QVector<Vertex> &v, const GraphScene::SceneEdge &e, double width)
{
    if (e.poly.size() < 2)
        return;
    const bool inherit = e.kind == int(RefKind::Inherit);
    const double size = inherit ? 12 : 7 + width;
    addPolyline(v, shortened(e.poly, size * 0.9), width / 2, e.dashed);
    QPointF d = e.tip - e.tipFrom;
    const double len = std::hypot(d.x(), d.y());
    if (len < 1e-6)
        return;
    d /= len;
    const QPointF n(-d.y(), d.x());
    const QPointF base = e.tip - d * size;
    const QPointF l = base + n * size * 0.5, r = base - n * size * 0.5;
    if (inherit) {
        // Hollow, as UML draws it
        addSegment(v, e.tip, l, 0.7);
        addSegment(v, l, r, 0.7);
        addSegment(v, r, e.tip, 0.7);
    } else {
        addTriangle(v, e.tip, l, r);
    }
}

QSGGeometryNode *triangles(const QVector<Vertex> &v, const QColor &color)
{
    auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), int(v.size()));
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    std::copy(v.cbegin(), v.cend(), geometry->vertexDataAsPoint2D());
    auto *material = new QSGFlatColorMaterial;
    material->setColor(color);
    auto *node = new QSGGeometryNode;
    node->setGeometry(geometry);
    node->setMaterial(material);
    node->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
    return node;
}

double edgeWidth(int count)
{
    return std::clamp(1.1 + std::log2(double(std::max(1, count))) * 0.6, 1.1, 5.0);
}

} // namespace

EdgeLayer::EdgeLayer(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void EdgeLayer::rebuildAll()
{
    m_baseDirty = true;
    m_overlayDirty = true;
    update();
}

void EdgeLayer::setScene(GraphScene *s)
{
    if (s == m_scene)
        return;
    if (m_scene)
        disconnect(m_scene, nullptr, this, nullptr);
    m_scene = s;
    if (m_scene) {
        connect(m_scene, &GraphScene::changed, this, &EdgeLayer::rebuildAll);
        connect(m_scene, &GraphScene::pathsChanged, this, &EdgeLayer::rebuildAll);
        connect(m_scene, &GraphScene::highlightChanged, this, [this] {
            m_overlayDirty = true;
            update();
        });
    }
    emit sceneChanged();
    rebuildAll();
}

void EdgeLayer::setZoom(qreal z)
{
    if (qFuzzyCompare(z, m_zoom))
        return;
    m_zoom = z;
    // Thin lines are widened to a pixel at the zoom they were made for:
    // made again only when the zoom has moved far from it
    if (std::max(z / m_builtZoom, m_builtZoom / z) > 1.6)
        rebuildAll();
    emit viewChanged();
    update();
}

void EdgeLayer::setPanX(qreal v)
{
    if (qFuzzyCompare(v, m_panX))
        return;
    m_panX = v;
    emit viewChanged();
    update();
}

void EdgeLayer::setPanY(qreal v)
{
    if (qFuzzyCompare(v, m_panY))
        return;
    m_panY = v;
    emit viewChanged();
    update();
}

void EdgeLayer::setColors(const QVariantMap &c)
{
    m_colors = c;
    emit viewChanged();
    rebuildAll();
}

void EdgeLayer::setHoveredEdge(int e)
{
    if (e == m_hoveredEdge)
        return;
    m_hoveredEdge = e;
    m_overlayDirty = true;
    emit viewChanged();
    update();
}

QColor EdgeLayer::colorFor(int kind, bool cyclic) const
{
    if (cyclic)
        return colorOf(m_colors, QStringLiteral("cycle"), QColor(220, 60, 60));
    return colorOf(m_colors, kindKey(kind));
}

QSGNode *EdgeLayer::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *root = static_cast<QSGTransformNode *>(old);
    if (!root) {
        root = new QSGTransformNode;
        m_base = new QSGNode;
        m_overlay = new QSGNode;
        root->appendChildNode(m_base);
        root->appendChildNode(m_overlay);
        m_baseNodes.clear();
        m_baseDirty = m_overlayDirty = true;
    }
    QMatrix4x4 matrix;
    matrix.translate(float(m_panX), float(m_panY));
    matrix.scale(float(m_zoom));
    root->setMatrix(matrix);
    if (!m_scene)
        return root;
    const auto &edges = m_scene->edges();

    if (m_baseDirty) {
        m_baseDirty = false;
        m_builtZoom = m_zoom;
        m_base->removeAllChildNodes();
        for (auto &[node, color] : m_baseNodes)
            delete node;
        m_baseNodes.clear();
        // One buffer per colour
        const double minWidth = 1.0 / std::max(0.05, m_zoom);
        QHash<QRgb, QVector<Vertex>> batches;
        for (const GraphScene::SceneEdge &e : edges) {
            const QColor c = colorFor(e.kind, e.cyclic);
            addEdge(batches[c.rgba()], e, std::max(edgeWidth(e.count), minWidth));
        }
        for (auto it = batches.cbegin(); it != batches.cend(); ++it) {
            if (it->isEmpty())
                continue;
            const QColor c = QColor::fromRgba(it.key());
            QSGGeometryNode *node = triangles(*it, c);
            m_base->appendChildNode(node);
            m_baseNodes.append({node, c});
        }
        m_overlayDirty = true;
    }

    if (m_overlayDirty) {
        m_overlayDirty = false;
        // The rest dimmed while something is highlighted
        const bool focus = m_scene->hasHighlight() || m_hoveredEdge >= 0;
        for (auto &[node, color] : m_baseNodes) {
            auto *material = static_cast<QSGFlatColorMaterial *>(node->material());
            const QColor want = focus ? withAlpha(color, color.alphaF() * 0.13) : color;
            if (material->color() != want) {
                material->setColor(want);
                node->markDirty(QSGNode::DirtyMaterial);
            }
        }
        while (QSGNode *child = m_overlay->firstChild()) {
            m_overlay->removeChildNode(child);
            delete child;
        }
        if (focus) {
            const double minWidth = 1.0 / std::max(0.05, m_zoom);
            QHash<QRgb, QVector<Vertex>> batches;
            for (int i = 0; i < edges.size(); ++i) {
                if (!(m_scene->edgeHighlighted(i) || i == m_hoveredEdge))
                    continue;
                const GraphScene::SceneEdge &e = edges[i];
                const double width = std::max(edgeWidth(e.count), minWidth) + 0.8 + (i == m_hoveredEdge ? 1 : 0);
                addEdge(batches[colorFor(e.kind, e.cyclic).rgba()], e, width);
            }
            for (auto it = batches.cbegin(); it != batches.cend(); ++it) {
                if (!it->isEmpty())
                    m_overlay->appendChildNode(triangles(*it, QColor::fromRgba(it.key())));
            }
        }
    }
    return root;
}

int EdgeLayer::edgeAt(qreal x, qreal y) const
{
    if (!m_scene)
        return -1;
    const QPointF p((x - m_panX) / m_zoom, (y - m_panY) / m_zoom);
    const double tol = 6 / m_zoom;
    const auto &edges = m_scene->edges();
    for (int i = int(edges.size()) - 1; i >= 0; --i) {
        const GraphScene::SceneEdge &e = edges[i];
        if (e.poly.size() < 2 || !e.hitBounds.adjusted(-tol, -tol, tol, tol).contains(p))
            continue;
        for (qsizetype k = 0; k + 1 < e.poly.size(); ++k) {
            const QPointF a = e.poly[k], b = e.poly[k + 1];
            const QPointF ab = b - a;
            const double len2 = QPointF::dotProduct(ab, ab);
            const double t = len2 > 0 ? std::clamp(QPointF::dotProduct(p - a, ab) / len2, 0.0, 1.0) : 0.0;
            const QPointF d = p - (a + ab * t);
            if (QPointF::dotProduct(d, d) <= tol * tol)
                return i;
        }
    }
    return -1;
}

// --- BundleView ------------------------------------------------------------------------

BundleView::BundleView(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptHoverEvents(false);
    // A hidden view (another tab) waits until it is shown
    connect(Session::instance(), &Session::modelChanged, this, [this] {
        if (isVisible())
            rebuild();
        else
            m_dirty = true;
    });
    connect(Session::instance(), &Session::foldedChanged, this, [this] {
        if (isVisible())
            rebuild();
        else
            m_dirty = true;
    });
    rebuild();
}

void BundleView::setBundling(qreal b)
{
    b = std::clamp(b, 0.0, 1.0);
    if (qFuzzyCompare(b, m_beta))
        return;
    m_beta = b;
    m_cacheValid = false;
    m_pathsValid = false;
    emit optionsChanged();
    update();
}

void BundleView::setLeafLevel(int l)
{
    if (l == m_leafLevel)
        return;
    m_leafLevel = l;
    emit optionsChanged();
    rebuild();
}

void BundleView::setColorByKind(bool v)
{
    if (v == m_colorByKind)
        return;
    m_colorByKind = v;
    m_cacheValid = false;
    m_pathsValid = false;
    emit optionsChanged();
    update();
}

QVariantList BundleView::kinds() const
{
    QVariantList out;
    for (int k : m_kinds)
        out << k;
    return out;
}

void BundleView::setKinds(const QVariantList &k)
{
    QSet<int> s;
    for (const QVariant &v : k)
        s.insert(v.toInt());
    if (s == m_kinds)
        return;
    m_kinds = s;
    emit optionsChanged();
    rebuild();
}

void BundleView::setColors(const QVariantMap &c)
{
    m_colors = c;
    m_cacheValid = false;
    m_pathsValid = false;
    emit optionsChanged();
    update();
}

void BundleView::setActiveSymbol(int s)
{
    if (s == m_activeSymbol)
        return;
    m_activeSymbol = s;
    emit optionsChanged();
    update();
}

void BundleView::setZoom(qreal z)
{
    z = std::clamp(z, 0.4, 4.0);
    if (qFuzzyCompare(z, m_zoom))
        return;
    m_zoom = z;
    m_cacheValid = false;
    m_pathsValid = false;
    emit optionsChanged();
    update();
}

void BundleView::geometryChange(const QRectF &n, const QRectF &o)
{
    QQuickItem::geometryChange(n, o);
    m_cacheValid = false;
    m_pathsValid = false;
}

void BundleView::rebuild()
{
    m_model = Session::instance()->model();
    m_nodes.clear();
    m_leaves.clear();
    m_links.clear();
    m_leafLinks.clear();
    m_chains.clear();
    m_hoverLinked.clear();
    m_hovered = -1;
    m_cacheValid = false;
    m_pathsValid = false;
    if (!m_model) {
        emit rebuilt();
        update();
        return;
    }
    const CodeModel &m = *m_model;
    m_nodes.append(HNode{QStringLiteral("root")});
    QHash<QString, int> folderNode;
    folderNode.insert(QString(), 0);
    auto folder = [&](const QString &path) -> int {
        if (folderNode.contains(path))
            return folderNode.value(path);
        const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
        const int parent = slash < 0 ? 0 : -1;
        int p = parent;
        if (slash >= 0) {
            // Make the parent first (recursively)
            std::function<int(const QString &)> make = [&](const QString &pp) -> int {
                if (folderNode.contains(pp))
                    return folderNode.value(pp);
                const qsizetype s2 = pp.lastIndexOf(QLatin1Char('/'));
                const int par = s2 < 0 ? 0 : make(pp.left(s2));
                HNode h;
                h.name = s2 < 0 ? pp : pp.mid(s2 + 1);
                h.parent = par;
                h.depth = m_nodes[par].depth + 1;
                m_nodes.append(h);
                const int id = int(m_nodes.size() - 1);
                m_nodes[par].children.append(id);
                folderNode.insert(pp, id);
                return id;
            };
            p = make(path.left(slash));
        }
        HNode h;
        h.name = slash < 0 ? path : path.mid(slash + 1);
        h.parent = p;
        h.depth = m_nodes[p].depth + 1;
        m_nodes.append(h);
        const int id = int(m_nodes.size() - 1);
        m_nodes[p].children.append(id);
        folderNode.insert(path, id);
        return id;
    };
    // Files, sorted by path, then their nodes
    QVector<qint32> files;
    for (qint32 f = 0; f < m.files.size(); ++f) {
        if (m.files[f].project && !m.files[f].generated)
            files.append(f);
    }
    std::sort(files.begin(), files.end(), [&](qint32 a, qint32 b) { return m.files[a].relative < m.files[b].relative; });
    QHash<qint32, int> leafOfSymbol, leafOfFile;
    const QStringList folded = Session::instance()->foldedFolders();
    QHash<QString, int> foldLeaf;
    for (qint32 f : std::as_const(files)) {
        const File &file = m.files[f];
        // A folded folder: one leaf for all that is in it
        const QString fold = Analysis::foldedFolderOf(file.module, folded);
        if (!fold.isEmpty()) {
            auto it = foldLeaf.find(fold);
            if (it == foldLeaf.end()) {
                const qsizetype slash = fold.lastIndexOf(QLatin1Char('/'));
                const int dir = folder(slash < 0 ? QString() : fold.left(slash));
                HNode h;
                h.name = (slash < 0 ? fold : fold.mid(slash + 1)) + QStringLiteral("/");
                h.parent = dir;
                h.depth = m_nodes[dir].depth + 1;
                h.folder = fold;
                h.leaf = int(m_leaves.size());
                m_nodes.append(h);
                const int id = int(m_nodes.size() - 1);
                m_nodes[dir].children.append(id);
                m_leaves.append(id);
                it = foldLeaf.insert(fold, h.leaf);
            }
            leafOfFile.insert(f, *it);
            for (qint32 s : file.symbols)
                leafOfSymbol.insert(m.nodeOwner(s), *it);
            continue;
        }
        QVector<qint32> syms;
        for (qint32 s : file.symbols) {
            const Symbol &sym = m.symbols[s];
            if (m.nodeOwner(s) != s || sym.has(External) || sym.has(FromMacro) || sym.has(Implicit))
                continue;
            if (sym.kind == SymbolKind::Namespace || sym.kind == SymbolKind::Macro || sym.kind == SymbolKind::Typedef)
                continue;
            if (!sym.has(Defined) && !isType(sym.kind))
                continue;
            // A function declared in a header and defined in a source: one leaf, at the definition
            if (isFunction(sym.kind) && sym.def.valid() && sym.def.file != f)
                continue;
            syms.append(s);
        }
        if (syms.isEmpty() && m_leafLevel == 0)
            continue;
        const int dir = folder(file.module);
        HNode fh;
        fh.name = QFileInfo(file.path).fileName();
        fh.parent = dir;
        fh.depth = m_nodes[dir].depth + 1;
        fh.file = f;
        m_nodes.append(fh);
        const int fileNode = int(m_nodes.size() - 1);
        m_nodes[dir].children.append(fileNode);
        if (m_leafLevel == 1) {
            m_nodes[fileNode].leaf = int(m_leaves.size());
            m_leaves.append(fileNode);
            leafOfFile.insert(f, m_nodes[fileNode].leaf);
            continue;
        }
        std::sort(syms.begin(), syms.end(), [&](qint32 a, qint32 b) { return m.symbols[a].where().line < m.symbols[b].where().line; });
        for (qint32 s : std::as_const(syms)) {
            HNode h;
            h.name = m.symbols[s].name;
            h.parent = fileNode;
            h.depth = m_nodes[fileNode].depth + 1;
            h.symbol = s;
            h.file = f;
            h.leaf = int(m_leaves.size());
            m_nodes.append(h);
            const int id = int(m_nodes.size() - 1);
            m_nodes[fileNode].children.append(id);
            m_leaves.append(id);
            leafOfSymbol.insert(s, h.leaf);
        }
    }
    // Links between leaves
    QHash<quint64, int> linkIds;
    auto add = [&](int a, int b, int kind, int count) {
        if (a < 0 || b < 0 || a == b)
            return;
        const quint64 k = (quint64(quint32(a)) << 32) | quint32(b);
        auto it = linkIds.find(k);
        if (it == linkIds.end()) {
            m_links.append({a, b, 0, kind});
            it = linkIds.insert(k, int(m_links.size() - 1));
        }
        BLink &l = m_links[*it];
        if (count > l.count / 2)
            l.kind = kind;
        l.count += count;
    };
    auto leafFor = [&](qint32 s) -> int {
        if (m_leafLevel == 1) {
            const Symbol &sym = m.symbols[s];
            const qint32 f = sym.def.valid() ? sym.def.file : sym.decl.file;
            return leafOfFile.value(f, -1);
        }
        const qint32 owner = m.nodeOwner(s);
        return leafOfSymbol.value(owner, -1);
    };
    for (const Edge &e : m.edges) {
        if (e.kind == RefKind::Declaration || e.kind == RefKind::Include)
            continue;
        if (!m_kinds.isEmpty() && !m_kinds.contains(int(e.kind)))
            continue;
        if ((e.flags & RefImplicit) && e.kind == RefKind::Call)
            continue;
        int a = -1;
        if (m_leafLevel == 1) {
            const Reference &r = m.refs[e.firstRef];
            a = leafOfFile.value(r.loc.file, -1);
        } else {
            a = leafFor(e.from);
        }
        add(a, leafFor(e.to), int(e.kind), e.count);
    }
    if (m_leafLevel == 1 && (m_kinds.isEmpty() || m_kinds.contains(int(RefKind::Include)))) {
        for (qint32 f : std::as_const(files)) {
            for (const Include &inc : m.files[f].includes)
                add(leafOfFile.value(f, -1), leafOfFile.value(inc.file, -1), int(RefKind::Include), 1);
        }
    }
    m_leafLinks = QVector<QVector<int>>(m_leaves.size());
    for (int i = 0; i < m_links.size(); ++i) {
        m_leafLinks[m_links[i].a].append(i);
        m_leafLinks[m_links[i].b].append(i);
    }
    // Collapse folder chains with one child (a/b/c/ with only c in b)
    m_maxDepth = 1;
    for (const HNode &h : std::as_const(m_nodes))
        m_maxDepth = std::max(m_maxDepth, h.depth);
    layout();
    emit rebuilt();
    update();
}

void BundleView::layout()
{
    // Leaves on the circle in tree order, with gaps between top-level groups
    const int n = int(m_leaves.size());
    if (n == 0)
        return;
    QVector<int> order;
    QVector<double> gapsBefore;
    std::function<void(int, int)> walk = [&](int node, int depth) {
        const HNode &h = m_nodes[node];
        if (h.leaf >= 0) {
            order.append(node);
            return;
        }
        for (int c : h.children) {
            const int before = int(order.size());
            walk(c, depth + 1);
            Q_UNUSED(before)
        }
    };
    walk(0, 0);
    // A gap after each file's leaves (and a bigger one after each top folder)
    QVector<double> weight(order.size(), 1.0);
    for (int i = 1; i < order.size(); ++i) {
        const HNode &a = m_nodes[order[i - 1]], &b = m_nodes[order[i]];
        if (a.parent != b.parent)
            weight[i] += 1.0;
        // Different top-level folder
        auto top = [&](int node) {
            int cur = node;
            while (m_nodes[cur].parent > 0)
                cur = m_nodes[cur].parent;
            return cur;
        };
        if (top(order[i - 1]) != top(order[i]))
            weight[i] += 2.0;
    }
    double total = 0;
    for (double w : std::as_const(weight))
        total += w;
    total += 3;   // and before the first
    double acc = 1.5;
    for (int i = 0; i < order.size(); ++i) {
        acc += weight[i];
        m_nodes[order[i]].angle = (acc - 0.5) / total * 2 * M_PI - M_PI / 2;
    }
    // Inner nodes: their children's mean angle, and the span they cover
    std::function<void(int)> inner = [&](int node) {
        HNode &h = m_nodes[node];
        if (h.leaf >= 0) {
            h.angleFrom = h.angleTo = h.angle;
            return;
        }
        double sum = 0;
        double from = 1e9, to = -1e9;
        for (int c : std::as_const(h.children)) {
            inner(c);
            sum += m_nodes[c].angle;
            from = std::min(from, m_nodes[c].angleFrom);
            to = std::max(to, m_nodes[c].angleTo);
        }
        if (!h.children.isEmpty())
            h.angle = sum / h.children.size();
        h.angleFrom = from;
        h.angleTo = to;
    };
    inner(0);
    m_leafAngles.clear();
}

QPointF BundleView::nodePoint(int node) const
{
    const double size = std::min(width(), height()) * m_zoom;
    const double R = size / 2 - std::max(90.0, size * 0.16);
    const QPointF c(width() / 2, height() / 2);
    const HNode &h = m_nodes[node];
    if (node == 0)
        return c;
    const double r = h.leaf >= 0 ? R : R * double(h.depth) / (m_maxDepth + 0.5);
    return c + QPointF(std::cos(h.angle) * r, std::sin(h.angle) * r);
}

void BundleView::ensurePaths()
{
    // The curves, made again only when the geometry or the bundling changes
    if (!m_pathsValid || m_paths.size() != m_links.size()) {
        if (m_chains.size() != m_links.size()) {
            // Leaf a up to the common ancestor and down to leaf b
            m_chains.resize(m_links.size());
            for (int li = 0; li < m_links.size(); ++li) {
                const BLink &l = m_links[li];
                const int na = m_leaves[l.a], nb = m_leaves[l.b];
                QVector<int> up, down;
                QSet<int> ancestorsA;
                for (int x = na; x >= 0; x = m_nodes[x].parent)
                    ancestorsA.insert(x);
                int lca = nb;
                while (lca >= 0 && !ancestorsA.contains(lca))
                    lca = m_nodes[lca].parent;
                for (int x = na; x != lca && x >= 0; x = m_nodes[x].parent)
                    up.append(x);
                for (int x = nb; x != lca && x >= 0; x = m_nodes[x].parent)
                    down.prepend(x);
                QVector<int> chain = up;
                if (lca > 0 || (up.size() + down.size()) <= 2)
                    chain.append(std::max(0, lca));
                chain += down;
                m_chains[li] = chain;
            }
        }
        m_paths.resize(m_links.size());
        m_ends.resize(m_links.size());
        for (int li = 0; li < m_links.size(); ++li) {
            // Straightened by beta, as a uniform cubic B-spline
            QVector<QPointF> cp;
            for (int x : std::as_const(m_chains[li]))
                cp.append(nodePoint(x));
            const int N = int(cp.size());
            QPainterPath path;
            if (N >= 2) {
                const QPointF p0 = cp.first(), pn = cp.last();
                for (int i = 0; i < N; ++i) {
                    const double t = double(i) / (N - 1);
                    cp[i] = cp[i] * m_beta + (p0 + (pn - p0) * t) * (1 - m_beta);
                }
                QVector<QPointF> P;
                P << cp.first() << cp.first();
                P += cp;
                P << cp.last() << cp.last();
                path.moveTo(cp.first());
                for (int i = 0; i + 3 < P.size(); ++i) {
                    const QPointF b1 = (2 * P[i + 1] + P[i + 2]) / 3;
                    const QPointF b2 = (P[i + 1] + 2 * P[i + 2]) / 3;
                    const QPointF b3 = (P[i + 1] + 4 * P[i + 2] + P[i + 3]) / 6;
                    path.cubicTo(b1, b2, b3);
                }
                m_ends[li] = {cp.first(), cp.last()};
            }
            m_paths[li] = path;
        }
        m_pathsValid = true;
    }
}

QSGNode *BundleView::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    QSGNode *root = old;
    if (!root) {
        root = new QSGNode;
        m_sgFade = new QSGOpacityNode;
        m_sgOverlay = new QSGNode;
        root->appendChildNode(m_sgFade);
        root->appendChildNode(m_sgOverlay);
        m_cacheValid = false;
    }
    auto clearNode = [](QSGNode *n) {
        while (QSGNode *c = n->firstChild()) {
            n->removeChildNode(c);
            delete c;
        }
    };
    if (!m_model || m_leaves.isEmpty()) {
        clearNode(m_sgFade);
        clearNode(m_sgOverlay);
        return root;
    }
    const QColor text = colorOf(m_colors, QStringLiteral("text"), Qt::black);
    const QColor muted = colorOf(m_colors, QStringLiteral("textMuted"), Qt::gray);
    const QColor source = colorOf(m_colors, QStringLiteral("source"), QColor(60, 170, 90));
    const QColor target = colorOf(m_colors, QStringLiteral("target"), QColor(220, 70, 60));
    const QColor accent = colorOf(m_colors, QStringLiteral("accent"), QColor(40, 110, 220));
    ensurePaths();
    int maxCount = 1;
    for (const BLink &l : std::as_const(m_links))
        maxCount = std::max(maxCount, l.count);
    // A link: its curve flattened, coloured from the user to the used
    auto addLink = [&](Sg::Mesh &mesh, int li, double alphaScale, bool emphasis) {
        const BLink &l = m_links[li];
        const QPolygonF poly = m_paths[li].toSubpathPolygons().value(0);
        if (poly.size() < 2)
            return;
        const double w = 0.8 + 2.2 * std::log2(1.0 + l.count) / std::log2(1.0 + maxCount) + (emphasis ? 0.8 : 0);
        const double alpha = std::clamp((0.22 + 0.5 * double(l.count) / maxCount) * alphaScale, 0.04, 1.0);
        const QColor kind = withAlpha(colorOf(m_colors, kindKey(l.kind)), alpha);
        const QColor from = withAlpha(source, alpha), to = withAlpha(target, alpha);
        for (qsizetype i = 1; i < poly.size(); ++i) {
            if (m_colorByKind) {
                mesh.segment(poly[i - 1], poly[i], w / 2, kind);
            } else {
                const double t0 = double(i - 1) / (poly.size() - 1), t1 = double(i) / (poly.size() - 1);
                mesh.segment(poly[i - 1], poly[i], w / 2, mix(from, to, t0), mix(from, to, t1));
            }
        }
    };
    const double size = std::min(width(), height()) * m_zoom;
    const double R = size / 2 - std::max(90.0, size * 0.16);
    const QPointF c(width() / 2, height() / 2);
    const int n = int(m_leaves.size());
    const double perLeaf = 2 * M_PI * R / std::max(1, n);
    const bool labels = perLeaf >= 5.5;
    QFont font = QGuiApplication::font();
    font.setPixelSize(int(std::clamp(perLeaf * 0.8, 6.0, 12.0)));
    // A label beside a leaf, turned along its radius
    auto addLabel = [&](QSGNode *parent, int li, const QColor &color, bool bold) {
        const HNode &h = m_nodes[m_leaves[li]];
        auto *turn = new QSGTransformNode;
        QMatrix4x4 m;
        m.translate(float(c.x()), float(c.y()));
        m.rotate(float(qRadiansToDegrees(h.angle)), 0, 0, 1);
        const bool flip = std::cos(h.angle) < 0;
        if (flip)
            m.rotate(180, 0, 0, 1);
        turn->setMatrix(m);
        QFont f = font;
        f.setBold(bold);
        QSGTextNode *label = Sg::textNode(window(), color);
        Sg::addText(label, h.name, f, flip ? QRectF(-R - 6 - 200, -8, 200, 16) : QRectF(R + 6, -8, 200, 16),
                    (flip ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
        turn->appendChildNode(label);
        parent->appendChildNode(turn);
    };

    // Everything once, made again when the layout or the colours change
    if (!m_cacheValid) {
        m_cacheValid = true;
        clearNode(m_sgFade);
        Sg::Mesh links;
        for (int li = 0; li < m_links.size(); ++li)
            addLink(links, li, 1.0, false);
        if (QSGNode *node = links.node())
            m_sgFade->appendChildNode(node);
        // Arcs for the top folders outside, the files just inside
        Sg::Mesh arcs;
        std::function<void(int)> arc = [&](int node) {
            const HNode &h = m_nodes[node];
            if (node > 0 && h.leaf < 0 && h.depth <= 2) {
                const double radius = h.file >= 0 ? R - 6 : R - 6 - (3 - h.depth) * 5;
                const QColor col = withAlpha(h.file >= 0 ? muted : text, h.file >= 0 ? 0.35 : 0.55);
                const double pad = 0.6 * 2 * M_PI / std::max(1, n);
                const double a0 = h.angleFrom - pad, a1 = h.angleTo + pad;
                const int steps = std::max(2, int((a1 - a0) * radius / 4));
                QPointF prev = c + QPointF(std::cos(a0), std::sin(a0)) * radius;
                for (int k = 1; k <= steps; ++k) {
                    const double t = a0 + (a1 - a0) * k / steps;
                    const QPointF p = c + QPointF(std::cos(t), std::sin(t)) * radius;
                    arcs.segment(prev, p, h.file >= 0 ? 1.0 : 1.5, col);
                    prev = p;
                }
            }
            for (int ch : h.children)
                arc(ch);
        };
        arc(0);
        if (QSGNode *node = arcs.node())
            m_sgFade->appendChildNode(node);
        if (labels) {
            for (int li = 0; li < n; ++li)
                addLabel(m_sgFade, li, muted, false);
        }
        // Top folder names along the outside
        QFont big = QGuiApplication::font();
        big.setPixelSize(12);
        big.setBold(true);
        QSGTextNode *tops = Sg::textNode(window(), text);
        for (int ch : m_nodes[0].children) {
            const HNode &h = m_nodes[ch];
            const double mid = (h.angleFrom + h.angleTo) / 2;
            const double rr = R + (labels ? 150 : 24);
            QPointF p = c + QPointF(std::cos(mid) * rr, std::sin(mid) * rr);
            p.setX(std::clamp(p.x(), 84.0, width() - 84));
            p.setY(std::clamp(p.y(), 12.0, height() - 12));
            Sg::addText(tops, h.name.isEmpty() ? QStringLiteral("(root)") : h.name, big, QRectF(p.x() - 80, p.y() - 9, 160, 18),
                        Qt::AlignCenter);
        }
        m_sgFade->appendChildNode(tops);
    }

    // The hovered leaf's links and labels over the rest, faded
    m_sgFade->setOpacity(m_hovered >= 0 ? 0.18 : 1.0);
    clearNode(m_sgOverlay);
    if (m_hovered >= 0) {
        Sg::Mesh hot;
        for (int li : m_leafLinks[m_hovered])
            addLink(hot, li, 4.0, true);
        if (QSGNode *node = hot.node())
            m_sgOverlay->appendChildNode(node);
        for (int li : std::as_const(m_hoverLinked))
            addLabel(m_sgOverlay, li, text, false);
    }
    for (int li = 0; li < n; ++li) {
        const HNode &h = m_nodes[m_leaves[li]];
        if (li == m_hovered || (h.symbol >= 0 && h.symbol == m_activeSymbol))
            addLabel(m_sgOverlay, li, accent, true);
    }
    return root;
}

int BundleView::leafAt(qreal x, qreal y) const
{
    if (m_leaves.isEmpty())
        return -1;
    const double size = std::min(width(), height()) * m_zoom;
    const double R = size / 2 - std::max(90.0, size * 0.16);
    const QPointF c(width() / 2, height() / 2);
    const QPointF d = QPointF(x, y) - c;
    const double r = std::hypot(d.x(), d.y());
    if (r < R * 0.82 || r > R + 170)
        return -1;
    const double a = std::atan2(d.y(), d.x());
    // The leaves' angles ascend around the circle: the nearest by bisection
    if (m_leafAngles.size() != m_leaves.size()) {
        m_leafAngles.clear();
        for (int i = 0; i < m_leaves.size(); ++i)
            m_leafAngles.append({m_nodes[m_leaves[i]].angle, i});
        std::sort(m_leafAngles.begin(), m_leafAngles.end());
    }
    const auto it = std::lower_bound(m_leafAngles.cbegin(), m_leafAngles.cend(), qMakePair(a, -1));
    int best = -1;
    double bestDiff = 1e9;
    const qsizetype at = it - m_leafAngles.cbegin(), n = m_leafAngles.size();
    for (qsizetype k : {(at + n - 1) % n, at % n}) {
        const auto &c = m_leafAngles[k];
        const double diff = std::abs(std::remainder(c.first - a, 2 * M_PI));
        if (diff < bestDiff) {
            bestDiff = diff;
            best = c.second;
        }
    }
    const double perLeaf = 2 * M_PI / std::max<qsizetype>(1, m_leaves.size());
    return bestDiff < std::max(perLeaf * 1.5, 0.02) ? best : -1;
}

void BundleView::hover(qreal x, qreal y)
{
    const int leaf = leafAt(x, y);
    if (leaf == m_hovered)
        return;
    m_hovered = leaf;
    m_hoverLinked.clear();
    if (leaf >= 0) {
        for (int k : m_leafLinks[leaf])
            m_hoverLinked.insert(m_links[k].a == leaf ? m_links[k].b : m_links[k].a);
    }
    emit hoverChanged();
    update();
}

QVariantMap BundleView::hoveredInfo() const
{
    QVariantMap out;
    if (m_hovered < 0 || !m_model)
        return out;
    const HNode &h = m_nodes[m_leaves[m_hovered]];
    int in = 0, outc = 0;
    for (int li : m_leafLinks[m_hovered]) {
        if (m_links[li].a == m_hovered)
            outc += m_links[li].count;
        else
            in += m_links[li].count;
    }
    out[QStringLiteral("name")] = !h.folder.isEmpty() ? h.folder
                                : h.symbol >= 0 ? m_model->displayName(h.symbol) : m_model->files[h.file].relative;
    out[QStringLiteral("kind")] = !h.folder.isEmpty() ? tr("folded folder: click to unfold")
                                : h.symbol >= 0 ? kindName(m_model->symbols[h.symbol].kind) : QStringLiteral("file");
    out[QStringLiteral("file")] = h.file >= 0 ? m_model->files[h.file].relative : QString();
    out[QStringLiteral("incoming")] = in;
    out[QStringLiteral("outgoing")] = outc;
    out[QStringLiteral("links")] = qint64(m_leafLinks[m_hovered].size());
    return out;
}

QVariantMap BundleView::itemAt(qreal x, qreal y) const
{
    QVariantMap out;
    const int leaf = leafAt(x, y);
    if (leaf < 0)
        return out;
    const HNode &h = m_nodes[m_leaves[leaf]];
    out[QStringLiteral("symbol")] = h.symbol;
    out[QStringLiteral("file")] = h.file;
    if (!h.folder.isEmpty())
        out[QStringLiteral("folder")] = h.folder;
    return out;
}

// --- TreemapView ----------------------------------------------------------------------

TreemapView::TreemapView(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    // A hidden view (another tab) waits until it is shown
    connect(Session::instance(), &Session::modelChanged, this, [this] {
        if (isVisible())
            rebuild();
        else
            m_dirty = true;
    });
    connect(Session::instance(), &Session::foldedChanged, this, [this] {
        if (isVisible())
            rebuild();
        else
            m_dirty = true;
    });
    rebuild();
}

void TreemapView::setColorMetric(int m)
{
    if (m == m_colorMetric)
        return;
    m_colorMetric = m;
    emit optionsChanged();
    rebuild();
}

void TreemapView::setSizeMetric(int m)
{
    if (m == m_sizeMetric)
        return;
    m_sizeMetric = m;
    emit optionsChanged();
    rebuild();
}

void TreemapView::setDepth(int d)
{
    if (d == m_depth)
        return;
    m_depth = d;
    emit optionsChanged();
    rebuild();
}

void TreemapView::setRootPath(const QString &p)
{
    if (p == m_rootPath)
        return;
    m_rootPath = p;
    emit optionsChanged();
    rebuild();
}

void TreemapView::setColors(const QVariantMap &c)
{
    m_colors = c;
    m_imageValid = false;
    emit optionsChanged();
    update();
}

void TreemapView::setActiveSymbol(int s)
{
    if (s == m_activeSymbol)
        return;
    m_activeSymbol = s;
    emit optionsChanged();
    update();
}

void TreemapView::geometryChange(const QRectF &n, const QRectF &o)
{
    QQuickItem::geometryChange(n, o);
    if (m_root >= 0 && n.size() != o.size()) {
        m_imageValid = false;
        layoutNode(m_root, QRectF(QPointF(0, 0), n.size()), 0);
        update();
    }
}

QString TreemapView::legendLow() const
{
    switch (m_colorMetric) {
    case Unused: return tr("used");
    case Kind: return tr("by kind");
    default: return QStringLiteral("0");
    }
}

QString TreemapView::legendHigh() const
{
    switch (m_colorMetric) {
    case Unused: return tr("looks unused");
    case Kind: return QString();
    default: return QString::number(qRound(m_maxValue));
    }
}

double TreemapView::metricOf(qint32 s) const
{
    const CodeModel &m = *m_model;
    const Symbol &sym = m.symbols[s];
    auto maxOver = [&](auto fn) {
        double v = fn(sym);
        for (qint32 c : sym.children)
            v = std::max(v, double(fn(m.symbols[c])));
        return v;
    };
    switch (m_colorMetric) {
    case Complexity: return maxOver([](const Symbol &x) { return double(x.metrics.complexity); });
    case Nesting: return maxOver([](const Symbol &x) { return double(x.metrics.nesting); });
    case FanIn: return sym.fanIn;
    case FanOut: return sym.fanOut;
    case Unused: {
        if (Analysis::looksUnused(m, s))
            return 1;
        for (qint32 c : sym.children) {
            if (Analysis::looksUnused(m, c))
                return 0.5;
        }
        return 0;
    }
    case Findings: {
        double n = m_findingCount.value(s);
        for (qint32 c : sym.children)
            n += m_findingCount.value(c);
        return n;
    }
    case Kind: return double(int(sym.kind));
    }
    return 0;
}

void TreemapView::rebuild()
{
    m_model = Session::instance()->model();
    m_tree.clear();
    m_root = -1;
    m_hoveredNode = -1;
    m_findingCount.clear();
    if (!m_model) {
        update();
        return;
    }
    const CodeModel &m = *m_model;
    for (const Analysis::Finding &f : Session::instance()->report().findings) {
        for (qint32 s : f.symbols)
            m_findingCount[s]++;
    }
    TNode root;
    root.name = m_rootPath.isEmpty() ? QFileInfo(m.root).fileName() : m_rootPath;
    root.path = m_rootPath;
    root.kind = -3;
    m_tree.append(root);
    m_root = 0;
    QHash<QString, int> folders;
    folders.insert(m_rootPath, 0);
    std::function<int(const QString &)> folder = [&](const QString &path) -> int {
        auto it = folders.constFind(path);
        if (it != folders.cend())
            return *it;
        const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
        const QString parentPath = slash < 0 ? QString() : path.left(slash);
        const int parent = (parentPath.size() < m_rootPath.size()) ? 0 : folder(parentPath);
        TNode n;
        n.name = slash < 0 ? path : path.mid(slash + 1);
        n.path = path;
        n.kind = -3;
        n.depth = m_tree[parent].depth + 1;
        m_tree.append(n);
        const int id = int(m_tree.size() - 1);
        m_tree[parent].children.append(id);
        folders.insert(path, id);
        return id;
    };
    double maxValue = 0;
    // Functions by the file they are defined in
    QHash<qint32, QVector<qint32>> definedIn;
    for (qint32 s = 0; s < m.symbols.size(); ++s) {
        const Symbol &sym = m.symbols[s];
        if (isFunction(sym.kind) && sym.def.valid())
            definedIn[sym.def.file].append(s);
    }
    for (qint32 f = 0; f < m.files.size(); ++f) {
        const File &file = m.files[f];
        if (!file.project || file.generated)
            continue;
        if (!m_rootPath.isEmpty() && file.module != m_rootPath && !file.module.startsWith(m_rootPath + QLatin1Char('/')))
            continue;
        const int dir = folder(file.module);
        TNode fn;
        fn.name = QFileInfo(file.path).fileName();
        fn.path = file.relative;
        fn.file = f;
        fn.kind = -2;
        fn.depth = m_tree[dir].depth + 1;
        m_tree.append(fn);
        const int fileNode = int(m_tree.size() - 1);
        m_tree[dir].children.append(fileNode);
        double sum = 0;
        // What is written in this file: its classes and functions, and the
        // methods defined here of classes declared elsewhere
        QVector<qint32> leaves;
        for (qint32 s : file.symbols) {
            const Symbol &sym = m.symbols[s];
            if (m.nodeOwner(s) == s)
                leaves.append(s);
        }
        for (qint32 s : definedIn.value(f)) {
            const Symbol &sym = m.symbols[s];
            if (m.nodeOwner(s) != s) {
                const Symbol &owner = m.symbols[m.nodeOwner(s)];
                if (!owner.def.valid() || owner.def.file != f || !owner.def.contains(sym.def))
                    leaves.append(s);
            }
        }
        for (qint32 s : std::as_const(leaves)) {
            const Symbol &sym = m.symbols[s];
            if (sym.has(External) || sym.has(FromMacro) || sym.has(Implicit))
                continue;
            if (sym.kind == SymbolKind::Namespace || sym.kind == SymbolKind::Macro)
                continue;
            if (isFunction(sym.kind) && sym.def.valid() && sym.def.file != f)
                continue;
            double size = 0;
            if (m_sizeMetric == Uniform) {
                size = 1;
            } else if (isType(sym.kind)) {
                size = m_sizeMetric == Lines ? sym.metrics.lines : 0;
                for (qint32 c : sym.children) {
                    const Symbol &cs = m.symbols[c];
                    if (m_sizeMetric == Statements && cs.def.valid() && sym.def.contains(cs.def))
                        size += cs.metrics.statements;
                }
            } else {
                size = m_sizeMetric == Lines ? sym.metrics.lines : sym.metrics.statements;
            }
            if (size <= 0)
                continue;
            TNode sn;
            sn.name = m.nodeOwner(s) != s ? m.symbols[m.nodeOwner(s)].name + QStringLiteral("::") + sym.name : sym.name;
            sn.symbol = s;
            sn.file = f;
            sn.kind = int(sym.kind);
            sn.size = size;
            sn.value = metricOf(s);
            sn.depth = m_tree[fileNode].depth + 1;
            maxValue = std::max(maxValue, sn.value);
            m_tree.append(sn);
            m_tree[fileNode].children.append(int(m_tree.size() - 1));
            sum += size;
        }
        // What isn't in a symbol: includes, comments, top-level code
        const double fileSize = m_sizeMetric == Lines ? file.codeLines : (m_sizeMetric == Uniform ? 1 : 0);
        m_tree[fileNode].size = std::max(sum, fileSize);
        if (m_tree[fileNode].children.isEmpty() && m_tree[fileNode].size <= 0)
            m_tree[fileNode].size = 1;
    }
    // Sizes of folders
    std::function<double(int)> total = [&](int n) -> double {
        TNode &t = m_tree[n];
        if (t.kind != -3)
            return t.size;
        double s = 0;
        for (int c : std::as_const(t.children))
            s += total(c);
        m_tree[n].size = s;
        return s;
    };
    total(0);
    m_maxValue = m_colorMetric == Unused ? 1 : std::max(1.0, maxValue);
    if (m_colorMetric == Complexity)
        m_maxValue = std::max(m_maxValue, 15.0);
    m_imageValid = false;
    layoutNode(m_root, QRectF(0, 0, width(), height()), 0);
    emit optionsChanged();
    update();
}

void TreemapView::layoutNode(int n, const QRectF &r, int depth)
{
    TNode &t = m_tree[n];
    t.rect = r;
    if (t.children.isEmpty() || depth >= m_depth + 2)
        return;
    const bool header = t.kind == -3 || t.kind == -2;
    QRectF inner = r;
    if (header && r.width() > 50 && r.height() > 30)
        inner = r.adjusted(2, t.kind == -3 ? 17 : 15, -2, -2);
    else
        inner = r.adjusted(1, 1, -1, -1);
    if (inner.width() < 3 || inner.height() < 3)
        return;
    QVector<double> sizes;
    const QVector<int> kids = t.children;
    double sum = 0;
    for (int c : kids) {
        sizes << m_tree[c].size;
        sum += m_tree[c].size;
    }
    // A file's own rest, so classes keep their share of the file
    if (t.kind == -2 && t.size > sum)
        sizes << t.size - sum;
    const QVector<QRectF> rects = Layout::squarify(sizes, inner);
    for (int i = 0; i < kids.size(); ++i)
        layoutNode(kids[i], rects[i], depth + 1);
}

QColor TreemapView::colorFor(const TNode &n) const
{
    const QColor low = colorOf(m_colors, QStringLiteral("low"), QColor(90, 180, 110));
    const QColor midc = colorOf(m_colors, QStringLiteral("mid"), QColor(230, 190, 60));
    const QColor high = colorOf(m_colors, QStringLiteral("high"), QColor(220, 70, 60));
    if (m_colorMetric == Kind) {
        const SymbolKind k = SymbolKind(n.kind);
        if (isType(k))
            return colorOf(m_colors, QStringLiteral("typeNode"), QColor(150, 150, 160));
        if (isFunction(k))
            return colorOf(m_colors, QStringLiteral("functionNode"), QColor(230, 190, 60));
        return colorOf(m_colors, QStringLiteral("variableNode"), QColor(80, 140, 220));
    }
    const double t = std::sqrt(std::clamp(n.value / m_maxValue, 0.0, 1.0));
    return t < 0.5 ? mix(low, midc, t * 2) : mix(midc, high, (t - 0.5) * 2);
}

QSGNode *TreemapView::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    QSGNode *root = old;
    if (!root) {
        root = new QSGNode;
        m_sgBase = new QSGNode;
        m_sgOverlay = new QSGNode;
        root->appendChildNode(m_sgBase);
        root->appendChildNode(m_sgOverlay);
        m_imageValid = false;
    }
    if (m_root < 0) {
        m_sgBase->removeAllChildNodes();
        m_sgOverlay->removeAllChildNodes();
        return root;
    }
    const QColor text = colorOf(m_colors, QStringLiteral("text"), Qt::black);
    const QColor muted = colorOf(m_colors, QStringLiteral("textMuted"), Qt::gray);
    const QColor frame = colorOf(m_colors, QStringLiteral("frame"), QColor(200, 200, 200));
    const QColor bg = colorOf(m_colors, QStringLiteral("background"), Qt::white);
    const QColor accent = colorOf(m_colors, QStringLiteral("accent"), QColor(40, 110, 220));
    auto clearNode = [](QSGNode *n) {
        while (QSGNode *c = n->firstChild()) {
            n->removeChildNode(c);
            delete c;
        }
    };

    // The map: made again when the layout, the data or the colours change
    if (!m_imageValid) {
        m_imageValid = true;
        clearNode(m_sgBase);
        QFont font = QGuiApplication::font();
        font.setPixelSize(11);
        QFont bold = font;
        bold.setBold(true);
        Sg::Mesh mesh;
        QSGTextNode *headers = Sg::textNode(window(), text);
        QSGTextNode *files = Sg::textNode(window(), muted);
        QSGTextNode *dark = Sg::textNode(window(), QColor(20, 20, 20));
        QSGTextNode *light = Sg::textNode(window(), QColor(245, 245, 245));
        std::function<void(int)> draw = [&](int n) {
            const TNode &t = m_tree[n];
            const QRectF r = t.rect;
            if (r.width() < 1 || r.height() < 1)
                return;
            if (t.kind == -3 || t.kind == -2) {
                if (n != m_root) {
                    mesh.rect(r.adjusted(1, 1, -1, -1), t.kind == -3 ? withAlpha(frame, 0.18) : withAlpha(bg, 0.6));
                    mesh.frame(r, 1, withAlpha(frame, t.kind == -3 ? 1.0 : 0.7));
                }
                if (r.width() > 50 && r.height() > 30)
                    Sg::addText(t.kind == -3 ? headers : files, t.name, t.kind == -3 ? bold : font,
                                QRectF(r.left() + 4, r.top() + 1, r.width() - 8, 14));
                for (int c : t.children)
                    draw(c);
                return;
            }
            // A class or function: shaded like a cushion, a gap around it
            const QColor c = colorFor(t);
            mesh.gradient(r.adjusted(0.5, 0.5, -0.5, -0.5), c.lighter(118), c.darker(112));
            if (r.width() > 36 && r.height() > 14)
                Sg::addText(c.lightnessF() > 0.55 ? dark : light, t.name, font, QRectF(r.left() + 3, r.top() + 1, r.width() - 6, 14));
            for (int ch : t.children)
                draw(ch);
        };
        draw(m_root);
        if (QSGNode *node = mesh.node())
            m_sgBase->appendChildNode(node);
        for (QSGTextNode *tn : {headers, files, dark, light})
            m_sgBase->appendChildNode(tn);
    }

    // The hovered and the active node outlined over it
    clearNode(m_sgOverlay);
    Sg::Mesh outlines;
    for (int i = 0; i < m_tree.size(); ++i) {
        if (m_tree[i].symbol >= 0 && m_tree[i].symbol == m_activeSymbol)
            outlines.frame(m_tree[i].rect, 3, accent);
    }
    if (m_hoveredNode >= 0)
        outlines.frame(m_tree[m_hoveredNode].rect, 2, text);
    if (QSGNode *node = outlines.node())
        m_sgOverlay->appendChildNode(node);
    return root;
}

int TreemapView::nodeAt(qreal x, qreal y) const
{
    if (m_root < 0)
        return -1;
    int cur = m_root;
    for (;;) {
        int next = -1;
        for (int c : m_tree[cur].children) {
            if (m_tree[c].rect.contains(QPointF(x, y))) {
                next = c;
                break;
            }
        }
        if (next < 0)
            return cur;
        cur = next;
    }
}

void TreemapView::hover(qreal x, qreal y)
{
    const int n = nodeAt(x, y);
    if (n == m_hoveredNode)
        return;
    m_hoveredNode = n;
    m_hoveredInfo.clear();
    if (n >= 0 && m_model) {
        const TNode &t = m_tree[n];
        m_hoveredInfo[QStringLiteral("name")] = t.symbol >= 0 ? m_model->displayName(t.symbol) : t.path;
        m_hoveredInfo[QStringLiteral("kind")] = t.kind == -3 ? QStringLiteral("folder")
                                              : t.kind == -2 ? QStringLiteral("file")
                                                             : kindName(SymbolKind(t.kind));
        m_hoveredInfo[QStringLiteral("size")] = t.size;
        m_hoveredInfo[QStringLiteral("value")] = t.value;
        m_hoveredInfo[QStringLiteral("symbol")] = t.symbol;
        m_hoveredInfo[QStringLiteral("file")] = t.file;
    }
    emit hoverChanged();
    update();
}

QVariantMap TreemapView::itemAt(qreal x, qreal y) const
{
    QVariantMap out;
    const int n = nodeAt(x, y);
    if (n < 0)
        return out;
    const TNode &t = m_tree[n];
    out[QStringLiteral("symbol")] = t.symbol;
    out[QStringLiteral("file")] = t.file;
    out[QStringLiteral("folder")] = t.kind == -3 ? t.path : QString();
    // The folder around a file or symbol, for zooming in
    QString folder;
    if (t.kind == -3)
        folder = t.path;
    else if (t.file >= 0 && m_model)
        folder = m_model->files[t.file].module;
    out[QStringLiteral("enclosingFolder")] = folder;
    return out;
}

// --- MatrixView ------------------------------------------------------------------------

MatrixView::MatrixView(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    // A hidden view (another tab) waits until it is shown
    connect(Session::instance(), &Session::modelChanged, this, [this] {
        if (isVisible())
            rebuild();
        else
            m_dirty = true;
    });
    connect(Session::instance(), &Session::foldedChanged, this, [this] {
        if (isVisible())
            rebuild();
        else
            m_dirty = true;
    });
    rebuild();
}

void MatrixView::setLevel(int l)
{
    if (l == m_level)
        return;
    m_level = l;
    emit optionsChanged();
    rebuild();
}

void MatrixView::setModuleDepth(int d)
{
    if (d == m_moduleDepth)
        return;
    m_moduleDepth = d;
    emit optionsChanged();
    rebuild();
}

void MatrixView::setColors(const QVariantMap &c)
{
    m_colors = c;
    m_baseValid = false;
    emit optionsChanged();
    update();
}

void MatrixView::rebuild()
{
    m_model = Session::instance()->model();
    m_baseValid = false;
    m_order.clear();
    m_position.clear();
    m_selectedLink = -1;
    m_selRow = m_selCol = -1;
    m_hoverRow = m_hoverCol = -1;
    if (!m_model) {
        emit rebuilt();
        update();
        return;
    }
    m_graph = Analysis::dependencyGraph(*m_model, Analysis::Level(std::clamp(m_level, 0, 2)), m_moduleDepth, false, {},
                                        Session::instance()->foldedFolders());
    m_order = Analysis::dependencyOrder(m_graph);
    m_position = QVector<int>(m_graph.names.size(), -1);
    for (int i = 0; i < m_order.size(); ++i)
        m_position[m_order[i]] = i;
    m_maxCount = 1;
    for (const auto &l : m_graph.links)
        m_maxCount = std::max(m_maxCount, l.count + l.includes);
    QFont font = QGuiApplication::font();
    font.setPixelSize(11);
    const QFontMetricsF fm(font);
    double w = 80;
    for (const QString &n : std::as_const(m_graph.names))
        w = std::max(w, fm.horizontalAdvance(n) + 44);
    m_labelWidth = std::min(w, 340.0);
    m_baseValid = false;
    resize();
    emit selectionChanged();
}

void MatrixView::resize()
{
    // Cells to fit the viewport, but not smaller than 3 px: then it scrolls
    const int n = int(m_order.size());
    const double cell = n ? std::clamp((std::min(width() - m_labelWidth, height() - 30) - 8) / n, 3.0, 36.0) : 16;
    if (cell != m_cell)
        m_baseValid = false;
    m_cell = cell;
    emit rebuilt();
    update();
}

void MatrixView::setViewX(qreal v)
{
    if (qFuzzyCompare(v + 1, m_viewX + 1))
        return;
    m_viewX = v;
    emit viewChanged();
    update();
}

void MatrixView::setViewY(qreal v)
{
    if (qFuzzyCompare(v + 1, m_viewY + 1))
        return;
    m_viewY = v;
    emit viewChanged();
    update();
}

void MatrixView::geometryChange(const QRectF &n, const QRectF &o)
{
    QQuickItem::geometryChange(n, o);
    if (n.size() != o.size())
        resize();
}

bool MatrixView::cellAt(qreal x, qreal y, int *row, int *col) const
{
    const int n = int(m_order.size());
    const double cx = x - m_labelWidth, cy = y - 30;
    *row = cy >= 0 ? int(cy / m_cell) : -1;
    *col = cx >= 0 ? int(cx / m_cell) : -1;
    return *row >= 0 && *row < n && *col >= 0 && *col < n;
}

QSGNode *MatrixView::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *root = static_cast<QSGTransformNode *>(old);
    if (!root) {
        root = new QSGTransformNode;
        m_sgBase = new QSGNode;
        m_sgOverlay = new QSGNode;
        root->appendChildNode(m_sgBase);
        root->appendChildNode(m_sgOverlay);
        m_baseValid = false;
    }
    // Scrolling only moves the whole matrix
    QMatrix4x4 matrix;
    matrix.translate(float(-m_viewX), float(-m_viewY));
    root->setMatrix(matrix);
    auto clearNode = [](QSGNode *n) {
        while (QSGNode *c = n->firstChild()) {
            n->removeChildNode(c);
            delete c;
        }
    };
    const int n = int(m_order.size());
    const QColor text = colorOf(m_colors, QStringLiteral("text"), Qt::black);
    const QColor muted = colorOf(m_colors, QStringLiteral("textMuted"), Qt::gray);
    const QColor grid = colorOf(m_colors, QStringLiteral("frame"), QColor(220, 220, 220));
    const QColor accent = colorOf(m_colors, QStringLiteral("accent"), QColor(40, 110, 220));
    const QColor cycle = colorOf(m_colors, QStringLiteral("cycle"), QColor(220, 60, 60));
    const QColor hoverBg = colorOf(m_colors, QStringLiteral("hover"), QColor(0, 0, 0, 20));
    const double L = m_labelWidth, T = 30, c = m_cell;

    if (!m_baseValid) {
        m_baseValid = true;
        clearNode(m_sgBase);
        if (n == 0)
            return root;
        Sg::Mesh mesh;
        // Components on the diagonal
        for (int i = 0; i < n;) {
            const int comp = m_graph.component[m_order[i]];
            int j = i;
            while (j < n && m_graph.component[m_order[j]] == comp)
                ++j;
            if (j - i > 1) {
                const QRectF box(L + i * c, T + i * c, (j - i) * c, (j - i) * c);
                mesh.rect(box, withAlpha(cycle, 0.08));
                mesh.frame(box, 1.5, cycle);
            }
            i = j;
        }
        // Grid and diagonal
        if (c >= 6) {
            const QColor line = withAlpha(grid, 0.6);
            for (int i = 0; i <= n; ++i) {
                mesh.rect(QRectF(L, T + i * c - 0.5, n * c, 1), line);
                mesh.rect(QRectF(L + i * c - 0.5, T, 1, n * c), line);
            }
        }
        for (int i = 0; i < n; ++i)
            mesh.rect(QRectF(L + i * c + 1, T + i * c + 1, c - 1, c - 1), withAlpha(muted, 0.35));
        // Cells
        QFont font = QGuiApplication::font();
        font.setPixelSize(std::clamp(int(c * 0.75), 7, 11));
        QSGTextNode *countsDark = Sg::textNode(window(), text);
        QSGTextNode *countsLight = Sg::textNode(window(), QColor(255, 255, 255));
        for (const auto &l : m_graph.links) {
            const int r = m_position[l.from], col = m_position[l.to];
            if (r < 0 || col < 0)
                continue;
            const int count = l.count + l.includes;
            const double t = std::log2(1.0 + count) / std::log2(1.0 + m_maxCount);
            mesh.rect(QRectF(L + col * c + 1, T + r * c + 1, c - 1, c - 1), withAlpha(l.cyclic ? cycle : accent, 0.2 + 0.8 * t));
            if (c >= 14)
                Sg::addText(t > 0.55 ? countsLight : countsDark, count > 999 ? QStringLiteral("k") : QString::number(count),
                            font, QRectF(L + col * c, T + r * c, c, c), Qt::AlignCenter, false);
        }
        if (QSGNode *node = mesh.node())
            m_sgBase->appendChildNode(node);
        m_sgBase->appendChildNode(countsDark);
        m_sgBase->appendChildNode(countsLight);
        // Labels: index and name on the left, index on top
        if (c >= 7) {
            QSGTextNode *names = Sg::textNode(window(), text);
            QSGTextNode *cyclic = Sg::textNode(window(), cycle);
            QSGTextNode *numbers = Sg::textNode(window(), muted);
            for (int i = 0; i < n; ++i) {
                const int node = m_order[i];
                const bool inCycle = m_graph.componentSize[m_graph.component[node]] > 1;
                Sg::addText(inCycle ? cyclic : names, QString::number(i + 1) + QStringLiteral("  ") + m_graph.names[node], font,
                            QRectF(4, T + i * c, L - 8, c), Qt::AlignRight | Qt::AlignVCenter);
                if (c >= 14 || (i + 1) % 5 == 0)
                    Sg::addText(numbers, QString::number(i + 1), font, QRectF(L + i * c, 4, c, T - 6),
                                Qt::AlignHCenter | Qt::AlignBottom, false);
            }
            m_sgBase->appendChildNode(names);
            m_sgBase->appendChildNode(cyclic);
            m_sgBase->appendChildNode(numbers);
        }
    }

    // Hovered row and column, the selected cell
    clearNode(m_sgOverlay);
    Sg::Mesh over;
    if (m_hoverRow >= 0) {
        over.rect(QRectF(0, T + m_hoverRow * c, L + n * c, c), hoverBg);
        over.rect(QRectF(L + m_hoverCol * c, T, c, n * c), hoverBg);
        over.frame(QRectF(L + m_hoverCol * c, T + m_hoverRow * c, c, c), 1, accent);
    }
    if (m_selRow >= 0)
        over.frame(QRectF(L + m_selCol * c, T + m_selRow * c, c, c), 2, text);
    if (QSGNode *node = over.node())
        m_sgOverlay->appendChildNode(node);
    return root;
}

void MatrixView::hover(qreal x, qreal y)
{
    if (x < 0 || y < 0)   // left the item
        x = y = -1e9;
    x += m_viewX;
    y += m_viewY;
    int r, c;
    if (!cellAt(x, y, &r, &c)) {
        r = c = -1;
        // On a label: the row
        if (y >= 30 && x < m_labelWidth) {
            r = int((y - 30) / m_cell);
            if (r >= m_order.size())
                r = -1;
            c = r;
        }
    }
    if (r == m_hoverRow && c == m_hoverCol)
        return;
    m_hoverRow = r;
    m_hoverCol = c;
    m_hoveredInfo.clear();
    if (r >= 0 && c >= 0) {
        const int a = m_order[r], b = m_order[c];
        m_hoveredInfo[QStringLiteral("from")] = m_graph.names[a];
        m_hoveredInfo[QStringLiteral("to")] = m_graph.names[b];
        const int li = m_graph.linkIndex(a, b);
        int count = 0, includes = 0;
        QStringList kinds;
        if (li >= 0) {
            const auto &l = m_graph.links[li];
            count = l.count;
            includes = l.includes;
            for (int k = 0; k < int(RefKind::Count); ++k) {
                if (l.kinds[k])
                    kinds << QStringLiteral("%1 %2").arg(l.kinds[k]).arg(refKindName(RefKind(k)));
            }
            m_hoveredInfo[QStringLiteral("cyclic")] = l.cyclic;
        }
        m_hoveredInfo[QStringLiteral("count")] = count;
        m_hoveredInfo[QStringLiteral("includes")] = includes;
        m_hoveredInfo[QStringLiteral("kinds")] = kinds.join(QStringLiteral(", "));
        m_hoveredInfo[QStringLiteral("diagonal")] = r == c;
    }
    emit hoverChanged();
    update();
}

void MatrixView::select(qreal x, qreal y)
{
    x += m_viewX;
    y += m_viewY;
    int r, c;
    if (!cellAt(x, y, &r, &c))
        return;
    m_selRow = r;
    m_selCol = c;
    m_selectedLink = m_graph.linkIndex(m_order[r], m_order[c]);
    emit selectionChanged();
    update();
}

QVariantList MatrixView::selectedReferences(int limit) const
{
    QVariantList out;
    if (!m_model || m_selectedLink < 0)
        return out;
    Session *s = Session::instance();
    for (qint32 r : Analysis::linkReferences(*m_model, m_graph, m_selectedLink, limit))
        out << s->referenceInfo(r);
    return out;
}

QVariantMap MatrixView::nodeInfo(qreal x, qreal y) const
{
    x += m_viewX;
    y += m_viewY;
    QVariantMap out;
    if (y < 30 || x >= m_labelWidth || m_order.isEmpty())
        return out;
    const int r = int((y - 30) / m_cell);
    if (r < 0 || r >= m_order.size())
        return out;
    const int node = m_order[r];
    out[QStringLiteral("name")] = m_graph.names[node];
    out[QStringLiteral("level")] = m_level;
    if (!m_graph.folder[node].isEmpty())
        out[QStringLiteral("folder")] = m_graph.folder[node];
    else if (m_level == 1)
        out[QStringLiteral("file")] = m_graph.ids[node];
    else if (m_level == 2)
        out[QStringLiteral("symbol")] = m_graph.ids[node];
    return out;
}

void BundleView::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged && value.boolValue && m_dirty) {
        m_dirty = false;
        rebuild();
    }
}

void TreemapView::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged && value.boolValue && m_dirty) {
        m_dirty = false;
        rebuild();
    }
}

void MatrixView::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged && value.boolValue && m_dirty) {
        m_dirty = false;
        rebuild();
    }
}
