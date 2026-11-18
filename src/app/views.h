#pragma once

#include <QColor>
#include <QImage>
#include <QQuickItem>
#include <QPainterPath>
#include <QSet>
#include <QPointer>
#include <QSGNode>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "analysis.h"
#include "graphscene.h"
#include "model.h"

// The views that are painted rather than made of items: the edges of a
// graph scene, hierarchical edge bundles, the treemap and the dependency
// matrix. Colours come from QML (the theme) as a map.

// The edges of a GraphScene on the GPU: triangles in a few vertex buffers,
// one per colour, made when the layout or the edge style changes. Panning
// and zooming only move a transform; the highlighted edges are a small
// overlay over the dimmed rest.
class EdgeLayer : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(GraphScene *scene READ scene WRITE setScene NOTIFY sceneChanged)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY viewChanged)
    Q_PROPERTY(qreal panX READ panX WRITE setPanX NOTIFY viewChanged)
    Q_PROPERTY(qreal panY READ panY WRITE setPanY NOTIFY viewChanged)
    Q_PROPERTY(QVariantMap colors READ colors WRITE setColors NOTIFY viewChanged)
    Q_PROPERTY(int hoveredEdge READ hoveredEdge WRITE setHoveredEdge NOTIFY viewChanged)

public:
    explicit EdgeLayer(QQuickItem *parent = nullptr);

    GraphScene *scene() const { return m_scene; }
    void setScene(GraphScene *s);
    qreal zoom() const { return m_zoom; }
    void setZoom(qreal z);
    qreal panX() const { return m_panX; }
    void setPanX(qreal v);
    qreal panY() const { return m_panY; }
    void setPanY(qreal v);
    QVariantMap colors() const { return m_colors; }
    void setColors(const QVariantMap &c);
    int hoveredEdge() const { return m_hoveredEdge; }
    void setHoveredEdge(int e);

    // The edge under a point of the viewport, or -1
    Q_INVOKABLE int edgeAt(qreal x, qreal y) const;

signals:
    void sceneChanged();
    void viewChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;

private:
    QColor colorFor(int kind, bool cyclic) const;
    void rebuildAll();
    QPointer<GraphScene> m_scene;
    qreal m_zoom = 1, m_panX = 0, m_panY = 0;
    QVariantMap m_colors;
    int m_hoveredEdge = -1;
    bool m_baseDirty = true, m_overlayDirty = true;
    qreal m_builtZoom = 1;   // lines are at least a pixel wide at this zoom
    // Render-thread side, valid between updatePaintNode calls
    QSGNode *m_base = nullptr, *m_overlay = nullptr;
    QVector<QPair<class QSGGeometryNode *, QColor>> m_baseNodes;
};

// Holten's hierarchical edge bundles: the code's hierarchy (folders,
// files, classes) on a circle, dependencies as curves bundled along it
class BundleView : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal bundling READ bundling WRITE setBundling NOTIFY optionsChanged)
    Q_PROPERTY(int leafLevel READ leafLevel WRITE setLeafLevel NOTIFY optionsChanged)
    Q_PROPERTY(bool colorByKind READ colorByKind WRITE setColorByKind NOTIFY optionsChanged)
    Q_PROPERTY(QVariantList kinds READ kinds WRITE setKinds NOTIFY optionsChanged)
    Q_PROPERTY(QVariantMap colors READ colors WRITE setColors NOTIFY optionsChanged)
    Q_PROPERTY(int hovered READ hovered NOTIFY hoverChanged)
    Q_PROPERTY(QVariantMap hoveredInfo READ hoveredInfo NOTIFY hoverChanged)
    Q_PROPERTY(int activeSymbol READ activeSymbol WRITE setActiveSymbol NOTIFY optionsChanged)
    Q_PROPERTY(int leafCount READ leafCount NOTIFY rebuilt)
    Q_PROPERTY(int linkCount READ linkCount NOTIFY rebuilt)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY optionsChanged)

public:
    explicit BundleView(QQuickItem *parent = nullptr);

    qreal bundling() const { return m_beta; }
    void setBundling(qreal b);
    int leafLevel() const { return m_leafLevel; }
    void setLeafLevel(int l);
    bool colorByKind() const { return m_colorByKind; }
    void setColorByKind(bool v);
    QVariantList kinds() const;
    void setKinds(const QVariantList &k);
    QVariantMap colors() const { return m_colors; }
    void setColors(const QVariantMap &c);
    int hovered() const { return m_hovered; }
    QVariantMap hoveredInfo() const;
    int activeSymbol() const { return m_activeSymbol; }
    void setActiveSymbol(int s);
    int leafCount() const { return int(m_leaves.size()); }
    int linkCount() const { return int(m_links.size()); }
    qreal zoom() const { return m_zoom; }
    void setZoom(qreal z);

    Q_INVOKABLE void hover(qreal x, qreal y);
    // The symbol (or -1) and file (or -1) under a point
    Q_INVOKABLE QVariantMap itemAt(qreal x, qreal y) const;

