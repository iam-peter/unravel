#pragma once

#include <QFont>
#include <QObject>
#include <QPainterPath>
#include <QPointF>
#include <QSet>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "analysis.h"
#include "model.h"

// A laid-out graph for the canvas views: nodes (with member rows) that
// QML draws as cards, and routed edges that an EdgeLayer paints. Each
// subclass makes one kind of graph from the code model; all of them
// redo the layout when their inputs or the model change.
class GraphScene : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("A base class")

    Q_PROPERTY(QVariantList nodes READ nodes NOTIFY changed)
    Q_PROPERTY(qreal sceneWidth READ sceneWidth NOTIFY changed)
    Q_PROPERTY(qreal sceneHeight READ sceneHeight NOTIFY changed)
    Q_PROPERTY(int edgeCount READ edgeCount NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged)
    Q_PROPERTY(int highlightNode READ highlightNode WRITE setHighlightNode NOTIFY highlightChanged)
    Q_PROPERTY(int highlightSymbol READ highlightSymbol WRITE setHighlightSymbol NOTIFY highlightChanged)
    Q_PROPERTY(int selectedEdge READ selectedEdge WRITE setSelectedEdge NOTIFY highlightChanged)
    Q_PROPERTY(QVariantList hiddenSymbols READ hiddenSymbols NOTIFY changed)
    Q_PROPERTY(QVariantList groups READ groups NOTIFY changed)
    // Only an active scene lays itself out; an inactive one remembers to
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(int edgeStyle READ edgeStyle WRITE setEdgeStyle NOTIFY edgeStyleChanged)

public:
    enum Port { Auto, Left, Right, Top, Bottom };

    struct SceneEdge {
        QVector<QPointF> points;
        Port startPort = Right, endPort = Left;
        int kind = 0;            // Unravel::RefKind
        int count = 1;
        int fromNode = -1, toNode = -1;
        int fromSymbol = -1, toSymbol = -1;
        QVector<qint32> modelEdges; // indexes into CodeModel::edges
        int link = -1;           // DepGraph link, for dependency scenes
        bool cyclic = false;
        bool dashed = false;
        bool bothWays = false;
        QPainterPath path;       // scene coordinates
        QPointF tip, tipFrom;    // the arrow: its point and where it comes from
        QPolygonF poly;          // the path flattened: for the GPU and for pointing
        QRectF hitBounds;
        mutable QPainterPath hit; // the stroke to point at, made when first needed
    };

    explicit GraphScene(QObject *parent = nullptr);

    QVariantList nodes() const { return m_nodes; }
    qreal sceneWidth() const { return m_width; }
    qreal sceneHeight() const { return m_height; }
    int edgeCount() const { return int(m_edges.size()); }
    QString summary() const { return m_summary; }
    QFont font() const { return m_font; }
    void setFont(const QFont &f);
    int highlightNode() const { return m_highlightNode; }
    void setHighlightNode(int n);
    int highlightSymbol() const { return m_highlightSymbol; }
    void setHighlightSymbol(int s);
    int selectedEdge() const { return m_selectedEdge; }
    void setSelectedEdge(int e);
    QVariantList hiddenSymbols() const;
    QVariantList groups() const { return m_groups; }
    bool active() const { return m_active; }
    void setActive(bool a);

    enum EdgeStyle { Curved, Straight, Orthogonal };
    Q_ENUM(EdgeStyle)
    // The line of an edge through its points, leaving and entering by its ports
    static QPainterPath edgePath(const SceneEdge &e, int style = Curved);
    int edgeStyle() const { return m_edgeStyle; }
    void setEdgeStyle(int style);

    QVector<SceneEdge> &edges() { return m_edges; }
    const QVector<SceneEdge> &edges() const { return m_edges; }
    int revision() const { return m_revision; }

    // Whether an edge touches the highlighted node or symbol
    bool edgeHighlighted(int e) const;
    bool hasHighlight() const { return m_highlightNode >= 0 || m_highlightSymbol >= 0 || m_selectedEdge >= 0; }

    Q_INVOKABLE QVariantMap edgeInfo(int e) const;
    // The references behind an edge, for the code view
    Q_INVOKABLE QVariantList edgeReferences(int e, int limit = 200) const;
    Q_INVOKABLE QRectF nodeRect(int node) const;
    Q_INVOKABLE int nodeOfSymbol(int symbol) const;
    Q_INVOKABLE void hide(int symbol);
    Q_INVOKABLE void unhideAll();
    Q_INVOKABLE void rebuild();
    Q_INVOKABLE QString toSvg(const QVariantMap &colors) const;

