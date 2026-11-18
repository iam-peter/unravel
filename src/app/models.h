#pragma once

#include <QAbstractItemModel>
#include <QAbstractListModel>
#include <QAbstractTableModel>
#include <QHash>
#include <QObject>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "analysis.h"
#include "model.h"

class ReviewFocus;
// The list, tree and table models the QML views show, all over the
// Session's code model and rebuilt when it changes.

// Fuzzy search over symbols and files: "inv::rep" finds Inventory::report
class SymbolSearch : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool includeExternal READ includeExternal WRITE setIncludeExternal NOTIFY queryChanged)
    Q_PROPERTY(int category READ category WRITE setCategory NOTIFY queryChanged)
    Q_PROPERTY(int limit READ limit WRITE setLimit NOTIFY queryChanged)

public:
    enum Roles { SymbolRole = Qt::UserRole + 1, FileRole, NameRole, QualifiedRole, KindRole, KindNameRole,
                 LocationRole, NameHtmlRole, ExternalRole, ScoreRole };
    enum Category { All, Types, Functions, Variables, Files, Macros };
    Q_ENUM(Category)

    explicit SymbolSearch(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString query() const { return m_query; }
    void setQuery(const QString &q);
    int count() const { return int(m_results.size()); }
    bool includeExternal() const { return m_includeExternal; }
    void setIncludeExternal(bool v);
    int category() const { return m_category; }
    void setCategory(int c);
    int limit() const { return m_limit; }
    void setLimit(int l);

    Q_INVOKABLE QVariantMap get(int row) const;

    // Score of a pattern in a text, or -1; positions of the matched characters
    static int score(const QString &text, const QString &pattern, QVector<int> *positions = nullptr);

signals:
    void queryChanged();
    void countChanged();

private:
    void run();
    struct Hit {
        int symbol = -1, file = -1;
        int score = 0;
        QVector<int> positions;
        bool inQualified = false;
    };
    QString m_query;
    bool m_includeExternal = false;
    int m_category = All;
    int m_limit = 200;
    QVector<Hit> m_results;
    std::shared_ptr<const Unravel::CodeModel> m_model;
};

// The project as a tree: folders, files, what they declare, the members
class ProjectTree : public QAbstractItemModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY modeChanged)

public:
    enum Roles { NameRole = Qt::UserRole + 1, KindRole, SymbolRole, FileRole, DetailRole, ExternalRole,
                 UnusedRole, IconRole, PathRole };
    enum Mode { Files, Namespaces };
    Q_ENUM(Mode)

    explicit ProjectTree(QObject *parent = nullptr);
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool hasChildren(const QModelIndex &parent = {}) const override;
    QHash<int, QByteArray> roleNames() const override;

    int mode() const { return m_mode; }
    void setMode(int m);

    // The index of a symbol's or a file's row, made on the way (for revealing)
    Q_INVOKABLE QModelIndex indexOfSymbol(int symbol);
    Q_INVOKABLE QModelIndex indexOfFile(int file);

signals:
    void modeChanged();

private:
    struct Node {
        int parent = -1;
        int row = 0;
        int kind = -3;        // -3 folder, -2 file, else SymbolKind
        int symbol = -1, file = -1;
        QString name, detail, path;
        bool built = false;
        bool leaf = false;
        QVector<int> children;
    };
    void reset();
    void build(int node) const;
    int addNode(Node n) const;
    mutable QVector<Node> m_nodes;
    int m_mode = Files;
    std::shared_ptr<const Unravel::CodeModel> m_model;
};

// Metrics as a sortable table, with each value's share of its column's
// maximum for the table lens bars
class MetricsTable : public QAbstractTableModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(int sortColumn READ sortColumn NOTIFY sortChanged)
    Q_PROPERTY(bool descending READ descending NOTIFY sortChanged)
    Q_PROPERTY(QStringList headers READ headers NOTIFY modeChanged)
    Q_PROPERTY(QVariantList columnWidths READ columnWidths NOTIFY modeChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString prioritySummary READ prioritySummary NOTIFY countChanged)