signals:
    void optionsChanged();
    void hoverChanged();
    void rebuilt();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &n, const QRectF &o) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    bool m_dirty = false;
    struct HNode {
        QString name;
        int parent = -1;
        int depth = 0;
        QVector<int> children;
        double angle = 0, angleFrom = 0, angleTo = 0;
        int symbol = -1, file = -1;
        int leaf = -1;      // index into m_leaves
        QString folder;     // a folded folder as one leaf
    };
    struct BLink {
        int a, b;           // leaves
        int count;
        int kind;
    };
    void rebuild();
    void layout();
    int leafAt(qreal x, qreal y) const;
    QPointF nodePoint(int node) const;

    QVector<HNode> m_nodes;
    QVector<int> m_leaves;  // node ids
    QVector<BLink> m_links;
    QVector<QVector<int>> m_leafLinks;
    int m_maxDepth = 1;
    qreal m_beta = 0.85;
    int m_leafLevel = 0;    // 0 classes and functions, 1 files
    bool m_colorByKind = false;
    QSet<int> m_kinds;
    QVariantMap m_colors;
    int m_hovered = -1;
    int m_activeSymbol = -1;
    qreal m_zoom = 1;
    bool m_cacheValid = false;   // the links' and labels' nodes are up to date
    class QSGOpacityNode *m_sgFade = nullptr;   // all of it, faded while a leaf is hovered
    QSGNode *m_sgOverlay = nullptr;
    void ensurePaths();
    QVector<QVector<int>> m_chains;       // per link, the hierarchy nodes it bends through
    QVector<QPainterPath> m_paths;        // per link, in item coordinates
    QVector<QPair<QPointF, QPointF>> m_ends;
    bool m_pathsValid = false;
    QSet<int> m_hoverLinked;              // leaves linked to the hovered one
    mutable QVector<QPair<double, int>> m_leafAngles;
    std::shared_ptr<const Unravel::CodeModel> m_model;
};

// A squarified treemap of folders, files and their classes and
// functions: area for size, colour for a metric
class TreemapView : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int colorMetric READ colorMetric WRITE setColorMetric NOTIFY optionsChanged)
    Q_PROPERTY(int sizeMetric READ sizeMetric WRITE setSizeMetric NOTIFY optionsChanged)
    Q_PROPERTY(int depth READ depth WRITE setDepth NOTIFY optionsChanged)
    Q_PROPERTY(QString rootPath READ rootPath WRITE setRootPath NOTIFY optionsChanged)
    Q_PROPERTY(QVariantMap colors READ colors WRITE setColors NOTIFY optionsChanged)
    Q_PROPERTY(QVariantMap hoveredInfo READ hoveredInfo NOTIFY hoverChanged)
    Q_PROPERTY(int activeSymbol READ activeSymbol WRITE setActiveSymbol NOTIFY optionsChanged)
    Q_PROPERTY(QString legendLow READ legendLow NOTIFY optionsChanged)
    Q_PROPERTY(QString legendHigh READ legendHigh NOTIFY optionsChanged)

public:
    enum ColorMetric { Complexity, FanIn, FanOut, Unused, Findings, Nesting, Kind, Churn };
    Q_ENUM(ColorMetric)
    enum SizeMetric { Lines, Statements, Uniform };
    Q_ENUM(SizeMetric)

    explicit TreemapView(QQuickItem *parent = nullptr);

    int colorMetric() const { return m_colorMetric; }
    void setColorMetric(int m);
    int sizeMetric() const { return m_sizeMetric; }
    void setSizeMetric(int m);
    int depth() const { return m_depth; }
    void setDepth(int d);
    QString rootPath() const { return m_rootPath; }
    void setRootPath(const QString &p);
    QVariantMap colors() const { return m_colors; }
    void setColors(const QVariantMap &c);
    QVariantMap hoveredInfo() const { return m_hoveredInfo; }
    int activeSymbol() const { return m_activeSymbol; }
    void setActiveSymbol(int s);
    QString legendLow() const;
    QString legendHigh() const;

    Q_INVOKABLE void hover(qreal x, qreal y);
    Q_INVOKABLE QVariantMap itemAt(qreal x, qreal y) const;