signals:
    void changed();
    void fontChanged();
    void highlightChanged();
    void activeChanged();
    void edgeStyleChanged();
    void pathsChanged();   // the same layout, edges drawn another way

public slots:
    void scheduleBuild();

protected:
    virtual void build() = 0;
    void finish();   // computes the scene's size, emits changed

    std::shared_ptr<const Unravel::CodeModel> model() const;
    double textWidth(const QString &text, bool bold = false) const;

    QVariantList m_nodes;
    QVariantList m_groups;
    QVector<QRectF> m_rects;
    QVector<int> m_nodeSymbols;   // per node, the symbol it is (or -1)
    QVector<SceneEdge> m_edges;
    QSet<int> m_hidden;
    qreal m_width = 0, m_height = 0;
    QString m_summary;
    QFont m_font;
    int m_highlightNode = -1;
    int m_highlightSymbol = -1;
    int m_selectedEdge = -1;
    int m_revision = 0;
    bool m_buildPending = false;
    bool m_active = true;
    int m_edgeStyle = Curved;
    void makePaths();
    bool m_dirty = false;
    std::shared_ptr<const Unravel::CodeModel> m_built;   // the model the scene was made from
};

// Sourcetrail's view: the active symbol in the middle with its members,
// what uses it on the left, what it uses on the right, base classes above
// and subclasses below.
class SymbolGraph : public GraphScene
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int symbol READ symbol WRITE setSymbol NOTIFY inputChanged)
    Q_PROPERTY(int file READ file WRITE setFile NOTIFY inputChanged)
    Q_PROPERTY(bool allMembers READ allMembers WRITE setAllMembers NOTIFY inputChanged)
    Q_PROPERTY(int grouping READ grouping WRITE setGrouping NOTIFY inputChanged)
    Q_PROPERTY(int maxPerSide READ maxPerSide WRITE setMaxPerSide NOTIFY inputChanged)
    Q_PROPERTY(bool showExternal READ showExternal WRITE setShowExternal NOTIFY inputChanged)
    Q_PROPERTY(QVariantList kindFilter READ kindFilter WRITE setKindFilter NOTIFY inputChanged)

public:
    enum Grouping { NoGrouping, ByNamespace, ByFile };
    Q_ENUM(Grouping)

    explicit SymbolGraph(QObject *parent = nullptr);

    int symbol() const { return m_symbol; }
    void setSymbol(int s);
    int file() const { return m_file; }
    void setFile(int f);
    bool allMembers() const { return m_allMembers; }
    void setAllMembers(bool v);
    int grouping() const { return m_grouping; }
    void setGrouping(int g);
    int maxPerSide() const { return m_maxPerSide; }
    void setMaxPerSide(int n);
    bool showExternal() const { return m_showExternal; }
    void setShowExternal(bool v);
    QVariantList kindFilter() const;
    void setKindFilter(const QVariantList &kinds);
    Q_INVOKABLE void expandSide(int side);

signals:
    void inputChanged();

protected:
    void build() override;

private:
    void buildForFile();
    int m_symbol = -1;
    int m_file = -1;
    bool m_allMembers = false;
    int m_grouping = NoGrouping;
    int m_maxPerSide = 24;
    bool m_showExternal = true;
    QSet<int> m_kinds;   // RefKinds shown; empty = all
    QSet<int> m_expandedSides;
};

// Calls as a layered graph: what a function calls and who calls it, to a
// depth, or every path from one function to another.
class CallGraphScene : public GraphScene
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int symbol READ symbol WRITE setSymbol NOTIFY inputChanged)
    Q_PROPERTY(int target READ target WRITE setTarget NOTIFY inputChanged)
    Q_PROPERTY(int depth READ depth WRITE setDepth NOTIFY inputChanged)
    Q_PROPERTY(bool callees READ callees WRITE setCallees NOTIFY inputChanged)
    Q_PROPERTY(bool callers READ callers WRITE setCallers NOTIFY inputChanged)
    Q_PROPERTY(bool showExternal READ showExternal WRITE setShowExternal NOTIFY inputChanged)
    Q_PROPERTY(bool vertical READ vertical WRITE setVertical NOTIFY inputChanged)

