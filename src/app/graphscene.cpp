#include "graphscene.h"

#include <QFileInfo>
#include <QFontMetricsF>
#include <QHash>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "changes.h"
#include "layout.h"
#include "session.h"

using namespace Unravel;

namespace {

constexpr double kHeader = 30;
constexpr double kRow = 22;
constexpr double kSection = 18;
constexpr double kPad = 12;
constexpr double kMinW = 140;
constexpr double kMaxW = 380;
constexpr double kNodeGap = 14;

QString escapeXml(QString s)
{
    s.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    s.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    s.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    s.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return s;
}

// Positions along a column: each node as near its wish as the order and
// the gaps allow
QVector<double> pack(const QVector<double> &want, const QVector<double> &size, double gap)
{
    const qsizetype n = want.size();
    QVector<double> fwd(n), bwd(n), out(n);
    for (qsizetype i = 0; i < n; ++i) {
        fwd[i] = want[i];
        if (i > 0)
            fwd[i] = std::max(fwd[i], fwd[i - 1] + size[i - 1] + gap);
    }
    for (qsizetype i = n - 1; i >= 0; --i) {
        bwd[i] = want[i];
        if (i + 1 < n)
            bwd[i] = std::min(bwd[i], bwd[i + 1] - size[i] - gap);
    }
    for (qsizetype i = 0; i < n; ++i)
        out[i] = (fwd[i] + bwd[i]) / 2;
    for (qsizetype i = 1; i < n; ++i)
        out[i] = std::max(out[i], out[i - 1] + size[i - 1] + gap);
    return out;
}

QString memberName(const CodeModel &m, qint32 member, qint32 node)
{
    // Relative to the node: Inner::f for a nested class's method
    const QString q = m.displayName(member);
    const QString prefix = m.displayName(node) + QStringLiteral("::");
    QString name = q.startsWith(prefix) ? q.mid(prefix.size()) : m.symbols[member].name;
    if (isFunction(m.symbols[member].kind))
        name += QStringLiteral("()");
    return name;
}

QString subtitleOf(const CodeModel &m, qint32 s)
{
    const Symbol &sym = m.symbols[s];
    if (sym.parent >= 0)
        return m.displayName(sym.parent);
    const Location w = sym.where();
    if (w.file >= 0)
        return QFileInfo(m.files[w.file].path).fileName();
    return QString();
}

} // namespace

// --- GraphScene ------------------------------------------------------------------

GraphScene::GraphScene(QObject *parent)
    : QObject(parent)
{
    connect(Session::instance(), &Session::modelChanged, this, [this] {
        m_hidden.clear();   // symbol ids of the old model
        scheduleBuild();
    });
    connect(Session::instance(), &Session::foldedChanged, this, &GraphScene::scheduleBuild);
}

void GraphScene::setActive(bool a)
{
    if (a == m_active)
        return;
    m_active = a;
    emit activeChanged();
    if (m_active && m_dirty)
        scheduleBuild();
}

void GraphScene::setFont(const QFont &f)
{
    if (f == m_font)
        return;
    m_font = f;
    emit fontChanged();
    scheduleBuild();
}

void GraphScene::setHighlightNode(int n)
{
    if (n == m_highlightNode)
        return;
    m_highlightNode = n;
    emit highlightChanged();
}

void GraphScene::setHighlightSymbol(int s)
{
    if (s == m_highlightSymbol)
        return;
    m_highlightSymbol = s;
    emit highlightChanged();
}

void GraphScene::setSelectedEdge(int e)
{
    if (e == m_selectedEdge)
        return;
    m_selectedEdge = e;
    emit highlightChanged();
}

QVariantList GraphScene::hiddenSymbols() const
{
    QVariantList out;
    for (int s : m_hidden)
        out << s;
    return out;
}

bool GraphScene::edgeHighlighted(int e) const
{
    if (e < 0 || e >= m_edges.size())
        return false;
    const SceneEdge &edge = m_edges[e];
    if (e == m_selectedEdge)
        return true;
    if (m_highlightSymbol >= 0 && (edge.fromSymbol == m_highlightSymbol || edge.toSymbol == m_highlightSymbol))
        return true;
    if (m_highlightNode >= 0 && (edge.fromNode == m_highlightNode || edge.toNode == m_highlightNode))
        return true;
    return false;
}

std::shared_ptr<const CodeModel> GraphScene::model() const
{
    return Session::instance()->model();
}

double GraphScene::textWidth(const QString &text, bool bold) const
{
    QFont f = m_font;
    if (bold)
        f.setWeight(QFont::DemiBold);
    return QFontMetricsF(f).horizontalAdvance(text);
}

void GraphScene::scheduleBuild()
{
    if (!m_active) {
        m_dirty = true;
        return;
    }
    m_dirty = false;
    if (m_buildPending)
        return;
    m_buildPending = true;
    QTimer::singleShot(0, this, [this] {
        m_buildPending = false;
        rebuild();
    });
}

void GraphScene::rebuild()
{
    m_nodes.clear();
    m_groups.clear();
    m_rects.clear();
    m_nodeSymbols.clear();
    m_edges.clear();
    m_summary.clear();
    // Node indexes change with the layout: what was highlighted is gone
    m_selectedEdge = -1;
    m_highlightNode = -1;
    m_highlightSymbol = -1;
    m_built = model();
    if (m_built)
        build();
    finish();
}

void GraphScene::finish()
{
    // Move everything to positive coordinates with a margin
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool first = true;
    for (const QRectF &r : std::as_const(m_rects)) {
        if (first) {
            minX = r.left();
            minY = r.top();
            maxX = r.right();
            maxY = r.bottom();
            first = false;
        } else {
            minX = std::min(minX, r.left());
            minY = std::min(minY, r.top());
            maxX = std::max(maxX, r.right());
            maxY = std::max(maxY, r.bottom());
        }
    }
    for (const SceneEdge &e : std::as_const(m_edges)) {
        for (const QPointF &p : e.points) {
            minX = std::min(minX, p.x());
            minY = std::min(minY, p.y());
            maxX = std::max(maxX, p.x());
            maxY = std::max(maxY, p.y());
        }
    }
    const double margin = 60;
    const double dx = margin - minX, dy = margin - minY;
    for (qsizetype i = 0; i < m_rects.size(); ++i) {
        m_rects[i].translate(dx, dy);
        QVariantMap n = m_nodes[i].toMap();
        n[QStringLiteral("x")] = m_rects[i].x();
        n[QStringLiteral("y")] = m_rects[i].y();
        n[QStringLiteral("index")] = int(i);
        m_nodes[i] = n;
    }
    for (SceneEdge &e : m_edges) {
        for (QPointF &p : e.points)
            p += QPointF(dx, dy);
    }
    makePaths();
    for (QVariant &gv : m_groups) {
        QVariantMap g = gv.toMap();
        g[QStringLiteral("x")] = g.value(QStringLiteral("x")).toDouble() + dx;
        g[QStringLiteral("y")] = g.value(QStringLiteral("y")).toDouble() + dy;
        gv = g;
    }
    m_width = m_rects.isEmpty() ? 0 : maxX - minX + 2 * margin;
    m_height = m_rects.isEmpty() ? 0 : maxY - minY + 2 * margin;
    m_revision++;
    emit changed();
    emit highlightChanged();
}

void GraphScene::makePaths()
{
    for (SceneEdge &e : m_edges) {
        e.path = edgePath(e, m_edgeStyle);
        e.poly = e.path.toSubpathPolygons().value(0);
        // The arrow, once, not in every frame
        const double len = e.path.length();
        e.tip = e.path.pointAtPercent(1.0);
        e.tipFrom = e.path.pointAtPercent(std::max(0.0, 1.0 - 9.0 / std::max(1.0, len)));
        e.hitBounds = e.path.controlPointRect();
        e.hit = QPainterPath();
    }
}

void GraphScene::setEdgeStyle(int style)
{
    if (style == m_edgeStyle)
        return;
    m_edgeStyle = style;
    emit edgeStyleChanged();
    // Same nodes, other lines: no new layout, no new fit
    makePaths();
    emit pathsChanged();
}

QPainterPath GraphScene::edgePath(const SceneEdge &e, int style)
{
    QPainterPath path;
    const QVector<QPointF> &p = e.points;
    if (p.size() < 2)
        return path;
    auto dir = [](Port port) -> QPointF {
        switch (port) {
        case Left: return QPointF(-1, 0);
        case Right: return QPointF(1, 0);
        case Top: return QPointF(0, -1);
        case Bottom: return QPointF(0, 1);
        default: return QPointF(0, 0);
        }
    };
    path.moveTo(p.first());
    if (style == Straight) {
        for (qsizetype i = 1; i < p.size(); ++i)
            path.lineTo(p[i]);
        return path;
    }
    if (style == Orthogonal) {
        // Steps along the flow: out of the port, across halfway, into the next
        const bool horizontal = e.startPort == Left || e.startPort == Right || e.startPort == Auto;
        if (p.size() == 2 && e.startPort == e.endPort && e.startPort != Auto) {
            // Both ends on one side: a bracket
            const QPointF out = dir(e.startPort) * 28;
            path.lineTo(p[0] + out);
            path.lineTo(horizontal ? QPointF(p[0].x() + out.x(), p[1].y()) : QPointF(p[1].x(), p[0].y() + out.y()));
            path.lineTo(p[1]);
            return path;
        }
        for (qsizetype i = 1; i < p.size(); ++i) {
            const QPointF a = p[i - 1], b = p[i];
            if (horizontal) {
                const double mx = (a.x() + b.x()) / 2;
                path.lineTo(mx, a.y());
                path.lineTo(mx, b.y());
            } else {
                const double my = (a.y() + b.y()) / 2;
                path.lineTo(a.x(), my);
                path.lineTo(b.x(), my);
            }
            path.lineTo(b);
        }
        return path;
    }
    if (p.size() == 2) {
        const QPointF a = p[0], b = p[1];
        const double dist = std::hypot(b.x() - a.x(), b.y() - a.y());
        double reach = std::max(30.0, dist * 0.45);
        // Both ends on the same side: a bracket out and back
        if (e.startPort == e.endPort && (e.startPort == Left || e.startPort == Right))
            reach = std::max(50.0, std::abs(b.y() - a.y()) * 0.4);
        QPointF c1 = a + dir(e.startPort) * reach;
        QPointF c2 = b + dir(e.endPort) * reach;
        if (e.startPort == Auto)
            c1 = a + (b - a) / 3;
        if (e.endPort == Auto)
            c2 = b - (b - a) / 3;
        path.cubicTo(c1, c2, b);
        return path;
    }
    // Through the bends: Catmull-Rom as cubic Béziers, the ends along the ports
    QVector<QPointF> pts = p;
    for (qsizetype i = 0; i + 1 < pts.size(); ++i) {
        const QPointF p0 = i > 0 ? pts[i - 1] : pts[i] - dir(e.startPort) * 40;
        const QPointF p1 = pts[i];
        const QPointF p2 = pts[i + 1];
        const QPointF p3 = i + 2 < pts.size() ? pts[i + 2] : pts[i + 1] - dir(e.endPort) * 40;
        QPointF c1 = p1 + (p2 - p0) / 6.0;
        QPointF c2 = p2 - (p3 - p1) / 6.0;
        if (i == 0 && e.startPort != Auto)
            c1 = p1 + dir(e.startPort) * std::max(20.0, std::hypot(p2.x() - p1.x(), p2.y() - p1.y()) / 3);
        if (i + 2 == pts.size() && e.endPort != Auto)
            c2 = p2 + dir(e.endPort) * std::max(20.0, std::hypot(p2.x() - p1.x(), p2.y() - p1.y()) / 3);
        path.cubicTo(c1, c2, p2);
    }
    return path;
}

