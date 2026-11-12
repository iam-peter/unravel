#include "views.h"

#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPathStroker>
#include <QQuickWindow>
#include <QtMath>

#include <algorithm>
#include <cmath>

#include "layout.h"
#include "session.h"

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

void arrowHead(QPainter *p, const QPointF &tip, const QPointF &from, double size, bool hollow, const QColor &fill,
               const QColor &background)
{
    QPointF d = tip - from;
    const double len = std::hypot(d.x(), d.y());
    if (len < 0.001)
        return;
    d /= len;
    const QPointF n(-d.y(), d.x());
    const QPointF base = tip - d * size;
    QPolygonF tri;
    tri << tip << base + n * size * 0.5 << base - n * size * 0.5;
    p->setBrush(hollow ? background : fill);
    p->drawPolygon(tri);
}

} // namespace

// --- EdgeLayer -----------------------------------------------------------------------

EdgeLayer::EdgeLayer(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

void EdgeLayer::setScene(GraphScene *s)
{
    if (s == m_scene)
        return;
    if (m_scene)
        disconnect(m_scene, nullptr, this, nullptr);
    m_scene = s;
    if (m_scene) {
        connect(m_scene, &GraphScene::changed, this, [this] { update(); });
        connect(m_scene, &GraphScene::highlightChanged, this, [this] { update(); });
    }
    emit sceneChanged();
    update();
}

void EdgeLayer::setZoom(qreal z)
{
    if (qFuzzyCompare(z, m_zoom))
        return;
    m_zoom = z;
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
    update();
}

void EdgeLayer::setHoveredEdge(int e)
{
    if (e == m_hoveredEdge)
        return;
    m_hoveredEdge = e;
    emit viewChanged();
    update();
}

QColor EdgeLayer::colorFor(int kind, bool cyclic) const
{
    if (cyclic)
        return colorOf(m_colors, QStringLiteral("cycle"), QColor(220, 60, 60));
    return colorOf(m_colors, kindKey(kind));
}

void EdgeLayer::paint(QPainter *painter)
{
    if (!m_scene)
        return;
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate(m_panX, m_panY);
    painter->scale(m_zoom, m_zoom);
    const auto &edges = m_scene->edges();
    const bool focus = m_scene->hasHighlight() || m_hoveredEdge >= 0;
    const QColor background = colorOf(m_colors, QStringLiteral("background"), Qt::white);
    // Faint ones first, so the highlighted lie on top
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < edges.size(); ++i) {
            const GraphScene::SceneEdge &e = edges[i];
            if (e.path.isEmpty())
                continue;
            const bool strong = !focus || m_scene->edgeHighlighted(i) || i == m_hoveredEdge;
            if ((pass == 0) == strong)
                continue;
            QColor c = colorFor(e.kind, e.cyclic);
            if (!strong)
                c = withAlpha(c, 0.13);
            double width = std::clamp(1.1 + std::log2(double(std::max(1, e.count))) * 0.6, 1.1, 5.0);
            if (focus && strong)
                width += 0.8;
            if (i == m_hoveredEdge)
                width += 1;
            QPen pen(c, width);
            pen.setCapStyle(Qt::RoundCap);
            if (e.dashed)
                pen.setStyle(Qt::DashLine);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(e.path);
            // Arrow at the target
            QPen solid(c, 1.2);
            painter->setPen(solid);
            const bool inherit = e.kind == int(RefKind::Inherit);
            arrowHead(painter, e.tip, e.tipFrom, inherit ? 12 : 8 + width, inherit, c, background);
        }
    }
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
        if (e.path.isEmpty() || !e.hitBounds.adjusted(-tol, -tol, tol, tol).contains(p))
            continue;
        // Distance to the flattened curve: no stroke to build per move
        const QPolygonF poly = e.path.toSubpathPolygons().value(0);
        for (qsizetype k = 0; k + 1 < poly.size(); ++k) {
            const QPointF a = poly[k], b = poly[k + 1];
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
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setAcceptHoverEvents(false);
    // A hidden view (another tab) waits until it is shown
    connect(Session::instance(), &Session::modelChanged, this, [this] {
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
    QQuickPaintedItem::geometryChange(n, o);
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
    for (qint32 f : std::as_const(files)) {
        const File &file = m.files[f];
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

void BundleView::paint(QPainter *painter)
{
    if (!m_model || m_leaves.isEmpty())
        return;
    const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const QColor text = colorOf(m_colors, QStringLiteral("text"), Qt::black);
    const QColor muted = colorOf(m_colors, QStringLiteral("textMuted"), Qt::gray);
    const QColor source = colorOf(m_colors, QStringLiteral("source"), QColor(60, 170, 90));
    const QColor target = colorOf(m_colors, QStringLiteral("target"), QColor(220, 70, 60));
    const QColor accent = colorOf(m_colors, QStringLiteral("accent"), QColor(40, 110, 220));

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
    int maxCount = 1;
    for (const BLink &l : std::as_const(m_links))
        maxCount = std::max(maxCount, l.count);
    auto drawLink = [&](QPainter *p, int li, double alphaScale, bool emphasis) {
        const BLink &l = m_links[li];
        const QPainterPath &path = m_paths[li];
        if (path.isEmpty())
            return;
        const double w = 0.8 + 2.2 * std::log2(1.0 + l.count) / std::log2(1.0 + maxCount);
        const double alpha = std::clamp((0.22 + 0.5 * double(l.count) / maxCount) * alphaScale, 0.04, 1.0);
        QPen pen;
        pen.setWidthF(emphasis ? w + 0.8 : w);
        if (m_colorByKind) {
            pen.setColor(withAlpha(colorOf(m_colors, kindKey(l.kind)), alpha));
        } else {
            QLinearGradient g(m_ends[li].first, m_ends[li].second);
            g.setColorAt(0, withAlpha(source, alpha));
            g.setColorAt(1, withAlpha(target, alpha));
            pen.setBrush(g);
        }
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        p->drawPath(path);
    };

    if (!m_cacheValid || m_cache.size() != (size() * dpr).toSize()) {
        m_cache = QImage((size() * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
        m_cache.setDevicePixelRatio(dpr);
        m_cache.fill(Qt::transparent);
        QPainter cp(&m_cache);
        cp.setRenderHint(QPainter::Antialiasing);
        for (int li = 0; li < m_links.size(); ++li)
            drawLink(&cp, li, 1.0, false);
        m_cacheValid = true;
    }
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setOpacity(m_hovered >= 0 ? 0.18 : 1.0);
    painter->drawImage(QPointF(0, 0), m_cache);
    painter->setOpacity(1.0);
    if (m_hovered >= 0) {
        for (int li : m_leafLinks[m_hovered])
            drawLink(painter, li, 4.0, true);
    }

    // Arcs for the folders and files, labels for the leaves
    const double size = std::min(width(), height()) * m_zoom;
    const double R = size / 2 - std::max(90.0, size * 0.16);
    const QPointF c(width() / 2, height() / 2);
    QFont font = QGuiApplication::font();
    const int n = int(m_leaves.size());
    const double perLeaf = 2 * M_PI * R / std::max(1, n);
    font.setPixelSize(int(std::clamp(perLeaf * 0.8, 6.0, 12.0)));
    painter->setFont(font);
    const bool labels = perLeaf >= 5.5;
    for (int li = 0; li < n; ++li) {
        const HNode &h = m_nodes[m_leaves[li]];
        const bool hot = li == m_hovered || h.symbol == m_activeSymbol;
        const bool linked = !hot && m_hoverLinked.contains(li);
        if (!labels && !hot && !linked)
            continue;
        painter->save();
        painter->translate(c);
        painter->rotate(qRadiansToDegrees(h.angle));
        const bool flip = std::cos(h.angle) < 0;
        painter->setPen(hot ? accent : linked ? text : muted);
        QFont f = font;
        f.setBold(hot);
        painter->setFont(f);
        const QString label = h.name;
        if (flip) {
            painter->rotate(180);
            painter->drawText(QRectF(-R - 6 - 200, -8, 200, 16), Qt::AlignRight | Qt::AlignVCenter, label);
        } else {
            painter->drawText(QRectF(R + 6, -8, 200, 16), Qt::AlignLeft | Qt::AlignVCenter, label);
        }
        painter->restore();
    }
    // Group arcs: top folders outside, files just inside
    std::function<void(int)> arcs = [&](int node) {
        const HNode &h = m_nodes[node];
        if (node > 0 && h.leaf < 0 && h.depth <= 2) {
            const double radius = h.file >= 0 ? R - 6 : R - 6 - (3 - h.depth) * 5;
            QPen pen(withAlpha(h.file >= 0 ? muted : text, h.file >= 0 ? 0.35 : 0.55), h.file >= 0 ? 2 : 3);
            painter->setPen(pen);
            const double pad = 0.6 * 2 * M_PI / std::max(1, n);
            const double a0 = qRadiansToDegrees(h.angleFrom - pad), a1 = qRadiansToDegrees(h.angleTo + pad);
            painter->drawArc(QRectF(c.x() - radius, c.y() - radius, 2 * radius, 2 * radius), int(-a0 * 16),
                             int(-(a1 - a0) * 16));
        }
        for (int ch : h.children)
            arcs(ch);
    };
    arcs(0);
    // Top folder names along the outside
    QFont big = QGuiApplication::font();
    big.setPixelSize(12);
    big.setBold(true);
    painter->setFont(big);
    for (int ch : m_nodes[0].children) {
        const HNode &h = m_nodes[ch];
        const double mid = (h.angleFrom + h.angleTo) / 2;
        const double rr = R + (labels ? 150 : 24);
        QPointF p = c + QPointF(std::cos(mid) * rr, std::sin(mid) * rr);
        p.setX(std::clamp(p.x(), 84.0, width() - 84));
        p.setY(std::clamp(p.y(), 12.0, height() - 12));
        painter->setPen(text);
        const QString name = h.name.isEmpty() ? QStringLiteral("(root)") : h.name;
        const QRectF br(p.x() - 80, p.y() - 9, 160, 18);
        painter->drawText(br, Qt::AlignCenter, name);
    }
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
    out[QStringLiteral("name")] = h.symbol >= 0 ? m_model->displayName(h.symbol) : m_model->files[h.file].relative;
    out[QStringLiteral("kind")] = h.symbol >= 0 ? kindName(m_model->symbols[h.symbol].kind) : QStringLiteral("file");
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
    return out;
}

// --- TreemapView ----------------------------------------------------------------------

TreemapView::TreemapView(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(false);
    // A hidden view (another tab) waits until it is shown
    connect(Session::instance(), &Session::modelChanged, this, [this] {
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
    QQuickPaintedItem::geometryChange(n, o);
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

void TreemapView::paint(QPainter *painter)
{
    if (m_root < 0)
        return;
    const QColor text = colorOf(m_colors, QStringLiteral("text"), Qt::black);
    const QColor muted = colorOf(m_colors, QStringLiteral("textMuted"), Qt::gray);
    const QColor frame = colorOf(m_colors, QStringLiteral("frame"), QColor(200, 200, 200));
    const QColor bg = colorOf(m_colors, QStringLiteral("background"), Qt::white);
    const QColor accent = colorOf(m_colors, QStringLiteral("accent"), QColor(40, 110, 220));
    QFont font = QGuiApplication::font();
    font.setPixelSize(11);
    QFont bold = font;
    bold.setBold(true);
    const QFontMetricsF fm(font);
    std::function<void(int, int)> draw = [&](int n, int depth) {
        const TNode &t = m_tree[n];
        const QRectF r = t.rect;
        if (r.width() < 1 || r.height() < 1)
            return;
        if (t.kind == -3 || t.kind == -2) {
            if (n != m_root) {
                painter->setPen(QPen(withAlpha(frame, t.kind == -3 ? 1.0 : 0.7), 1));
                painter->setBrush(t.kind == -3 ? withAlpha(frame, 0.18) : withAlpha(bg, 0.6));
                painter->drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
            }
            if (r.width() > 50 && r.height() > 30) {
                painter->setFont(t.kind == -3 ? bold : font);
                painter->setPen(t.kind == -3 ? text : muted);
                painter->drawText(QRectF(r.left() + 4, r.top() + 1, r.width() - 8, 14), Qt::AlignLeft | Qt::AlignVCenter,
                                  fm.elidedText(t.name, Qt::ElideMiddle, r.width() - 8));
            }
            for (int c : t.children)
                draw(c, depth + 1);
            return;
        }
        // A class or function: shaded like a cushion
        const QColor c = colorFor(t);
        QLinearGradient g(r.topLeft(), r.bottomRight());
        g.setColorAt(0, c.lighter(118));
        g.setColorAt(1, c.darker(112));
        painter->setPen(QPen(withAlpha(bg, 0.9), 1));
        painter->setBrush(g);
        painter->drawRect(r);
        if (r.width() > 36 && r.height() > 14) {
            painter->setFont(font);
            painter->setPen(c.lightnessF() > 0.55 ? QColor(20, 20, 20) : QColor(245, 245, 245));
            painter->drawText(r.adjusted(3, 1, -3, -1), Qt::AlignLeft | Qt::AlignTop,
                              fm.elidedText(t.name, Qt::ElideRight, r.width() - 6));
        }
        for (int ch : t.children)
            draw(ch, depth + 1);
    };
    // The map itself once; hover and the active symbol drawn over it
    const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const QSize px = (size() * dpr).toSize();
    if (!m_imageValid || m_image.size() != px) {
        m_image = QImage(px, QImage::Format_ARGB32_Premultiplied);
        m_image.setDevicePixelRatio(dpr);
        m_image.fill(Qt::transparent);
        QPainter ip(&m_image);
        QPainter *onScreen = painter;
        painter = &ip;
        draw(m_root, 0);
        painter = onScreen;
        m_imageValid = true;
    }
    painter->drawImage(QPointF(0, 0), m_image);
    auto outline = [&](int n, const QColor &col, double w) {
        if (n < 0)
            return;
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(col, w));
        painter->drawRect(m_tree[n].rect.adjusted(w / 2, w / 2, -w / 2, -w / 2));
    };
    for (int i = 0; i < m_tree.size(); ++i) {
        if (m_tree[i].symbol >= 0 && m_tree[i].symbol == m_activeSymbol)
            outline(i, accent, 3);
    }
    outline(m_hoveredNode, text, 2);
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
    : QQuickPaintedItem(parent)
{
    setAntialiasing(false);
    // A hidden view (another tab) waits until it is shown
    connect(Session::instance(), &Session::modelChanged, this, [this] {
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
    emit optionsChanged();
    update();
}

void MatrixView::rebuild()
{
    m_model = Session::instance()->model();
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
    m_graph = Analysis::dependencyGraph(*m_model, Analysis::Level(std::clamp(m_level, 0, 2)), m_moduleDepth);
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
    resize();
    emit selectionChanged();
}

void MatrixView::resize()
{
    // Cells to fit the viewport, but not smaller than 3 px: then it scrolls
    const int n = int(m_order.size());
    m_cell = n ? std::clamp((std::min(width() - m_labelWidth, height() - 30) - 8) / n, 3.0, 36.0) : 16;
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
    QQuickPaintedItem::geometryChange(n, o);
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

void MatrixView::paint(QPainter *painter)
{
    const int n = int(m_order.size());
    if (!n)
        return;
    const QColor text = colorOf(m_colors, QStringLiteral("text"), Qt::black);
    const QColor muted = colorOf(m_colors, QStringLiteral("textMuted"), Qt::gray);
    const QColor grid = colorOf(m_colors, QStringLiteral("frame"), QColor(220, 220, 220));
    const QColor accent = colorOf(m_colors, QStringLiteral("accent"), QColor(40, 110, 220));
    const QColor cycle = colorOf(m_colors, QStringLiteral("cycle"), QColor(220, 60, 60));
    const QColor hoverBg = colorOf(m_colors, QStringLiteral("hover"), QColor(0, 0, 0, 20));
    QFont font = QGuiApplication::font();
    font.setPixelSize(std::clamp(int(m_cell * 0.75), 7, 11));
    painter->setFont(font);
    const QFontMetricsF fm(font);
    const double L = m_labelWidth, T = 30, c = m_cell;
    // Only what shows
    painter->translate(-m_viewX, -m_viewY);
    const int from = std::clamp(int(std::min(m_viewX - L, m_viewY - T) / c) - 1, 0, n);
    const int rowFrom = std::clamp(int((m_viewY - T) / c) - 1, 0, n);
    const int rowTo = std::clamp(int((m_viewY + height() - T) / c) + 2, 0, n);
    const int colFrom = std::clamp(int((m_viewX - L) / c) - 1, 0, n);
    const int colTo = std::clamp(int((m_viewX + width() - L) / c) + 2, 0, n);
    Q_UNUSED(from)
    // Hovered row and column
    if (m_hoverRow >= 0) {
        painter->fillRect(QRectF(0, T + m_hoverRow * c, L + n * c, c), hoverBg);
        painter->fillRect(QRectF(L + m_hoverCol * c, T, c, n * c), hoverBg);
    }
    // Components on the diagonal
    for (int i = 0; i < n;) {
        const int comp = m_graph.component[m_order[i]];
        int j = i;
        while (j < n && m_graph.component[m_order[j]] == comp)
            ++j;
        if (j - i > 1) {
            painter->setPen(QPen(cycle, 1.5));
            painter->setBrush(withAlpha(cycle, 0.08));
            painter->drawRect(QRectF(L + i * c, T + i * c, (j - i) * c, (j - i) * c));
        }
        i = j;
    }
    // Grid
    painter->setPen(QPen(withAlpha(grid, 0.6), 1));
    if (c >= 6) {
        for (int i = rowFrom; i <= rowTo; ++i)
            painter->drawLine(QPointF(L + colFrom * c, T + i * c), QPointF(L + colTo * c, T + i * c));
        for (int i = colFrom; i <= colTo; ++i)
            painter->drawLine(QPointF(L + i * c, T + rowFrom * c), QPointF(L + i * c, T + rowTo * c));
    }
    // Diagonal
    for (int i = std::max(rowFrom, colFrom); i < std::min(rowTo, colTo); ++i)
        painter->fillRect(QRectF(L + i * c + 1, T + i * c + 1, c - 1, c - 1), withAlpha(muted, 0.35));
    // Cells
    for (const auto &l : m_graph.links) {
        const int r = m_position[l.from], col = m_position[l.to];
        if (r < rowFrom || r >= rowTo || col < colFrom || col >= colTo)
            continue;
        const int count = l.count + l.includes;
        const double t = std::log2(1.0 + count) / std::log2(1.0 + m_maxCount);
        const QColor base = l.cyclic ? cycle : accent;
        painter->fillRect(QRectF(L + col * c + 1, T + r * c + 1, c - 1, c - 1), withAlpha(base, 0.2 + 0.8 * t));
        if (c >= 14) {
            painter->setPen(t > 0.55 ? QColor(255, 255, 255) : text);
            painter->drawText(QRectF(L + col * c, T + r * c, c, c), Qt::AlignCenter,
                              count > 999 ? QStringLiteral("k") : QString::number(count));
        }
    }
    if (m_selRow >= 0) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(text, 2));
        painter->drawRect(QRectF(L + m_selCol * c, T + m_selRow * c, c, c));
    }
    // Labels: index and name on the left, index on top
    for (int i = std::min(rowFrom, colFrom); i < std::max(rowTo, colTo); ++i) {
        const int node = m_order[i];
        const bool hot = i == m_hoverRow || i == m_hoverCol;
        painter->setPen(hot ? accent : (m_graph.componentSize[m_graph.component[node]] > 1 ? cycle : text));
        if (c >= 7) {
            const QString label = QString::number(i + 1) + QStringLiteral("  ") + m_graph.names[node];
            painter->drawText(QRectF(4, T + i * c, L - 8, c), Qt::AlignRight | Qt::AlignVCenter,
                              fm.elidedText(label, Qt::ElideMiddle, L - 8));
            painter->setPen(hot ? accent : muted);
            painter->drawText(QRectF(L + i * c, 4, c, T - 6), Qt::AlignHCenter | Qt::AlignBottom,
                              c >= 14 || (i + 1) % 5 == 0 ? QString::number(i + 1) : QString());
        }
    }
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
    if (m_level == 1)
        out[QStringLiteral("file")] = m_graph.ids[node];
    else if (m_level == 2)
        out[QStringLiteral("symbol")] = m_graph.ids[node];
    return out;
}

void BundleView::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickPaintedItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged && value.boolValue && m_dirty) {
        m_dirty = false;
        rebuild();
    }
}

void TreemapView::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickPaintedItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged && value.boolValue && m_dirty) {
        m_dirty = false;
        rebuild();
    }
}

void MatrixView::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickPaintedItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged && value.boolValue && m_dirty) {
        m_dirty = false;
        rebuild();
    }
}