public:
    explicit CallGraphScene(QObject *parent = nullptr);

    int symbol() const { return m_symbol; }
    void setSymbol(int s);
    int target() const { return m_target; }
    void setTarget(int s);
    int depth() const { return m_depth; }
    void setDepth(int d);
    bool callees() const { return m_callees; }
    void setCallees(bool v);
    bool callers() const { return m_callers; }
    void setCallers(bool v);
    bool showExternal() const { return m_showExternal; }
    void setShowExternal(bool v);
    bool vertical() const { return m_vertical; }
    void setVertical(bool v);

signals:
    void inputChanged();

protected:
    void build() override;

private:
    int m_symbol = -1;
    int m_target = -1;
    int m_depth = 3;
    bool m_callees = true;
    bool m_callers = false;
    bool m_showExternal = false;
    bool m_vertical = false;
};

// Modules, files or classes and what depends on what, layered so that
// the dependencies sit below (or right of) what uses them; cycles in red.
class DependencyScene : public GraphScene
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int level READ level WRITE setLevel NOTIFY inputChanged)
    Q_PROPERTY(int moduleDepth READ moduleDepth WRITE setModuleDepth NOTIFY inputChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY inputChanged)
    Q_PROPERTY(bool showExternal READ showExternal WRITE setShowExternal NOTIFY inputChanged)
    Q_PROPERTY(bool includesOnly READ includesOnly WRITE setIncludesOnly NOTIFY inputChanged)
    Q_PROPERTY(bool vertical READ vertical WRITE setVertical NOTIFY inputChanged)
    Q_PROPERTY(bool cyclesOnly READ cyclesOnly WRITE setCyclesOnly NOTIFY inputChanged)
    Q_PROPERTY(int maxNodes READ maxNodes WRITE setMaxNodes NOTIFY inputChanged)
    Q_PROPERTY(QStringList moduleNames READ moduleNames NOTIFY changed)
    Q_PROPERTY(int cycleCount READ cycleCount NOTIFY changed)

public:
    explicit DependencyScene(QObject *parent = nullptr);

    int level() const { return m_level; }
    void setLevel(int l);
    int moduleDepth() const { return m_moduleDepth; }
    void setModuleDepth(int d);
    QString filter() const { return m_filter; }
    void setFilter(const QString &f);
    bool showExternal() const { return m_showExternal; }
    void setShowExternal(bool v);
    bool includesOnly() const { return m_includesOnly; }
    void setIncludesOnly(bool v);
    bool vertical() const { return m_vertical; }
    void setVertical(bool v);
    bool cyclesOnly() const { return m_cyclesOnly; }
    void setCyclesOnly(bool v);
    int maxNodes() const { return m_maxNodes; }
    void setMaxNodes(int n);
    QStringList moduleNames() const;
    int cycleCount() const { return m_cycleCount; }

    const Unravel::Analysis::DepGraph &graph() const { return m_graph; }
    Q_INVOKABLE QVariantList linkReferences(int edge, int limit = 300) const;

signals:
    void inputChanged();

protected:
    void build() override;

private:
    int m_level = 0;
    int m_moduleDepth = 0;
    QString m_filter;
    bool m_showExternal = false;
    bool m_includesOnly = false;
    bool m_vertical = true;
    bool m_cyclesOnly = false;
    int m_maxNodes = 150;
    int m_cycleCount = 0;
    Unravel::Analysis::DepGraph m_graph;
};

// Only the code that selected commits changed: the classes and functions
// with their changed members, how they use each other, and if asked what
// uses them (the change's reach) and what they use
class ChangeScene : public GraphScene
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int context READ context WRITE setContext NOTIFY inputChanged)
    Q_PROPERTY(bool vertical READ vertical WRITE setVertical NOTIFY inputChanged)
    Q_PROPERTY(int maxContext READ maxContext WRITE setMaxContext NOTIFY inputChanged)

public:
    enum Context { ChangedOnly, WithUsers, WithUsersAndUses };
    Q_ENUM(Context)

    explicit ChangeScene(QObject *parent = nullptr);

    int context() const { return m_context; }
    void setContext(int c);
    bool vertical() const { return m_vertical; }
    void setVertical(bool v);
    int maxContext() const { return m_maxContext; }
    void setMaxContext(int n);

signals:
    void inputChanged();

protected:
    void build() override;

private:
    int m_context = WithUsers;
    bool m_vertical = false;
    int m_maxContext = 40;
};