QVariantMap GraphScene::edgeInfo(int e) const
{
    QVariantMap out;
    auto m = m_built;
    if (!m || e < 0 || e >= m_edges.size())
        return out;
    const SceneEdge &edge = m_edges[e];
    out[QStringLiteral("kind")] = edge.kind;
    out[QStringLiteral("kindName")] = refKindName(RefKind(edge.kind));
    out[QStringLiteral("count")] = edge.count;
    out[QStringLiteral("cyclic")] = edge.cyclic;
    out[QStringLiteral("from")] = edge.fromSymbol;
    out[QStringLiteral("to")] = edge.toSymbol;
    auto nodeTitle = [&](int node) {
        return node >= 0 && node < m_nodes.size() ? m_nodes[node].toMap().value(QStringLiteral("title")).toString()
                                                  : QString();
    };
    out[QStringLiteral("fromName")] = edge.fromSymbol >= 0 ? m->displayName(edge.fromSymbol) : nodeTitle(edge.fromNode);
    out[QStringLiteral("toName")] = edge.toSymbol >= 0 ? m->displayName(edge.toSymbol) : nodeTitle(edge.toNode);
    return out;
}

QVariantList GraphScene::edgeReferences(int e, int limit) const
{
    QVariantList out;
    auto m = m_built;
    if (!m || e < 0 || e >= m_edges.size())
        return out;
    const SceneEdge &edge = m_edges[e];
    Session *session = Session::instance();
    for (qint32 me : edge.modelEdges) {
        if (me < 0 || me >= m->edges.size())
            continue;
        const Edge &ed = m->edges[me];
        for (qint32 r : m->refsTo[ed.to]) {
            if (out.size() >= limit)
                return out;
            const Reference &ref = m->refs[r];
            if (ref.from == ed.from && ref.kind == ed.kind)
                out << session->referenceInfo(r);
        }
    }
    return out;
}

QRectF GraphScene::nodeRect(int node) const
{
    return node >= 0 && node < m_rects.size() ? m_rects[node] : QRectF();
}

int GraphScene::nodeOfSymbol(int symbol) const
{
    return int(m_nodeSymbols.indexOf(symbol));
}

void GraphScene::hide(int symbol)
{
    m_hidden.insert(symbol);
    scheduleBuild();
}

void GraphScene::unhideAll()
{
    m_hidden.clear();
    scheduleBuild();
}

QString GraphScene::toSvg(const QVariantMap &colors) const
{
    auto color = [&](const QString &key, const char *fallback) {
        const QString c = colors.value(key).toString();
        return c.isEmpty() ? QString::fromLatin1(fallback) : c;
    };
    QString out;
    QTextStream t(&out);
    t << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << m_width << "\" height=\"" << m_height
      << "\" viewBox=\"0 0 " << m_width << " " << m_height << "\" font-family=\"" << escapeXml(m_font.family())
      << "\" font-size=\"12\">\n";
    t << "<rect width=\"100%\" height=\"100%\" fill=\"" << color(QStringLiteral("background"), "#ffffff") << "\"/>\n";
    static const char *kindKeys[] = {"call", "type", "var", "inherit", "override", "macro", "include", "type", "type"};
    for (const SceneEdge &e : m_edges) {
        const QPainterPath path = e.path.isEmpty() ? QPainterPath() : e.path;
        if (path.isEmpty())
            continue;
        QString d;
        for (int i = 0; i < path.elementCount(); ++i) {
            const QPainterPath::Element el = path.elementAt(i);
            if (el.isMoveTo())
                d += QStringLiteral("M%1 %2 ").arg(el.x).arg(el.y);
            else if (el.isLineTo())
                d += QStringLiteral("L%1 %2 ").arg(el.x).arg(el.y);
            else if (el.type == QPainterPath::CurveToElement)
                d += QStringLiteral("C%1 %2 ").arg(el.x).arg(el.y);
            else
                d += QStringLiteral("%1 %2 ").arg(el.x).arg(el.y);
        }
        const QString stroke = e.cyclic ? color(QStringLiteral("cycle"), "#d33")
                                        : color(QString::fromLatin1(kindKeys[std::clamp(e.kind, 0, 8)]), "#888");
        t << "<path d=\"" << d << "\" fill=\"none\" stroke=\"" << stroke << "\" stroke-width=\""
          << std::min(1.0 + std::log2(double(std::max(1, e.count))), 5.0) << "\"" << (e.dashed ? " stroke-dasharray=\"5 4\"" : "")
          << "/>\n";
    }
    for (qsizetype i = 0; i < m_nodes.size(); ++i) {
        const QVariantMap n = m_nodes[i].toMap();
        const QRectF r = m_rects[i];
        t << "<rect x=\"" << r.x() << "\" y=\"" << r.y() << "\" width=\"" << r.width() << "\" height=\"" << r.height()
          << "\" rx=\"4\" fill=\"" << color(QStringLiteral("node"), "#f4f4f5") << "\" stroke=\""
          << color(QStringLiteral("nodeBorder"), "#bbb") << "\"/>\n";
        t << "<text x=\"" << r.x() + kPad << "\" y=\"" << r.y() + 19 << "\" font-weight=\"600\" fill=\""
          << color(QStringLiteral("text"), "#111") << "\">" << escapeXml(n.value(QStringLiteral("title")).toString()) << "</text>\n";
        const QVariantList rows = n.value(QStringLiteral("rows")).toList();
        for (const QVariant &rv : rows) {
            const QVariantMap row = rv.toMap();
            t << "<text x=\"" << r.x() + kPad + 6 << "\" y=\"" << r.y() + row.value(QStringLiteral("y")).toDouble() + 15
              << "\" fill=\"" << color(QStringLiteral("text"), "#111") << "\">"
              << escapeXml(row.value(QStringLiteral("name")).toString()) << "</text>\n";
        }
    }
    t << "</svg>\n";
    return out;
}

// --- SymbolGraph -------------------------------------------------------------------

SymbolGraph::SymbolGraph(QObject *parent)
    : GraphScene(parent)
{
    connect(this, &SymbolGraph::inputChanged, this, &GraphScene::scheduleBuild);
    connect(Changes::instance(), &Changes::changesChanged, this, &GraphScene::scheduleBuild);
}

void SymbolGraph::setSymbol(int s)
{
    if (s == m_symbol)
        return;
    m_symbol = s;
    m_expandedSides.clear();
    m_allMembers = false;
    emit inputChanged();
}

void SymbolGraph::setFile(int f)
{
    if (f == m_file)
        return;
    m_file = f;
    emit inputChanged();
}

void SymbolGraph::setAllMembers(bool v)
{
    if (v == m_allMembers)
        return;
    m_allMembers = v;
    emit inputChanged();
}

void SymbolGraph::setGrouping(int g)
{
    if (g == m_grouping)
        return;
    m_grouping = g;
    emit inputChanged();
}

void SymbolGraph::setMaxPerSide(int n)
{
    if (n == m_maxPerSide)
        return;
    m_maxPerSide = n;
    emit inputChanged();
}

void SymbolGraph::setShowExternal(bool v)
{
    if (v == m_showExternal)
        return;
    m_showExternal = v;
    emit inputChanged();
}

QVariantList SymbolGraph::kindFilter() const
{
    QVariantList out;
    for (int k : m_kinds)
        out << k;
    return out;
}

void SymbolGraph::setKindFilter(const QVariantList &kinds)
{
    QSet<int> k;
    for (const QVariant &v : kinds)
        k.insert(v.toInt());
    if (k == m_kinds)
        return;
    m_kinds = k;
    emit inputChanged();
}

void SymbolGraph::expandSide(int side)
{
    m_expandedSides.insert(side);
    scheduleBuild();
}