public:
    enum Roles { ValueRole = Qt::UserRole + 1, FractionRole, SymbolRole, FileRole, NumericRole, HotRole };
    enum Mode { Functions, Classes, Files, Modules };
    Q_ENUM(Mode)

    explicit MetricsTable(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int mode() const { return m_mode; }
    void setMode(int m);
    QString filter() const { return m_filter; }
    void setFilter(const QString &f);
    int sortColumn() const { return m_sortColumn; }
    bool descending() const { return m_descending; }
    QStringList headers() const;
    QVariantList columnWidths() const;
    int count() const { return int(m_rows.size()); }
    QString prioritySummary() const;

    Q_INVOKABLE void sortBy(int column);
    Q_INVOKABLE QVariantMap rowInfo(int row) const;
    Q_INVOKABLE QString toCsv() const;

signals:
    void modeChanged();
    void filterChanged();
    void sortChanged();
    void countChanged();

private:
    struct Row {
        int symbol = -1, file = -1;
        QString name;
        QVector<double> values;
        QString location;
        bool binaryHistory = false;
    };
    struct Column {
        QString title;
        bool numeric = true;
        double hot = 0;   // values from here on are worth a look (0: none)
        int width = 80;
    };
    void reload();
    void applySort();
    QVector<Column> m_columns;
    QVector<Row> m_all, m_rows;
    QVector<double> m_max;
    int m_mode = Functions;
    ReviewFocus *m_priority = nullptr;
    QString m_filter;
    int m_sortColumn = 2;
    bool m_descending = true;
    std::shared_ptr<const Unravel::CodeModel> m_model;
};

// The analysis' findings, by kind
class FindingsList : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList kinds READ kinds NOTIFY changed)
    Q_PROPERTY(int kind READ kind WRITE setKind NOTIFY changed)
    Q_PROPERTY(int minSeverity READ minSeverity WRITE setMinSeverity NOTIFY changed)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)

public:
    enum Roles { KindRole = Qt::UserRole + 1, KindNameRole, SeverityRole, TitleRole, DetailRole, LocationRole,
                 SymbolRole, FileRole, LineRole, SymbolsRole, IndexRole };

    explicit FindingsList(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QVariantList kinds() const;
    int kind() const { return m_kind; }
    void setKind(int k);
    int minSeverity() const { return m_minSeverity; }
    void setMinSeverity(int s);
    QString filter() const { return m_filter; }
    void setFilter(const QString &f);
    int count() const { return int(m_rows.size()); }

    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE QString hint(int kind) const;

signals:
    void changed();

private:
    void reload();
    QVector<int> m_rows;   // indexes into the report's findings
    int m_kind = -1;
    int m_minSeverity = 0;
    QString m_filter;
    std::shared_ptr<const Unravel::CodeModel> m_model;
};

// A call tree to unfold: what a function calls in the order of its code,
// what those call, and so on (or who calls it, and who calls those)
class CallTree : public QAbstractItemModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int root READ root WRITE setRoot NOTIFY rootChanged)
    Q_PROPERTY(bool callers READ callers WRITE setCallers NOTIFY rootChanged)
    Q_PROPERTY(bool showImplicit READ showImplicit WRITE setShowImplicit NOTIFY rootChanged)
    Q_PROPERTY(bool showExternal READ showExternal WRITE setShowExternal NOTIFY rootChanged)