signals:
    void optionsChanged();
    void hoverChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &n, const QRectF &o) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    bool m_dirty = false;
    QSGNode *m_sgBase = nullptr, *m_sgOverlay = nullptr;   // the map, and the hover over it
    bool m_imageValid = false;
    struct TNode {
        QString name, path;
        double size = 0;
        double value = 0;   // the colour metric
        int symbol = -1, file = -1;
        int kind = -1;      // -3 folder, -2 file, else SymbolKind
        QVector<int> children;
        QRectF rect;
        int depth = 0;
    };
    void rebuild();
    void layoutNode(int n, const QRectF &r, int depth);
    double metricOf(qint32 symbol) const;
    QColor colorFor(const TNode &n) const;
    int nodeAt(qreal x, qreal y) const;

    QVector<TNode> m_tree;
    int m_root = -1;
    double m_maxValue = 1;
    int m_colorMetric = Complexity;
    int m_sizeMetric = Lines;
    int m_depth = 4;
    QString m_rootPath;
    QVariantMap m_colors;
    QVariantMap m_hoveredInfo;
    int m_hoveredNode = -1;
    int m_activeSymbol = -1;
    QSet<qint32> m_findingSymbols;
    QHash<qint32, int> m_findingCount;
    bool m_historyReady = false;
    std::shared_ptr<const Unravel::CodeModel> m_model;
};

// A dependency structure matrix: rows depend on columns, ordered so that
// dependencies come first; cycles show above the diagonal
class MatrixView : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int level READ level WRITE setLevel NOTIFY optionsChanged)
    Q_PROPERTY(int moduleDepth READ moduleDepth WRITE setModuleDepth NOTIFY optionsChanged)
    Q_PROPERTY(QVariantMap colors READ colors WRITE setColors NOTIFY optionsChanged)
    Q_PROPERTY(QVariantMap hoveredInfo READ hoveredInfo NOTIFY hoverChanged)
    Q_PROPERTY(int count READ count NOTIFY rebuilt)
    Q_PROPERTY(qreal cell READ cell NOTIFY rebuilt)
    Q_PROPERTY(qreal labelWidth READ labelWidth NOTIFY rebuilt)
    Q_PROPERTY(int selectedLink READ selectedLink NOTIFY selectionChanged)
    // The item is as big as the viewport and paints only what shows of the
    // whole matrix (contentWidth × contentHeight) at the scroll offset
    Q_PROPERTY(qreal viewX READ viewX WRITE setViewX NOTIFY viewChanged)
    Q_PROPERTY(qreal viewY READ viewY WRITE setViewY NOTIFY viewChanged)
    Q_PROPERTY(qreal contentWidth READ contentWidth NOTIFY rebuilt)
    Q_PROPERTY(qreal contentHeight READ contentHeight NOTIFY rebuilt)

public:
    explicit MatrixView(QQuickItem *parent = nullptr);

    int level() const { return m_level; }
    void setLevel(int l);
    int moduleDepth() const { return m_moduleDepth; }
    void setModuleDepth(int d);
    QVariantMap colors() const { return m_colors; }
    void setColors(const QVariantMap &c);
    QVariantMap hoveredInfo() const { return m_hoveredInfo; }
    int count() const { return int(m_order.size()); }
    qreal cell() const { return m_cell; }
    qreal labelWidth() const { return m_labelWidth; }
    int selectedLink() const { return m_selectedLink; }
    qreal viewX() const { return m_viewX; }
    void setViewX(qreal v);
    qreal viewY() const { return m_viewY; }
    void setViewY(qreal v);
    qreal contentWidth() const { return m_labelWidth + m_order.size() * m_cell + 20; }
    qreal contentHeight() const { return 30 + m_order.size() * m_cell + 20; }

    Q_INVOKABLE void hover(qreal x, qreal y);
    Q_INVOKABLE void select(qreal x, qreal y);
    Q_INVOKABLE QVariantList selectedReferences(int limit = 300) const;
    Q_INVOKABLE QVariantMap nodeInfo(qreal x, qreal y) const;

signals:
    void optionsChanged();
    void hoverChanged();
    void rebuilt();
    void selectionChanged();
    void viewChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &n, const QRectF &o) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    void rebuild();
    void resize();
    bool cellAt(qreal x, qreal y, int *row, int *col) const;

    Unravel::Analysis::DepGraph m_graph;
    QVector<int> m_order;
    QVector<int> m_position;
    int m_level = 0;
    int m_moduleDepth = 0;
    QVariantMap m_colors;
    QVariantMap m_hoveredInfo;
    int m_hoverRow = -1, m_hoverCol = -1;
    int m_selectedLink = -1;
    int m_selRow = -1, m_selCol = -1;
    qreal m_cell = 16;
    qreal m_labelWidth = 200;
    qreal m_viewX = 0, m_viewY = 0;
    bool m_dirty = false;   // the model changed while hidden
    bool m_baseValid = false;   // the cells' nodes are up to date (hover drawn apart)
    QSGNode *m_sgBase = nullptr, *m_sgOverlay = nullptr;
    int m_maxCount = 1;
    std::shared_ptr<const Unravel::CodeModel> m_model;
};