void SymbolGraph::build()
{
    const CodeModel &m = *m_built;
    if (m_symbol < 0 || m_symbol >= m.symbols.size()) {
        if (m_file >= 0 && m_file < m.files.size())
            buildForFile();
        return;
    }
    const qint32 active = m_symbol;
    const Symbol &a = m.symbols[active];
    const qint32 center = a.kind == SymbolKind::Namespace ? active : m.nodeOwner(active);
    const Symbol &c = m.symbols[center];

    // What the edges start or end at: the symbol, and for a node all its members
    QSet<qint32> focus = {active};
    if (active == center && a.kind != SymbolKind::Namespace) {
        QVector<qint32> todo = {active};
        while (!todo.isEmpty()) {
            const qint32 s = todo.takeLast();
            for (qint32 ch : m.symbols[s].children) {
                if (!focus.contains(ch)) {
                    focus.insert(ch);
                    todo.append(ch);
                }
            }
        }
    }

    enum Side { LeftSide, RightSide, TopSide, BottomSide };
    struct Neighbour {
        qint32 node;
        QSet<qint32> members;
        double in = 0, out = 0, inherit = 0, inheritIn = 0;
        int side = RightSide;
        double bary = 0;
        int baryCount = 0;
        QString group;
    };
    struct Item {
        qint32 modelEdge;
        qint32 from, to;     // symbols
        qint32 neighbour;    // node symbol of the other end
        bool outgoing;
    };
    QHash<qint32, Neighbour> neighbours;
    QVector<Item> items;
    QVector<Item> internal;
    // Neighbours in folded folders become one node per folder, keyed -2 - index
    const QStringList folded = Session::instance()->foldedFolders();
    QStringList foldNames;
    const Location centerAt = c.where();
    const QString centerFold = centerAt.file >= 0 && m.files[centerAt.file].project
                                   ? Analysis::foldedFolderOf(m.files[centerAt.file].module, folded) : QString();
    auto foldKey = [&](qint32 node) -> qint32 {
        if (folded.isEmpty())
            return node;
        const Location w = m.symbols[node].where();
        if (w.file < 0 || !m.files[w.file].project)
            return node;
        const QString fold = Analysis::foldedFolderOf(m.files[w.file].module, folded);
        if (fold.isEmpty() || fold == centerFold)
            return node;
        qsizetype i = foldNames.indexOf(fold);
        if (i < 0) {
            foldNames << fold;
            i = foldNames.size() - 1;
        }
        return qint32(-2 - i);
    };
    QHash<qint32, QSet<qint32>> foldMembers;   // what is used in each folded folder
    QSet<qint32> centerMembers;
    if (active != center)
        centerMembers.insert(active);

    auto consider = [&](qint32 e, bool outgoing) {
        const Edge &edge = m.edges[e];
        if (edge.kind == RefKind::Declaration)
            return;
        if (!m_kinds.isEmpty() && !m_kinds.contains(int(edge.kind)))
            return;
        const qint32 self = outgoing ? edge.from : edge.to;
        const qint32 other = outgoing ? edge.to : edge.from;
        if (other < 0 || m_hidden.contains(other))
            return;
        const Symbol &o = m.symbols[other];
        if (o.kind == SymbolKind::Namespace)
            return;
        if (!m_showExternal && o.has(External))
            return;
        if ((edge.flags & RefImplicit) && edge.count == 1 && edge.kind == RefKind::Call
            && (o.kind == SymbolKind::Constructor || o.kind == SymbolKind::Destructor))
            return;   // the compiler's own calls of constructors add noise
        qint32 node = m.nodeOwner(other);
        if (node == center && active != center && other != center) {
            // Within the class: shown as brackets beside the members
            internal.append({e, edge.from, edge.to, center, outgoing});
            centerMembers.insert(other);
            return;
        }
        if (node == center || node < 0 || m_hidden.contains(node))
            return;   // within the node
        node = foldKey(node);
        Neighbour &n = neighbours[node];
        n.node = node;
        if (node < -1)
            foldMembers[node].insert(other);
        else if (other != node)
            n.members.insert(other);
        const double w = edge.count;
        if (edge.kind == RefKind::Inherit || edge.kind == RefKind::Override) {
            if (outgoing)
                n.inherit += w;
            else
                n.inheritIn += w;
        } else if (outgoing) {
            n.out += w;
        } else {
            n.in += w;
        }
        if (self != center)
            centerMembers.insert(self);
        items.append({e, edge.from, edge.to, node, outgoing});
    };
    if (a.kind != SymbolKind::Namespace) {
        for (qint32 f : std::as_const(focus)) {
            for (qint32 e : m.outEdges[f])
                consider(e, true);
            for (qint32 e : m.inEdges[f])
                consider(e, false);
        }
    }

    // The centre's rows: connected members, or all of them
    QVector<qint32> allChildren;
    for (qint32 ch : c.children) {
        const Symbol &x = m.symbols[ch];
        if (x.has(Implicit) || x.has(FromMacro))
            continue;
        if (a.kind == SymbolKind::Namespace && x.has(External))
            continue;
        allChildren.append(ch);
    }
    QVector<qint32> rows;
    int hidden = 0;
    if (m_allMembers || a.kind == SymbolKind::Namespace || (centerMembers.isEmpty() && allChildren.size() <= 40)) {
        rows = allChildren;
        for (qint32 s : std::as_const(centerMembers)) {
            if (!rows.contains(s))
                rows.append(s);
        }
    } else {
        rows = QVector<qint32>(centerMembers.cbegin(), centerMembers.cend());
        for (qint32 ch : std::as_const(allChildren)) {
            if (!centerMembers.contains(ch))
                ++hidden;
        }
    }
    if (a.kind == SymbolKind::Namespace && rows.size() > 120)
        rows.resize(120);
    auto kindOrder = [&](qint32 s) {
        const SymbolKind k = m.symbols[s].kind;
        if (isType(k))
            return 0;
        if (k == SymbolKind::Constructor || k == SymbolKind::Destructor)
            return 1;
        if (isFunction(k))
            return 2;
        return 3;
    };
    std::sort(rows.begin(), rows.end(), [&](qint32 x, qint32 y) {
        const Symbol &sx = m.symbols[x], &sy = m.symbols[y];
        const int ax = sx.access == Access::None ? 0 : int(sx.access);
        const int ay = sy.access == Access::None ? 0 : int(sy.access);
        if (ax != ay)
            return ax < ay;
        if (kindOrder(x) != kindOrder(y))
            return kindOrder(x) < kindOrder(y);
        return sx.where().line < sy.where().line;
    });

    // Node geometry
    auto makeNode = [&](qint32 sym, const QVector<qint32> &memberRows, bool isCenter, bool sections,
                        QHash<qint32, double> *rowY) -> QVariantMap {
        const Symbol &s = m.symbols[sym];
        QVariantMap n;
        const QString title = sym == center && isCenter ? m.displayName(sym) : s.name;
        double w = textWidth(title, true) + 2 * kPad + 26;
        double y = kHeader;
        QVariantList rowList, sectionList;
        int lastAccess = -1;
        for (qint32 r : memberRows) {
            const Symbol &rs = m.symbols[r];
            if (sections && int(rs.access) != lastAccess && rs.access != Access::None) {
                lastAccess = int(rs.access);
                QVariantMap sec;
                sec[QStringLiteral("label")] = rs.access == Access::Public ? QStringLiteral("public")
                                             : rs.access == Access::Protected ? QStringLiteral("protected")
                                                                              : QStringLiteral("private");
                sec[QStringLiteral("y")] = y;
                sectionList << sec;
                y += kSection;
            }
            QVariantMap row;
            const QString name = memberName(m, r, sym);
            row[QStringLiteral("symbol")] = r;
            row[QStringLiteral("name")] = name;
            row[QStringLiteral("kind")] = int(rs.kind);
            row[QStringLiteral("y")] = y;
            row[QStringLiteral("h")] = kRow;
            row[QStringLiteral("active")] = r == active;
            row[QStringLiteral("access")] = int(rs.access);
            row[QStringLiteral("external")] = rs.has(External);
            row[QStringLiteral("virtual")] = rs.has(Virtual);
            row[QStringLiteral("static")] = rs.has(Static);
            row[QStringLiteral("unused")] = Analysis::looksUnused(m, r);
            row[QStringLiteral("signature")] = rs.signature;
            if (Changes::instance()->isTouched(r)) {   // in the selected commits
                const History::Touched t = Changes::instance()->touched().value(r);
                row[QStringLiteral("changed")] = true;
                row[QStringLiteral("added")] = t.added;
                row[QStringLiteral("lines")] = t.lines;
            }
            rowList << row;
            if (rowY)
                rowY->insert(r, y + kRow / 2);
            w = std::max(w, textWidth(name) + 2 * kPad + 28);
            y += kRow;
        }
        if (!memberRows.isEmpty())
            y += 6;
        w = std::clamp(w, kMinW, kMaxW);
        n[QStringLiteral("symbol")] = sym;
        n[QStringLiteral("file")] = -1;
        n[QStringLiteral("kind")] = int(s.kind);
        n[QStringLiteral("title")] = title;
        n[QStringLiteral("subtitle")] = isCenter ? s.signature : subtitleOf(m, sym);
        n[QStringLiteral("w")] = w;
        n[QStringLiteral("h")] = std::max(y, kHeader + 2);
        n[QStringLiteral("external")] = s.has(External);
        n[QStringLiteral("center")] = isCenter;
        n[QStringLiteral("active")] = sym == active;
        n[QStringLiteral("rows")] = rowList;
        n[QStringLiteral("sections")] = sectionList;
        n[QStringLiteral("unused")] = Analysis::looksUnused(m, sym);
        n[QStringLiteral("abstract")] = s.has(Abstract);
        return n;
    };

    QHash<qint32, double> centerRowY;
    const bool classLike = isType(c.kind) || c.kind == SymbolKind::Namespace;
    QVariantMap centerNode = makeNode(center, rows, true, classLike && c.kind != SymbolKind::Namespace, &centerRowY);
    centerNode[QStringLiteral("hiddenCount")] = hidden;
    if (hidden > 0)   // room for the "+n" beside the title
        centerNode[QStringLiteral("w")] = std::min(kMaxW + 60, centerNode[QStringLiteral("w")].toDouble() + 34);
    const double cw = centerNode[QStringLiteral("w")].toDouble();
    const double ch = centerNode[QStringLiteral("h")].toDouble();
    m_nodes << centerNode;
    m_rects << QRectF(0, 0, cw, ch);
    m_nodeSymbols << center;

    // Sides, bary-centres from the centre rows they connect to
    for (Neighbour &n : neighbours) {
        if (n.inherit > 0)
            n.side = TopSide;
        else if (n.inheritIn > 0)
            n.side = BottomSide;
        else
            n.side = n.out >= n.in ? RightSide : LeftSide;
        if (n.node < -1) {
            n.group = QStringLiteral("(folded)");
        } else if (m_grouping == ByNamespace) {
            const qint32 p = m.symbols[n.node].parent;
            n.group = p >= 0 ? m.displayName(p) : QStringLiteral("(global)");
        } else if (m_grouping == ByFile) {
            const Location w = m.symbols[n.node].where();
            n.group = w.file >= 0 ? m.files[w.file].relative : QStringLiteral("(unknown)");
        }
    }
    for (const Item &it : std::as_const(items)) {
        Neighbour &n = neighbours[it.neighbour];
        const qint32 self = it.outgoing ? it.from : it.to;
        n.bary += centerRowY.value(self, kHeader / 2);
        n.baryCount++;
    }

    QHash<qint32, int> nodeIndex;   // neighbour symbol -> scene node
    QHash<qint32, QHash<qint32, double>> nodeRowY;
    QHash<int, int> bundleOfSide;
    QHash<qint32, int> bundled;     // neighbour symbol -> bundle node
    const double gapX = 150 + std::min<double>(neighbours.size(), 40) * 2.5;
    const double gapY = 70;

    for (int side = LeftSide; side <= BottomSide; ++side) {
        QVector<Neighbour *> list;
        for (Neighbour &n : neighbours) {
            if (n.side == side)
                list.append(&n);
        }
        if (list.isEmpty())
            continue;
        // The strongest, then bundle the rest
        std::sort(list.begin(), list.end(), [](const Neighbour *x, const Neighbour *y) {
            return (x->in + x->out + x->inherit + x->inheritIn) > (y->in + y->out + y->inherit + y->inheritIn);
        });
        QVector<Neighbour *> rest;
        const int limit = m_expandedSides.contains(side) ? 1000 : m_maxPerSide;
        if (list.size() > limit) {
            rest = list.mid(limit);
            list.resize(limit);
        }
        std::sort(list.begin(), list.end(), [&](const Neighbour *x, const Neighbour *y) {
            if (x->group != y->group)
                return x->group < y->group;
            const double bx = x->baryCount ? x->bary / x->baryCount : 0;
            const double by = y->baryCount ? y->bary / y->baryCount : 0;
            if (std::abs(bx - by) > 0.5)
                return bx < by;
            const QString nx = x->node < -1 ? foldNames[-2 - x->node] : m.symbols[x->node].name;
            const QString ny = y->node < -1 ? foldNames[-2 - y->node] : m.symbols[y->node].name;
            return nx < ny;
        });
        QVector<QVariantMap> made;
        QVector<double> sizes, wants;
        double maxW = 0, maxH = 0;
        for (Neighbour *n : std::as_const(list)) {
            if (n->node < -1) {
                // A folded folder: one card, unfolded by a click
                const QString fold = foldNames[-2 - n->node];
                QVariantMap node;
                node[QStringLiteral("symbol")] = -1;
                node[QStringLiteral("file")] = -1;
                node[QStringLiteral("kind")] = -4;
                node[QStringLiteral("folder")] = fold;
                node[QStringLiteral("title")] = QFileInfo(fold).fileName() + QStringLiteral("/");
                node[QStringLiteral("subtitle")] = tr("%1 · %n symbols used", nullptr, int(foldMembers.value(n->node).size())).arg(fold);
                node[QStringLiteral("compact")] = true;
                node[QStringLiteral("rows")] = QVariantList();
                node[QStringLiteral("side")] = side;
                node[QStringLiteral("group")] = n->group;
                const double w = std::clamp(std::max(textWidth(node[QStringLiteral("title")].toString(), true) + 30,
                                                     textWidth(node[QStringLiteral("subtitle")].toString()) * 0.92) + 2 * kPad,
                                            kMinW, kMaxW);
                node[QStringLiteral("w")] = w;
                node[QStringLiteral("h")] = 44.0;
                made << node;
                maxW = std::max(maxW, w);
                maxH = std::max(maxH, 44.0);
                const bool vertical = side == LeftSide || side == RightSide;
                sizes << (vertical ? 44.0 : w);
                wants << (vertical ? (n->baryCount ? n->bary / n->baryCount : ch / 2) - 22 : 0);
                continue;
            }
            QVector<qint32> mem(n->members.cbegin(), n->members.cend());
            std::sort(mem.begin(), mem.end(), [&](qint32 x, qint32 y) { return m.symbols[x].name < m.symbols[y].name; });
            if (mem.size() > 30)
                mem.resize(30);
            QHash<qint32, double> ry;
            QVariantMap node = makeNode(n->node, mem, false, false, &ry);
            node[QStringLiteral("side")] = side;
            node[QStringLiteral("group")] = n->group;
            nodeRowY.insert(n->node, ry);
            made << node;
            const double w = node[QStringLiteral("w")].toDouble(), h = node[QStringLiteral("h")].toDouble();
            maxW = std::max(maxW, w);
            maxH = std::max(maxH, h);
            const bool vertical = side == LeftSide || side == RightSide;
            sizes << (vertical ? h : w);
            wants << (vertical ? (n->baryCount ? n->bary / n->baryCount : ch / 2) - h / 2 : 0);
        }
        if (side == TopSide || side == BottomSide) {
            // Centred in a row above or below
            double total = 0;
            for (double s : std::as_const(sizes))
                total += s + kNodeGap * 2;
            double x = cw / 2 - total / 2;
            for (qsizetype i = 0; i < made.size(); ++i) {
                wants[i] = x;
                x += sizes[i] + kNodeGap * 2;
            }
        }
        // Group headers need room
        if (m_grouping != NoGrouping) {
            for (qsizetype i = 1; i < made.size(); ++i) {
                if (made[i][QStringLiteral("group")] != made[i - 1][QStringLiteral("group")])
                    wants[i] = std::max(wants[i], wants[i - 1] + sizes[i - 1] + 34);
            }
        }
        const QVector<double> pos = pack(wants, sizes, m_grouping != NoGrouping ? kNodeGap + 8 : kNodeGap);
        for (qsizetype i = 0; i < made.size(); ++i) {
            QVariantMap &node = made[i];
            const double w = node[QStringLiteral("w")].toDouble(), h = node[QStringLiteral("h")].toDouble();
            QRectF r;
            switch (side) {
            case LeftSide: r = QRectF(-gapX - w, pos[i], w, h); break;
            case RightSide: r = QRectF(cw + gapX, pos[i], w, h); break;
            case TopSide: r = QRectF(pos[i], -gapY - h, w, h); break;
            default: r = QRectF(pos[i], ch + gapY, w, h); break;
            }
            nodeIndex.insert(list[i]->node, int(m_nodes.size()));
            m_nodes << node;
            m_rects << r;
            m_nodeSymbols << list[i]->node;
        }
        if (!rest.isEmpty()) {
            // "+ n more", which expands the side
            QVariantMap b;
            b[QStringLiteral("symbol")] = -1;
            b[QStringLiteral("file")] = -1;
            b[QStringLiteral("bundle")] = true;
            b[QStringLiteral("side")] = side;
            b[QStringLiteral("count")] = int(rest.size());
            b[QStringLiteral("title")] = tr("%n more", nullptr, int(rest.size()));
            QStringList names;
            for (const Neighbour *n : std::as_const(rest))
                names << (n->node < -1 ? foldNames[-2 - n->node] : m.symbols[n->node].name);
            b[QStringLiteral("subtitle")] = names.mid(0, 12).join(QStringLiteral(", ")) + (names.size() > 12 ? QStringLiteral("…") : QString());
            b[QStringLiteral("w")] = 160.0;
            b[QStringLiteral("h")] = 34.0;
            b[QStringLiteral("kind")] = -1;
            b[QStringLiteral("rows")] = QVariantList();
            double y = 0;
            if (!m_rects.isEmpty()) {
                const QRectF last = m_rects.last();
                y = side == LeftSide || side == RightSide ? last.bottom() + kNodeGap * 2 : last.top();
            }
            QRectF r;
            switch (side) {
            case LeftSide: r = QRectF(-gapX - 160, y, 160, 34); break;
            case RightSide: r = QRectF(cw + gapX, y, 160, 34); break;
            case TopSide: r = QRectF(m_rects.last().right() + kNodeGap * 2, -gapY - 34, 160, 34); break;
            default: r = QRectF(m_rects.last().right() + kNodeGap * 2, ch + gapY, 160, 34); break;
            }
            const int bi = int(m_nodes.size());
            bundleOfSide.insert(side, bi);
            for (const Neighbour *n : std::as_const(rest))
                bundled.insert(n->node, bi);
            m_nodes << b;
            m_rects << r;
            m_nodeSymbols << -1;
        }
        // Frames around the groups
        if (m_grouping != NoGrouping && !made.isEmpty()) {
            int start = 0;
            for (qsizetype i = 1; i <= made.size(); ++i) {
                if (i == made.size() || made[i][QStringLiteral("group")] != made[start][QStringLiteral("group")]) {
                    QRectF box;
                    for (qsizetype k = start; k < i; ++k)
                        box = box.united(m_rects[nodeIndex.value(list[k]->node)]);
                    box.adjust(-8, -24, 8, 8);
                    QVariantMap g;
                    g[QStringLiteral("x")] = box.x();
                    g[QStringLiteral("y")] = box.y();
                    g[QStringLiteral("w")] = box.width();
                    g[QStringLiteral("h")] = box.height();
                    g[QStringLiteral("label")] = made[start][QStringLiteral("group")];
                    m_groups << g;
                    start = int(i);
                }
            }
        }
    }

    // Edges between rows (or headers)
    auto rowPoint = [&](int node, qint32 sym, bool rightSide) -> QPointF {
        const QRectF r = m_rects[node];
        double y = r.top() + kHeader / 2;
        if (node == 0) {
            y = r.top() + centerRowY.value(sym, kHeader / 2);
        } else {
            const auto it = nodeRowY.constFind(m_nodeSymbols[node]);
            if (it != nodeRowY.cend())
                y = r.top() + it->value(sym, kHeader / 2);
        }
        return QPointF(rightSide ? r.right() : r.left(), y);
    };
    QHash<quint64, int> sceneEdgeOf;
    for (const Item &it : std::as_const(items)) {
        const Edge &edge = m.edges[it.modelEdge];
        int other = nodeIndex.value(it.neighbour, -1);
        const bool isBundle = other < 0;
        if (isBundle)
            other = bundled.value(it.neighbour, -1);
        if (other < 0)
            continue;
        const int side = m_nodes[other].toMap().value(QStringLiteral("side")).toInt();
        const qint32 selfSym = it.outgoing ? it.from : it.to;
        // A bundle or a folded folder: all of it meets in one point
        const qint32 otherSym = isBundle || it.neighbour < -1 ? -1 : (it.outgoing ? it.to : it.from);
        // One scene edge per pair of rows and kind
        const quint64 key = (quint64(quint32(selfSym)) << 40) ^ (quint64(quint32(otherSym + 1)) << 8)
                          ^ (quint64(it.outgoing) << 7) ^ quint64(edge.kind) ^ (quint64(other) << 20);
        auto found = sceneEdgeOf.constFind(key);
        if (found != sceneEdgeOf.cend()) {
            SceneEdge &se = m_edges[*found];
            se.count += edge.count;
            se.modelEdges << it.modelEdge;
            continue;
        }
        SceneEdge se;
        se.kind = int(edge.kind);
        se.count = edge.count;
        se.modelEdges << it.modelEdge;
        se.dashed = edge.kind == RefKind::Override;
        QPointF centerPt, otherPt;
        Port centerPort, otherPort;
        switch (side) {
        case LeftSide:
            centerPt = rowPoint(0, selfSym, false);
            otherPt = rowPoint(other, otherSym, true);
            centerPort = Left;
            otherPort = Right;
            break;
        case RightSide:
            centerPt = rowPoint(0, selfSym, true);
            otherPt = rowPoint(other, otherSym, false);
            centerPort = Right;
            otherPort = Left;
            break;
        default: {
            const bool top = side == TopSide;
            if (edge.kind == RefKind::Inherit) {
                const QRectF cr = m_rects[0], orr = m_rects[other];
                centerPt = QPointF(cr.center().x(), top ? cr.top() : cr.bottom());
                otherPt = QPointF(orr.center().x(), top ? orr.bottom() : orr.top());
                centerPort = top ? Top : Bottom;
                otherPort = top ? Bottom : Top;
            } else {
                centerPt = rowPoint(0, selfSym, false);
                otherPt = rowPoint(other, otherSym, false);
                centerPort = Left;
                otherPort = Left;
            }
            break;
        }
        }
        if (it.outgoing) {
            se.points = {centerPt, otherPt};
            se.startPort = centerPort;
            se.endPort = otherPort;
            se.fromNode = 0;
            se.toNode = other;
        } else {
            se.points = {otherPt, centerPt};
            se.startPort = otherPort;
            se.endPort = centerPort;
            se.fromNode = other;
            se.toNode = 0;
        }
        se.fromSymbol = it.from;
        se.toSymbol = it.to;
        if (isBundle) {
            if (it.outgoing)
                se.toSymbol = -1;
            else
                se.fromSymbol = -1;
        }
        sceneEdgeOf.insert(key, int(m_edges.size()));
        m_edges << se;
    }
    // Uses within the class: callers of the member on the left, what it
    // uses on the right
    for (const Item &it : std::as_const(internal)) {
        const Edge &edge = m.edges[it.modelEdge];
        if (it.from == it.to)
            continue;
        SceneEdge se;
        se.kind = int(edge.kind);
        se.count = edge.count;
        se.modelEdges << it.modelEdge;
        se.dashed = edge.kind == RefKind::Override;
        const bool right = it.outgoing;
        se.points = {rowPoint(0, it.from, right), rowPoint(0, it.to, right)};
        se.startPort = right ? Right : Left;
        se.endPort = right ? Right : Left;
        se.fromNode = 0;
        se.toNode = 0;
        se.fromSymbol = it.from;
        se.toSymbol = it.to;
        m_edges << se;
    }
    m_summary = tr("%1 · %n related", nullptr, int(neighbours.size())).arg(m.displayName(active));
}