public:
    enum Roles { NameRole = Qt::UserRole + 1, SymbolRole, KindRole, LineRole, CountRole, RecursiveRole,
                 VirtualRole, ExternalRole, RefRole, DetailRole, DispatchRole, ComplexityRole };

    explicit CallTree(QObject *parent = nullptr);
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool hasChildren(const QModelIndex &parent = {}) const override;
    QHash<int, QByteArray> roleNames() const override;

    int root() const { return m_root; }
    void setRoot(int s);
    bool callers() const { return m_callers; }
    void setCallers(bool v);
    bool showImplicit() const { return m_showImplicit; }
    void setShowImplicit(bool v);
    bool showExternal() const { return m_showExternal; }
    void setShowExternal(bool v);

signals:
    void rootChanged();

private:
    struct Node {
        int parent = -1;
        int row = 0;
        int symbol = -1;
        int ref = -1;
        int count = 1;
        bool recursive = false;
        bool dispatch = false;
        bool built = false;
        QVector<int> children;
    };
    void reset();
    void build(int node) const;
    mutable QVector<Node> m_nodes;
    int m_root = -1;
    bool m_callers = false;
    bool m_showImplicit = false;
    bool m_showExternal = true;
    std::shared_ptr<const Unravel::CodeModel> m_model;
};

// The code for the active symbol, as Sourcetrail's code view: its
// definition first, then every place that uses it, with the lines around;
// or a whole file. Names the indexer knows are links.
class CodeSnippets : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int symbol READ symbol WRITE setSymbol NOTIFY inputChanged)
    Q_PROPERTY(int file READ file WRITE setFile NOTIFY inputChanged)
    Q_PROPERTY(int line READ line WRITE setLine NOTIFY inputChanged)
    Q_PROPERTY(bool wholeFile READ wholeFile WRITE setWholeFile NOTIFY inputChanged)
    Q_PROPERTY(int context READ context WRITE setContext NOTIFY inputChanged)
    Q_PROPERTY(QVariantMap colors READ colors WRITE setColors NOTIFY inputChanged)
    Q_PROPERTY(QVariantList snippets READ snippets NOTIFY snippetsChanged)
    Q_PROPERTY(int referenceCount READ referenceCount NOTIFY snippetsChanged)
    Q_PROPERTY(int fileCount READ fileCount NOTIFY snippetsChanged)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY snippetsChanged)

public:
    explicit CodeSnippets(QObject *parent = nullptr);

    int symbol() const { return m_symbol; }
    void setSymbol(int s);
    int file() const { return m_file; }
    void setFile(int f);
    int line() const { return m_line; }
    void setLine(int l);
    bool wholeFile() const { return m_wholeFile; }
    void setWholeFile(bool v);
    int context() const { return m_context; }
    void setContext(int c);
    QVariantMap colors() const { return m_colors; }
    void setColors(const QVariantMap &c);
    QVariantList snippets() const { return m_snippets; }
    int referenceCount() const { return m_referenceCount; }
    int fileCount() const { return m_fileCount; }
    int hiddenCount() const { return m_hiddenCount; }

    // One file's lines as HTML, for the file view
    Q_INVOKABLE QVariantMap fileSnippet(int file, int focusLine);

signals:
    void inputChanged();
    void snippetsChanged();

private:
    struct FileText {
        QStringList lines;
        QVector<QByteArray> raw;
        QVector<QVector<QPair<int, int>>> spans;   // per line: (start, length) with kind in a parallel list
        QVector<QVector<int>> kinds;
    };
    void schedule();
    void rebuild();
    const FileText *text(int file);
    QVariantMap snippet(int file, int from, int to, const QString &title, const QSet<int> &markLines,
                        bool definition);
    QString lineHtml(int file, int line, const FileText &t);

    int m_symbol = -1, m_file = -1, m_line = 0;
    bool m_wholeFile = false;
    int m_context = 2;
    QVariantMap m_colors;
    QVariantList m_snippets;
    int m_referenceCount = 0, m_fileCount = 0, m_hiddenCount = 0;
    bool m_pending = false;
    QHash<int, FileText> m_texts;
    QList<int> m_textOrder;
    std::shared_ptr<const Unravel::CodeModel> m_model;
};