void SymbolGraph::buildForFile()
{
    const CodeModel &m = *m_built;
    const File &f = m.files[m_file];
    // The file with what it declares, includers left, includes right
    QVector<qint32> syms;
    for (qint32 s : f.symbols) {
        const Symbol &sym = m.symbols[s];
        if (m.nodeOwner(s) == s && sym.kind != SymbolKind::Namespace && !sym.has(Implicit) && !sym.has(FromMacro))
            syms.append(s);
    }
    std::sort(syms.begin(), syms.end(), [&](qint32 a, qint32 b) { return m.symbols[a].where().line < m.symbols[b].where().line; });
    if (syms.size() > 80)
        syms.resize(80);
    QVariantMap center;
    const QString title = f.relative;
    double w = textWidth(title, true) + 2 * kPad + 26;
    QVariantList rowList;
    double y = kHeader;
    for (qint32 s : std::as_const(syms)) {
        QVariantMap row;
        const QString name = m.symbols[s].name + (isFunction(m.symbols[s].kind) ? QStringLiteral("()") : QString());
        row[QStringLiteral("symbol")] = s;
        row[QStringLiteral("name")] = name;
        row[QStringLiteral("kind")] = int(m.symbols[s].kind);
        row[QStringLiteral("y")] = y;
        row[QStringLiteral("h")] = kRow;
        row[QStringLiteral("unused")] = Analysis::looksUnused(m, s);
        row[QStringLiteral("signature")] = m.symbols[s].signature;
        rowList << row;
        w = std::max(w, textWidth(name) + 2 * kPad + 28);
        y += kRow;
    }
    w = std::clamp(w, kMinW, kMaxW + 80);
    center[QStringLiteral("symbol")] = -1;
    center[QStringLiteral("file")] = m_file;
    center[QStringLiteral("kind")] = -2;
    center[QStringLiteral("title")] = title;
    center[QStringLiteral("subtitle")] = tr("%1 lines").arg(f.lines);
    center[QStringLiteral("w")] = w;
    center[QStringLiteral("h")] = y + 6;
    center[QStringLiteral("center")] = true;
    center[QStringLiteral("rows")] = rowList;
    m_nodes << center;
    m_rects << QRectF(0, 0, w, y + 6);
    m_nodeSymbols << -1;

    auto fileNode = [&](qint32 file, int side) {
        const File &ff = m.files[file];
        QVariantMap n;
        const QString t = ff.project ? ff.relative : QFileInfo(ff.path).fileName();
        n[QStringLiteral("symbol")] = -1;
        n[QStringLiteral("file")] = file;
        n[QStringLiteral("kind")] = -2;
        n[QStringLiteral("title")] = t;
        n[QStringLiteral("subtitle")] = ff.project ? tr("%1 lines").arg(ff.lines) : QFileInfo(ff.path).absolutePath();
        n[QStringLiteral("external")] = !ff.project;
        n[QStringLiteral("side")] = side;
        n[QStringLiteral("rows")] = QVariantList();
        const double nw = std::clamp(textWidth(t, true) + 2 * kPad + 26, kMinW, kMaxW);
        n[QStringLiteral("w")] = nw;
        n[QStringLiteral("h")] = kHeader + 2;
        return n;
    };
    const double gapX = 170;
    auto column = [&](const QVector<qint32> &files, int side) {
        QVector<double> sizes, wants;
        for (qsizetype i = 0; i < files.size(); ++i) {
            sizes << kHeader + 2;
            wants << 0;
        }
        double total = files.size() * (kHeader + 2 + kNodeGap);
        double start = (y + 6) / 2 - total / 2;
        for (qsizetype i = 0; i < files.size(); ++i)
            wants[i] = start + i * (kHeader + 2 + kNodeGap);
        const QVector<double> pos = pack(wants, sizes, kNodeGap);
        for (qsizetype i = 0; i < files.size(); ++i) {
            QVariantMap n = fileNode(files[i], side);
            const double nw = n[QStringLiteral("w")].toDouble();
            const QRectF r = side == 0 ? QRectF(-gapX - nw, pos[i], nw, kHeader + 2) : QRectF(w + gapX, pos[i], nw, kHeader + 2);
            const int idx = int(m_nodes.size());
            m_nodes << n;
            m_rects << r;
            m_nodeSymbols << -1;
            SceneEdge e;
            e.kind = int(RefKind::Include);
            if (side == 0) {
                e.points = {QPointF(r.right(), r.center().y()), QPointF(0, kHeader / 2)};
                e.startPort = Right;
                e.endPort = Left;
                e.fromNode = idx;
                e.toNode = 0;
            } else {
                e.points = {QPointF(w, kHeader / 2), QPointF(r.left(), r.center().y())};
                e.startPort = Right;
                e.endPort = Left;
                e.fromNode = 0;
                e.toNode = idx;
            }
            m_edges << e;
        }
    };
    QVector<qint32> includers = f.includedBy;
    std::sort(includers.begin(), includers.end(), [&](qint32 a, qint32 b) { return m.files[a].relative < m.files[b].relative; });
    QVector<qint32> includes;
    for (const Include &inc : f.includes) {
        if (m_showExternal || m.files[inc.file].project)
            includes << inc.file;
    }
    if (includers.size() > 60)
        includers.resize(60);
    if (includes.size() > 60)
        includes.resize(60);
    column(includers, 0);
    column(includes, 1);
    m_summary = tr("%1 · included by %2 · includes %3").arg(f.relative).arg(f.includedBy.size()).arg(f.includes.size());
}

// --- CallGraphScene ---------------------------------------------------------------

CallGraphScene::CallGraphScene(QObject *parent)
    : GraphScene(parent)
{
    connect(this, &CallGraphScene::inputChanged, this, &GraphScene::scheduleBuild);
}

void CallGraphScene::setSymbol(int s)
{
    if (s == m_symbol)
        return;
    m_symbol = s;
    emit inputChanged();
}

void CallGraphScene::setTarget(int s)
{
    if (s == m_target)
        return;
    m_target = s;
    emit inputChanged();
}

void CallGraphScene::setDepth(int d)
{
    d = std::clamp(d, 1, 12);
    if (d == m_depth)
        return;
    m_depth = d;
    emit inputChanged();
}

void CallGraphScene::setCallees(bool v)
{
    if (v == m_callees)
        return;
    m_callees = v;
    emit inputChanged();
}

void CallGraphScene::setCallers(bool v)
{
    if (v == m_callers)
        return;
    m_callers = v;
    emit inputChanged();
}

void CallGraphScene::setShowExternal(bool v)
{
    if (v == m_showExternal)
        return;
    m_showExternal = v;
    emit inputChanged();
}

void CallGraphScene::setVertical(bool v)
{
    if (v == m_vertical)
        return;
    m_vertical = v;
    emit inputChanged();
}

void CallGraphScene::build()
{
    const CodeModel &m = *m_built;
    if (m_symbol < 0 || m_symbol >= m.symbols.size())
        return;
    qint32 root = m_symbol;
    // A class: start from its methods' calls through the class node
    if (!isFunction(m.symbols[root].kind)) {
        m_summary = tr("Pick a function to see its calls");
        return;
    }
    Analysis::Subgraph sub = m_target >= 0 ? Analysis::callPaths(m, root, m_target, std::max(m_depth, 8))
                                           : Analysis::callNeighbourhood(m, root, m_depth, m_callees, m_callers, 300);
    QHash<qint32, int> index;
    QVector<Layout::Node> lnodes;
    QVector<qint32> syms;
    for (qint32 s : std::as_const(sub.nodes)) {
        const Symbol &sym = m.symbols[s];
        if (m_hidden.contains(s) && s != root)
            continue;
        if (!m_showExternal && sym.has(External) && s != root && s != m_target)
            continue;
        index.insert(s, int(syms.size()));
        syms << s;
        Layout::Node n;
        const QString title = sym.name + QStringLiteral("()");
        const QString sub2 = subtitleOf(m, s);
        const double badge = sym.metrics.complexity >= 10 ? 44 : 0;
        n.w = std::clamp(std::max(textWidth(title, true) + badge, textWidth(sub2) * 0.92) + 2 * kPad + 28, 120.0, 360.0);
        n.h = 44;
        lnodes << n;
    }
    QVector<Layout::Link> links;
    QVector<int> linkEdge;
    for (qsizetype i = 0; i < sub.edges.size(); ++i) {
        const auto &[a, b] = sub.edges[i];
        if (!index.contains(a) || !index.contains(b))
            continue;
        Layout::Link l;
        l.from = index.value(a);
        l.to = index.value(b);
        links << l;
        linkEdge << int(i);
    }
    Layout::Options o;
    o.direction = m_vertical ? Layout::Direction::TopToBottom : Layout::Direction::LeftToRight;
    o.layerGap = m_vertical ? 70 : 110;
    o.nodeGap = m_vertical ? 24 : 14;
    Layout::layered(lnodes, links, o);
    for (qsizetype i = 0; i < syms.size(); ++i) {
        const qint32 s = syms[i];
        const Symbol &sym = m.symbols[s];
        QVariantMap n;
        n[QStringLiteral("symbol")] = s;
        n[QStringLiteral("file")] = -1;
        n[QStringLiteral("kind")] = int(sym.kind);
        n[QStringLiteral("title")] = sym.name + QStringLiteral("()");
        n[QStringLiteral("subtitle")] = subtitleOf(m, s);
        n[QStringLiteral("w")] = lnodes[i].w;
        n[QStringLiteral("h")] = lnodes[i].h;
        n[QStringLiteral("external")] = sym.has(External);
        n[QStringLiteral("center")] = s == root;
        n[QStringLiteral("target")] = s == m_target;
        n[QStringLiteral("virtual")] = sym.has(Virtual);
        n[QStringLiteral("complexity")] = sym.metrics.complexity;
        n[QStringLiteral("lines")] = sym.metrics.lines;
        n[QStringLiteral("unused")] = Analysis::looksUnused(m, s);
        n[QStringLiteral("rows")] = QVariantList();
        n[QStringLiteral("compact")] = true;
        m_nodes << n;
        m_rects << QRectF(lnodes[i].x, lnodes[i].y, lnodes[i].w, lnodes[i].h);
        m_nodeSymbols << s;
    }
    for (qsizetype i = 0; i < links.size(); ++i) {
        const Layout::Link &l = links[i];
        if (l.points.size() < 2)
            continue;
        SceneEdge e;
        e.points = l.points;
        e.kind = int(sub.dispatch[linkEdge[i]] ? RefKind::Override : RefKind::Call);
        e.dashed = sub.dispatch[linkEdge[i]];
        e.fromNode = l.from;
        e.toNode = l.to;
        e.fromSymbol = syms[l.from];
        e.toSymbol = syms[l.to];
        if (m_vertical) {
            e.startPort = l.reversed ? Top : Bottom;
            e.endPort = l.reversed ? Bottom : Top;
        } else {
            e.startPort = l.reversed ? Left : Right;
            e.endPort = l.reversed ? Right : Left;
        }
        // The model's edge for its references
        for (qint32 me : m.outEdges[e.fromSymbol]) {
            if (m.edges[me].to == e.toSymbol && m.edges[me].kind == RefKind::Call) {
                e.modelEdges << me;
                e.count = m.edges[me].count;
            }
        }
        e.cyclic = e.fromSymbol == e.toSymbol || l.reversed;
        m_edges << e;
    }
    if (m_target >= 0)
        m_summary = sub.nodes.isEmpty() ? tr("No call path from %1 to %2 within %3 calls")
                                              .arg(m.symbols[root].name, m.symbols[m_target].name).arg(std::max(m_depth, 8))
                                        : tr("Call paths from %1 to %2: %3 functions")
                                              .arg(m.symbols[root].name, m.symbols[m_target].name).arg(syms.size());
    else
        m_summary = tr("%1: %2 functions, %3 calls").arg(m.symbols[root].name).arg(syms.size()).arg(m_edges.size());
}

// --- DependencyScene ----------------------------------------------------------------

DependencyScene::DependencyScene(QObject *parent)
    : GraphScene(parent)
{
    connect(this, &DependencyScene::inputChanged, this, &GraphScene::scheduleBuild);
}

void DependencyScene::setLevel(int l)
{
    if (l == m_level)
        return;
    m_level = l;
    emit inputChanged();
}

void DependencyScene::setModuleDepth(int d)
{
    if (d == m_moduleDepth)
        return;
    m_moduleDepth = d;
    emit inputChanged();
}

void DependencyScene::setFilter(const QString &f)
{
    if (f == m_filter)
        return;
    m_filter = f;
    emit inputChanged();
}

void DependencyScene::setShowExternal(bool v)
{
    if (v == m_showExternal)
        return;
    m_showExternal = v;
    emit inputChanged();
}

void DependencyScene::setIncludesOnly(bool v)
{
    if (v == m_includesOnly)
        return;
    m_includesOnly = v;
    emit inputChanged();
}

void DependencyScene::setVertical(bool v)
{
    if (v == m_vertical)
        return;
    m_vertical = v;
    emit inputChanged();
}

void DependencyScene::setCyclesOnly(bool v)
{
    if (v == m_cyclesOnly)
        return;
    m_cyclesOnly = v;
    emit inputChanged();
}

void DependencyScene::setMaxNodes(int n)
{
    if (n == m_maxNodes)
        return;
    m_maxNodes = n;
    emit inputChanged();
}

QStringList DependencyScene::moduleNames() const
{
    auto m = m_built;
    if (!m)
        return {};
    QStringList out = m->modules;
    out.removeAll(QString());
    return out;
}

QVariantList DependencyScene::linkReferences(int edge, int limit) const
{
    QVariantList out;
    auto m = m_built;
    if (!m || edge < 0 || edge >= m_edges.size())
        return out;
    const QVector<qint32> refs = Analysis::linkReferences(*m, m_graph, m_edges[edge].link, limit);
    Session *s = Session::instance();
    for (qint32 r : refs)
        out << s->referenceInfo(r);
    return out;
}

void DependencyScene::build()
{
    const CodeModel &m = *m_built;
    const auto level = Analysis::Level(std::clamp(m_level, 0, 2));
    m_graph = Analysis::dependencyGraph(m, level, m_moduleDepth, m_showExternal, m_filter,
                                        Session::instance()->foldedFolders());
    const Analysis::DepGraph &g = m_graph;
    const qint32 n = qint32(g.names.size());

    // Which nodes: cycles only, then the biggest
    QVector<int> keep;
    for (qint32 i = 0; i < n; ++i) {
        if (m_cyclesOnly && g.componentSize[g.component[i]] < 2)
            continue;
        if (m_hidden.contains(level == Analysis::Level::Class ? g.ids[i] : -1000 - i))
            continue;
        keep << i;
    }
    if (keep.size() > m_maxNodes) {
        std::sort(keep.begin(), keep.end(), [&](int a, int b) {
            return g.size[a] + 20 * (g.out[a].size() + g.in[a].size()) > g.size[b] + 20 * (g.out[b].size() + g.in[b].size());
        });
        keep.resize(m_maxNodes);
    }
    std::sort(keep.begin(), keep.end());
    QHash<int, int> index;
    QVector<Layout::Node> lnodes;
    qint32 maxSize = 1;
    for (int i : std::as_const(keep))
        maxSize = std::max(maxSize, g.size[i]);
    for (int i : std::as_const(keep)) {
        index.insert(i, int(lnodes.size()));
        Layout::Node ln;
        QString title = g.names[i];
        if (!g.folder[i].isEmpty())
            title = QFileInfo(g.folder[i]).fileName() + QStringLiteral("/");
        else if (level == Analysis::Level::File)
            title = QFileInfo(g.names[i]).fileName();
        else if (level == Analysis::Level::Class)
            title = m.symbols[g.ids[i]].name;
        const QString metric = !g.folder[i].isEmpty() ? tr("folded · %1 lines").arg(g.size[i])
                             : level == Analysis::Level::Class ? tr("%1 members · cc %2").arg(g.size[i]).arg(g.complexity[i])
                                                               : tr("%1 lines · cc %2").arg(g.size[i]).arg(g.complexity[i]);
        ln.w = std::clamp(std::max(textWidth(title, true) + 26, textWidth(metric) * 0.95) + 2 * kPad, 110.0, 320.0);
        ln.h = 44;
        lnodes << ln;
    }
    QVector<Layout::Link> links;
    QVector<int> linkOf;
    for (qint32 li = 0; li < g.links.size(); ++li) {
        const Analysis::DepGraph::Link &l = g.links[li];
        if (!index.contains(l.from) || !index.contains(l.to))
            continue;
        if (m_includesOnly && l.includes == 0)
            continue;
        if (!m_includesOnly && l.count == 0 && level == Analysis::Level::Class)
            continue;
        Layout::Link ll;
        ll.from = index.value(l.from);
        ll.to = index.value(l.to);
        ll.weight = l.count + l.includes;
        links << ll;
        linkOf << li;
    }
    Layout::Options o;
    o.direction = m_vertical ? Layout::Direction::TopToBottom : Layout::Direction::LeftToRight;
    o.layerGap = m_vertical ? 80 : 130;
    o.nodeGap = m_vertical ? 26 : 16;
    Layout::layered(lnodes, links, o);
    QSet<int> cyclicComponents;
    for (int i : std::as_const(keep)) {
        const int k = index.value(i);
        QVariantMap nm;
        QString title = g.names[i], subtitle;
        nm[QStringLiteral("symbol")] = -1;
        nm[QStringLiteral("file")] = -1;
        nm[QStringLiteral("kind")] = -3;
        if (!g.folder[i].isEmpty()) {
            // A folded folder: one node for all of it
            title = QFileInfo(g.folder[i]).fileName() + QStringLiteral("/");
            subtitle = g.folder[i];
            nm[QStringLiteral("kind")] = -4;
            nm[QStringLiteral("folder")] = g.folder[i];
        } else if (level == Analysis::Level::File) {
            title = QFileInfo(g.names[i]).fileName();
            subtitle = QFileInfo(g.names[i]).path();
            if (subtitle == QLatin1String("."))
                subtitle.clear();
            nm[QStringLiteral("file")] = g.ids[i];
            nm[QStringLiteral("kind")] = -2;
        } else if (level == Analysis::Level::Class) {
            const Symbol &s = m.symbols[g.ids[i]];
            title = s.name;
            subtitle = s.parent >= 0 ? m.displayName(s.parent) : QString();
            nm[QStringLiteral("symbol")] = g.ids[i];
            nm[QStringLiteral("kind")] = int(s.kind);
            nm[QStringLiteral("external")] = s.has(External);
        } else {
            nm[QStringLiteral("module")] = g.names[i] == QLatin1String("(root)") ? QString() : g.names[i];
            nm[QStringLiteral("external")] = g.names[i].startsWith(QLatin1Char('<'));
        }
        const bool cyc = g.componentSize[g.component[i]] > 1;
        if (cyc)
            cyclicComponents.insert(g.component[i]);
        nm[QStringLiteral("title")] = title;
        nm[QStringLiteral("subtitle")] = subtitle;
        nm[QStringLiteral("metric")] = !g.folder[i].isEmpty() && level == Analysis::Level::Class
                                           ? tr("folded · %n classes", nullptr, g.size[i])
                                       : !g.folder[i].isEmpty() ? tr("folded · %1 lines").arg(g.size[i])
                                       : level == Analysis::Level::Class
                                           ? tr("%1 members · cc %2").arg(g.size[i]).arg(g.complexity[i])
                                           : tr("%1 lines · cc %2").arg(g.size[i]).arg(g.complexity[i]);
        nm[QStringLiteral("w")] = lnodes[k].w;
        nm[QStringLiteral("h")] = lnodes[k].h;
        nm[QStringLiteral("cyclic")] = cyc;
        nm[QStringLiteral("sizeFraction")] = std::sqrt(double(g.size[i]) / maxSize);
        nm[QStringLiteral("rows")] = QVariantList();
        nm[QStringLiteral("compact")] = true;
        nm[QStringLiteral("graphNode")] = i;
        m_nodes << nm;
        m_rects << QRectF(lnodes[k].x, lnodes[k].y, lnodes[k].w, lnodes[k].h);
        m_nodeSymbols << (level == Analysis::Level::Class ? g.ids[i] : -1);
    }
    for (qsizetype i = 0; i < links.size(); ++i) {
        const Layout::Link &l = links[i];
        if (l.points.size() < 2)
            continue;
        const Analysis::DepGraph::Link &gl = g.links[linkOf[i]];
        SceneEdge e;
        e.points = l.points;
        e.link = linkOf[i];
        e.count = m_includesOnly ? gl.includes : gl.count + gl.includes;
        int best = int(RefKind::Include), bestCount = gl.includes;
        if (!m_includesOnly) {
            for (int k = 0; k < int(RefKind::Count); ++k) {
                if (gl.kinds[k] > bestCount) {
                    best = k;
                    bestCount = gl.kinds[k];
                }
            }
        }
        e.kind = best;
        e.cyclic = gl.cyclic;
        e.bothWays = g.linkIndex(gl.to, gl.from) >= 0;
        e.fromNode = l.from;
        e.toNode = l.to;
        e.fromSymbol = level == Analysis::Level::Class ? g.ids[gl.from] : -1;
        e.toSymbol = level == Analysis::Level::Class ? g.ids[gl.to] : -1;
        if (m_vertical) {
            e.startPort = l.reversed ? Top : Bottom;
            e.endPort = l.reversed ? Bottom : Top;
        } else {
            e.startPort = l.reversed ? Left : Right;
            e.endPort = l.reversed ? Right : Left;
        }
        m_edges << e;
    }
    m_cycleCount = int(cyclicComponents.size());
    const QString what = level == Analysis::Level::Module ? tr("modules") : level == Analysis::Level::File ? tr("files") : tr("classes");
    m_summary = tr("%1 %2, %3 dependencies").arg(keep.size()).arg(what).arg(m_edges.size());
    if (keep.size() < n)
        m_summary += tr(" (the %1 largest of %2)").arg(keep.size()).arg(n);
    if (m_cycleCount)
        m_summary += tr(" · %n cycles", nullptr, m_cycleCount);
}

// --- ChangeScene ---------------------------------------------------------------------

ChangeScene::ChangeScene(QObject *parent)
    : GraphScene(parent)
{
    connect(this, &ChangeScene::inputChanged, this, &GraphScene::scheduleBuild);
    connect(Changes::instance(), &Changes::changesChanged, this, &GraphScene::scheduleBuild);
}

void ChangeScene::setContext(int c)
{
    if (c == m_context)
        return;
    m_context = c;
    emit inputChanged();
}

void ChangeScene::setVertical(bool v)
{
    if (v == m_vertical)
        return;
    m_vertical = v;
    emit inputChanged();
}

void ChangeScene::setMaxContext(int n)
{
    if (n == m_maxContext)
        return;
    m_maxContext = n;
    emit inputChanged();
}

void ChangeScene::build()
{
    const CodeModel &m = *m_built;
    const auto &touched = Changes::instance()->touched();
    if (touched.isEmpty()) {
        m_summary = Changes::instance()->selected().isEmpty() ? tr("Pick commits to see the code they changed")
                                                              : tr("The selected commits changed no indexed code");
        return;
    }
    auto noise = [&](const Edge &e, qint32 other) {
        const Symbol &o = m.symbols[other];
        if (e.kind == RefKind::Declaration || o.has(External) || o.has(FromMacro) || o.kind == SymbolKind::Namespace)
            return true;
        return (e.flags & RefImplicit) && e.kind == RefKind::Call
            && (o.kind == SymbolKind::Constructor || o.kind == SymbolKind::Destructor);
    };

    // The changed nodes (classes, free functions…) and their changed members
    struct NodeData {
        qint32 owner = -1;
        bool changed = false;     // part of the change, or context
        bool ownerTouched = false;
        QVector<qint32> rows;
        QSet<qint32> rowSet;
        double weight = 0;
    };
    QHash<qint32, NodeData> nodes;
    for (auto it = touched.cbegin(); it != touched.cend(); ++it) {
        const qint32 s = it.key();
        const qint32 o = m.nodeOwner(s);
        if (o < 0 || m_hidden.contains(o))
            continue;
        NodeData &n = nodes[o];
        n.owner = o;
        n.changed = true;
        if (o == s) {
            n.ownerTouched = true;
        } else if (!n.rowSet.contains(s)) {
            n.rowSet.insert(s);
            n.rows.append(s);
        }
    }
    // The context: what uses the changed code, and what it uses
    auto addContext = [&](qint32 other) {
        const qint32 o = m.nodeOwner(other);
        if (o < 0 || m_hidden.contains(o))
            return;
        NodeData &n = nodes[o];
        n.owner = o;
        n.weight += 1;
        if (other != o && !n.rowSet.contains(other)) {
            n.rowSet.insert(other);
            n.rows.append(other);
        }
    };
    if (m_context != ChangedOnly) {
        QHash<qint32, double> weight;
        QVector<QPair<qint32, qint32>> candidates;   // (owner, symbol)
        for (auto it = touched.cbegin(); it != touched.cend(); ++it) {
            for (qint32 e : m.inEdges[it.key()]) {
                const qint32 u = m.edges[e].from;
                if (!touched.contains(u) && !noise(m.edges[e], u))
                    candidates.append({m.nodeOwner(u), u});
            }
            if (m_context == WithUsersAndUses) {
                for (qint32 e : m.outEdges[it.key()]) {
                    const qint32 v = m.edges[e].to;
                    if (!touched.contains(v) && !noise(m.edges[e], v))
                        candidates.append({m.nodeOwner(v), v});
                }
            }
        }
        for (const auto &[o, s] : std::as_const(candidates))
            weight[o] += 1;
        // The strongest new nodes; members of changed ones always
        QVector<qint32> owners;
        for (auto it = weight.cbegin(); it != weight.cend(); ++it) {
            if (!nodes.contains(it.key()))
                owners.append(it.key());
        }
        std::sort(owners.begin(), owners.end(), [&](qint32 a, qint32 b) { return weight[a] > weight[b]; });
        const QSet<qint32> allowed(owners.cbegin(), owners.cbegin() + std::min<qsizetype>(m_maxContext, owners.size()));
        int hiddenContext = int(owners.size()) - int(allowed.size());
        // Unchanged members of a changed class only when everything around
        // is asked for: else they bury the change
        for (const auto &[o, s] : std::as_const(candidates)) {
            const bool changedNode = nodes.contains(o) && nodes[o].changed;
            if ((changedNode && m_context == WithUsersAndUses) || (!changedNode && (nodes.contains(o) || allowed.contains(o))))
                addContext(s);
        }
        if (hiddenContext > 0)
            m_summary = tr("%n more users not shown", nullptr, hiddenContext);
    }

    // Cards: changed members first in the order of the code, then the context rows
    QVector<qint32> order = nodes.keys().toVector();
    std::sort(order.begin(), order.end(), [&](qint32 a, qint32 b) {
        if (nodes[a].changed != nodes[b].changed)
            return nodes[a].changed;
        return m.displayName(a) < m.displayName(b);
    });
    QHash<qint32, int> nodeIndex;
    QHash<qint32, QPair<int, double>> rowAt;   // symbol -> (node, y in it)
    QVector<Layout::Node> lnodes;
    for (qint32 o : std::as_const(order)) {
        NodeData &n = nodes[o];
        std::sort(n.rows.begin(), n.rows.end(), [&](qint32 a, qint32 b) {
            const bool ta = touched.contains(a), tb = touched.contains(b);
            if (ta != tb)
                return ta;
            return m.symbols[a].where().line < m.symbols[b].where().line;
        });
        const Symbol &sym = m.symbols[o];
        const QString title = m.displayName(o) + (isFunction(sym.kind) ? QStringLiteral("()") : QString());
        // Room for the icon and the "±n" of changed lines beside the title
        double w = textWidth(title, true) + 2 * kPad + 30 + (n.changed ? textWidth(QStringLiteral("±0000")) : 0);
        double y = kHeader;
        QVariantList rows;
        int changedRows = 0, changedLines = touched.value(o).lines;
        for (qint32 r : std::as_const(n.rows)) {
            const Symbol &rs = m.symbols[r];
            const bool changed = touched.contains(r);
            QVariantMap row;
            const QString name = memberName(m, r, o);
            row[QStringLiteral("symbol")] = r;
            row[QStringLiteral("name")] = name;
            row[QStringLiteral("kind")] = int(rs.kind);
            row[QStringLiteral("y")] = y;
            row[QStringLiteral("h")] = kRow;
            row[QStringLiteral("changed")] = changed;
            row[QStringLiteral("added")] = changed && touched.value(r).added;
            row[QStringLiteral("lines")] = changed ? touched.value(r).lines : 0;
            row[QStringLiteral("virtual")] = rs.has(Virtual);
            row[QStringLiteral("signature")] = rs.signature;
            rows << row;
            rowAt.insert(r, {int(m_nodes.size()), y + kRow / 2});
            w = std::max(w, textWidth(name) + 2 * kPad + 56);
            y += kRow;
            if (changed) {
                ++changedRows;
                changedLines += touched.value(r).lines;
            }
        }
        if (!n.rows.isEmpty())
            y += 6;
        w = std::clamp(w, kMinW, kMaxW + 40);
        QVariantMap node;
        node[QStringLiteral("symbol")] = o;
        node[QStringLiteral("file")] = -1;
        node[QStringLiteral("kind")] = int(sym.kind);
        node[QStringLiteral("title")] = title;
        node[QStringLiteral("subtitle")] = subtitleOf(m, o);
        node[QStringLiteral("w")] = w;
        node[QStringLiteral("h")] = std::max(y, kHeader + 2);
        node[QStringLiteral("rows")] = rows;
        node[QStringLiteral("changed")] = n.changed;
        node[QStringLiteral("context")] = !n.changed;
        node[QStringLiteral("added")] = n.ownerTouched && touched.value(o).added;
        node[QStringLiteral("changedLines")] = changedLines;
        node[QStringLiteral("external")] = sym.has(External);
        nodeIndex.insert(o, int(m_nodes.size()));
        rowAt.insert(o, {int(m_nodes.size()), kHeader / 2});
        m_nodes << node;
        m_nodeSymbols << o;
        Layout::Node ln;
        ln.w = w;
        ln.h = node[QStringLiteral("h")].toDouble();
        lnodes << ln;
    }

    // Edges between what is shown, one per pair of rows and kind
    struct Item {
        qint32 from, to;
        int kind;
        int count;
        qint32 modelEdge;
    };
    QVector<Item> items;
    QSet<quint64> seen;
    auto consider = [&](qint32 e) {
        const Edge &edge = m.edges[e];
        if (edge.kind == RefKind::Declaration || edge.from == edge.to)
            return;
        if (!rowAt.contains(edge.from) || !rowAt.contains(edge.to))
            return;
        if (!touched.contains(edge.from) && !touched.contains(edge.to))
            return;   // context with context
        const quint64 k = (quint64(quint32(e)));
        if (seen.contains(k))
            return;
        seen.insert(k);
        items.append({edge.from, edge.to, int(edge.kind), edge.count, e});
    };
    for (auto it = touched.cbegin(); it != touched.cend(); ++it) {
        for (qint32 e : m.outEdges[it.key()])
            consider(e);
        for (qint32 e : m.inEdges[it.key()])
            consider(e);
    }
    QVector<Layout::Link> links;
    QHash<quint64, int> linkOf;
    for (const Item &it : std::as_const(items)) {
        const int a = rowAt.value(it.from).first, b = rowAt.value(it.to).first;
        if (a == b)
            continue;
        const quint64 k = (quint64(quint32(a)) << 32) | quint32(b);
        if (!linkOf.contains(k)) {
            linkOf.insert(k, int(links.size()));
            Layout::Link l;
            l.from = a;
            l.to = b;
            links << l;
        }
    }
    Layout::Options o;
    o.direction = m_vertical ? Layout::Direction::TopToBottom : Layout::Direction::LeftToRight;
    o.layerGap = m_vertical ? 80 : 150;
    o.nodeGap = m_vertical ? 26 : 18;
    Layout::layered(lnodes, links, o);
    for (int i = 0; i < lnodes.size(); ++i)
        m_rects << QRectF(lnodes[i].x, lnodes[i].y, lnodes[i].w, lnodes[i].h);

    for (const Item &it : std::as_const(items)) {
        const auto [a, ya] = rowAt.value(it.from);
        const auto [b, yb] = rowAt.value(it.to);
        SceneEdge e;
        e.kind = it.kind;
        e.count = it.count;
        e.modelEdges << it.modelEdge;
        e.dashed = it.kind == int(RefKind::Override);
        e.fromNode = a;
        e.toNode = b;
        e.fromSymbol = it.from;
        e.toSymbol = it.to;
        const QRectF ra = m_rects[a], rb = m_rects[b];
        if (a == b) {
            // Within one class: a bracket on its right
            e.points = {QPointF(ra.right(), ra.top() + ya), QPointF(ra.right(), ra.top() + yb)};
            e.startPort = e.endPort = Right;
            m_edges << e;
            continue;
        }
        const Layout::Link &l = links[linkOf.value((quint64(quint32(a)) << 32) | quint32(b))];
        const bool backwards = m_vertical ? rb.center().y() < ra.center().y() : rb.center().x() < ra.center().x();
        if (m_vertical) {
            e.points = {QPointF(ra.center().x(), backwards ? ra.top() : ra.bottom()),
                        QPointF(rb.center().x(), backwards ? rb.bottom() : rb.top())};
            e.startPort = backwards ? Top : Bottom;
            e.endPort = backwards ? Bottom : Top;
        } else {
            e.points = {QPointF(backwards ? ra.left() : ra.right(), ra.top() + ya),
                        QPointF(backwards ? rb.right() : rb.left(), rb.top() + yb)};
            e.startPort = backwards ? Left : Right;
            e.endPort = backwards ? Right : Left;
        }
        // Through the layout's bends for long edges
        if (l.points.size() > 2) {
            QVector<QPointF> pts = l.points;
            pts.first() = e.points.first();
            pts.last() = e.points.last();
            e.points = pts;
        }
        m_edges << e;
    }
    int changedNodes = 0;
    for (const NodeData &n : std::as_const(nodes))
        changedNodes += n.changed;
    const QString more = m_summary;
    m_summary = tr("%n changed symbols", nullptr, int(touched.size())) + QStringLiteral(" in ")
              + tr("%n classes and functions", nullptr, changedNodes);
    if (int(nodes.size()) > changedNodes)
        m_summary += QStringLiteral(" · ") + tr("%n around them", nullptr, int(nodes.size()) - changedNodes);
    if (!more.isEmpty())
        m_summary += QStringLiteral(" · ") + more;
}
