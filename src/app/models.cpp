#include "models.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include "changes.h"
#include "highlight.h"
#include "hotspots.h"
#include "reviewfocus.h"
#include "session.h"

using namespace Unravel;

namespace {

bool boundary(const QString &t, qsizetype i)
{
    if (i <= 0)
        return true;
    const QChar p = t[i - 1], c = t[i];
    if (p == QLatin1Char('_') || p == QLatin1Char(':') || p == QLatin1Char('.') || p == QLatin1Char('/')
        || p == QLatin1Char(' ') || p == QLatin1Char('-') || p == QLatin1Char('~'))
        return true;
    return p.isLower() && c.isUpper();
}

QString esc(const QString &s)
{
    QString out;
    out.reserve(s.size() + 8);
    for (QChar c : s) {
        if (c == QLatin1Char('<'))
            out += QStringLiteral("&lt;");
        else if (c == QLatin1Char('>'))
            out += QStringLiteral("&gt;");
        else if (c == QLatin1Char('&'))
            out += QStringLiteral("&amp;");
        else
            out += c;
    }
    return out;
}

QString iconFor(int kind)
{
    if (kind == -3)
        return QStringLiteral("folder");
    if (kind == -2)
        return QStringLiteral("file-code");
    const SymbolKind k = SymbolKind(kind);
    if (k == SymbolKind::Namespace)
        return QStringLiteral("braces");
    if (k == SymbolKind::Enum || k == SymbolKind::EnumConstant)
        return QStringLiteral("list");
    if (k == SymbolKind::Typedef)
        return QStringLiteral("type");
    if (isType(k))
        return QStringLiteral("box");
    if (isFunction(k))
        return QStringLiteral("function-square");
    if (k == SymbolKind::Macro)
        return QStringLiteral("hash");
    return QStringLiteral("variable");
}

} // namespace

// --- SymbolSearch -------------------------------------------------------------------

SymbolSearch::SymbolSearch(QObject *parent)
    : QAbstractListModel(parent)
{
    connect(Session::instance(), &Session::modelChanged, this, &SymbolSearch::run);
}

int SymbolSearch::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_results.size());
}

QHash<int, QByteArray> SymbolSearch::roleNames() const
{
    return {{SymbolRole, "symbol"}, {FileRole, "file"}, {NameRole, "name"}, {QualifiedRole, "qualified"},
            {KindRole, "kind"}, {KindNameRole, "kindName"}, {LocationRole, "location"}, {NameHtmlRole, "nameHtml"},
            {ExternalRole, "external"}, {ScoreRole, "score"}};
}

QVariant SymbolSearch::data(const QModelIndex &index, int role) const
{
    if (!m_model || index.row() < 0 || index.row() >= m_results.size())
        return {};
    const Hit &h = m_results[index.row()];
    const CodeModel &m = *m_model;
    if (h.file >= 0) {
        const File &f = m.files[h.file];
        switch (role) {
        case SymbolRole: return -1;
        case FileRole: return h.file;
        case NameRole: return QFileInfo(f.path).fileName();
        case QualifiedRole: return f.relative;
        case KindRole: return -2;
        case KindNameRole: return QStringLiteral("file");
        case LocationRole: return f.relative;
        case ExternalRole: return !f.project;
        case ScoreRole: return h.score;
        case NameHtmlRole: {
            QString out;
            const QString &t = f.relative;
            for (qsizetype i = 0; i < t.size(); ++i) {
                const bool on = h.positions.contains(int(i));
                out += on ? QStringLiteral("<b>") + esc(QString(t[i])) + QStringLiteral("</b>") : esc(QString(t[i]));
            }
            return out;
        }
        }
        return {};
    }
    const Symbol &s = m.symbols[h.symbol];
    switch (role) {
    case SymbolRole: return h.symbol;
    case FileRole: return s.where().file;
    case NameRole: return s.name;
    case QualifiedRole: return m.displayName(h.symbol);
    case KindRole: return int(s.kind);
    case KindNameRole: return kindName(s.kind);
    case LocationRole: return m.locationText(s.where());
    case ExternalRole: return s.has(External);
    case ScoreRole: return h.score;
    case NameHtmlRole: {
        const QString t = h.inQualified ? m.displayName(h.symbol) : s.name;
        QString out;
        for (qsizetype i = 0; i < t.size(); ++i) {
            const bool on = h.positions.contains(int(i));
            out += on ? QStringLiteral("<b>") + esc(QString(t[i])) + QStringLiteral("</b>") : esc(QString(t[i]));
        }
        return out;
    }
    }
    return {};
}

void SymbolSearch::setQuery(const QString &q)
{
    if (q == m_query)
        return;
    m_query = q;
    emit queryChanged();
    run();
}

void SymbolSearch::setIncludeExternal(bool v)
{
    if (v == m_includeExternal)
        return;
    m_includeExternal = v;
    emit queryChanged();
    run();
}

void SymbolSearch::setCategory(int c)
{
    if (c == m_category)
        return;
    m_category = c;
    emit queryChanged();
    run();
}

void SymbolSearch::setLimit(int l)
{
    if (l == m_limit)
        return;
    m_limit = l;
    emit queryChanged();
    run();
}

QVariantMap SymbolSearch::get(int row) const
{
    QVariantMap out;
    const QModelIndex i = index(row, 0);
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        out[QString::fromLatin1(it.value())] = data(i, it.key());
    return out;
}

int SymbolSearch::score(const QString &text, const QString &pattern, QVector<int> *positions)
{
    if (pattern.isEmpty())
        return 0;
    const qsizetype idx = text.indexOf(pattern, 0, Qt::CaseInsensitive);
    if (idx >= 0) {
        int base = 400;
        if (idx == 0 && text.size() == pattern.size())
            base = 1000;
        else if (idx == 0)
            base = 800;
        else if (boundary(text, idx))
            base = 600;
        if (text.startsWith(pattern))   // case matches too
            base += 20;
        if (positions) {
            positions->clear();
            for (qsizetype i = idx; i < idx + pattern.size(); ++i)
                positions->append(int(i));
        }
        return base - int(std::min<qsizetype>(text.size() - pattern.size(), 200));
    }
    // In order, not together: prefer the starts of words
    QVector<int> pos;
    qsizetype j = 0;
    int score = 200;
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        const QChar pc = pattern[i].toLower();
        // The next start of a word with that letter, if near; else the next letter
        qsizetype found = -1, firstAny = -1;
        for (qsizetype k = j; k < text.size(); ++k) {
            if (text[k].toLower() != pc)
                continue;
            if (firstAny < 0)
                firstAny = k;
            if (boundary(text, k)) {
                found = k;
                break;
            }
        }
        if (found < 0)
            found = firstAny;
        if (found < 0)
            return -1;
        if (boundary(text, found))
            score += 12;
        if (!pos.isEmpty() && found == pos.last() + 1)
            score += 8;
        score -= int(std::min<qsizetype>(found - j, 20));
        pos.append(int(found));
        j = found + 1;
    }
    if (positions)
        *positions = pos;
    return std::max(1, score - int(std::min<qsizetype>(text.size(), 100)) / 4);
}

void SymbolSearch::run()
{
    beginResetModel();
    m_model = Session::instance()->model();
    m_results.clear();
    const QString q = m_query.trimmed();
    if (m_model && !q.isEmpty()) {
        const CodeModel &m = *m_model;
        const bool qualifiedQuery = q.contains(QLatin1String("::"));
        const QStringList words = q.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (m_category != Files) {
            for (int i = 0; i < m.symbols.size(); ++i) {
                const Symbol &s = m.symbols[i];
                if ((!m_includeExternal && s.has(External)) || s.has(Implicit) || s.has(FromMacro))
                    continue;
                if (s.name.isEmpty() || s.name.startsWith(QLatin1Char('(')))
                    continue;
                switch (m_category) {
                case Types: if (!isType(s.kind)) continue; break;
                case Functions: if (!isFunction(s.kind)) continue; break;
                case Variables: if (!isVariable(s.kind)) continue; break;
                case Macros: if (s.kind != SymbolKind::Macro) continue; break;
                default: break;
                }
                Hit h;
                h.symbol = i;
                if (words.size() > 1) {
                    const QString qn = m.displayName(i);
                    int total = 0;
                    bool all = true;
                    for (const QString &w : words) {
                        const int sc = score(qn, w);
                        if (sc < 0) {
                            all = false;
                            break;
                        }
                        total += sc;
                    }
                    if (!all)
                        continue;
                    h.score = total / int(words.size());
                    h.inQualified = true;
                    score(qn, words.first(), &h.positions);
                } else if (qualifiedQuery) {
                    h.score = score(m.displayName(i), q, &h.positions);
                    h.inQualified = true;
                } else {
                    h.score = score(s.name, q, &h.positions);
                    if (h.score < 0) {
                        h.score = score(m.displayName(i), q, &h.positions);
                        h.inQualified = true;
                        if (h.score >= 0)
                            h.score /= 3;
                    }
                }
                if (h.score < 0)
                    continue;
                if (s.has(External))
                    h.score -= 150;
                if (!s.has(Defined))
                    h.score -= 20;
                if (isType(s.kind))
                    h.score += 6;
                m_results.append(h);
            }
        }
        if (m_category == All || m_category == Files) {
            for (int f = 0; f < m.files.size(); ++f) {
                const File &file = m.files[f];
                if (!file.project && !m_includeExternal)
                    continue;
                Hit h;
                h.file = f;
                h.score = score(file.relative, q, &h.positions);
                if (h.score < 0)
                    continue;
                h.score -= 30;
                m_results.append(h);
            }
        }
        // Only the best `limit` need an order
        auto better = [&](const Hit &a, const Hit &b) {
            if (a.score != b.score)
                return a.score > b.score;
            const QString &na = a.file >= 0 ? m.files[a.file].relative : m.symbols[a.symbol].qualified;
            const QString &nb = b.file >= 0 ? m.files[b.file].relative : m.symbols[b.symbol].qualified;
            if (na.size() != nb.size())
                return na.size() < nb.size();
            return na < nb;
        };
        const qsizetype keep = std::min<qsizetype>(m_limit, m_results.size());
        std::partial_sort(m_results.begin(), m_results.begin() + keep, m_results.end(), better);
        m_results.resize(keep);
    }
    endResetModel();
    emit countChanged();
}

// --- ProjectTree ---------------------------------------------------------------------

ProjectTree::ProjectTree(QObject *parent)
    : QAbstractItemModel(parent)
{
    connect(Session::instance(), &Session::modelChanged, this, &ProjectTree::reset);
    reset();
}

void ProjectTree::setMode(int m)
{
    if (m == m_mode)
        return;
    m_mode = m;
    emit modeChanged();
    reset();
}

int ProjectTree::addNode(Node n) const
{
    n.row = n.parent >= 0 ? int(m_nodes[n.parent].children.size()) : 0;
    m_nodes.append(n);
    const int id = int(m_nodes.size() - 1);
    if (n.parent >= 0)
        m_nodes[n.parent].children.append(id);
    return id;
}

void ProjectTree::reset()
{
    beginResetModel();
    m_model = Session::instance()->model();
    m_nodes.clear();
    Node root;
    root.parent = -1;
    root.built = true;
    m_nodes.append(root);
    if (m_model) {
        const CodeModel &m = *m_model;
        if (m_mode == Files) {
            QHash<QString, int> folders;
            folders.insert(QString(), 0);
            std::function<int(const QString &)> folder = [&](const QString &path) -> int {
                const auto it = folders.constFind(path);
                if (it != folders.cend())
                    return *it;
                const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
                const int parent = folder(slash < 0 ? QString() : path.left(slash));
                Node n;
                n.parent = parent;
                n.kind = -3;
                n.name = slash < 0 ? path : path.mid(slash + 1);
                n.path = path;
                n.built = true;
                const int id = addNode(n);
                folders.insert(path, id);
                return id;
            };
            QVector<qint32> files;
            for (qint32 f = 0; f < m.files.size(); ++f) {
                if (m.files[f].project)
                    files.append(f);
            }
            // Folders before files at each level, both by name
            std::sort(files.begin(), files.end(), [&](qint32 a, qint32 b) {
                const QString &ma = m.files[a].module, &mb = m.files[b].module;
                if (ma != mb) {
                    if (ma.startsWith(mb + QLatin1Char('/')) || (mb.isEmpty() && !ma.isEmpty()))
                        return true;
                    if (mb.startsWith(ma + QLatin1Char('/')) || (ma.isEmpty() && !mb.isEmpty()))
                        return false;
                    return ma < mb;
                }
                return m.files[a].relative < m.files[b].relative;
            });
            for (qint32 f : std::as_const(files)) {
                const File &file = m.files[f];
                Node n;
                n.parent = folder(file.module);
                n.kind = -2;
                n.file = f;
                n.name = QFileInfo(file.path).fileName();
                n.detail = file.generated ? tr("generated") : tr("%1 lines").arg(file.lines);
                addNode(n);
            }
        }
        // Namespaces: built lazily from the root
        else {
            m_nodes[0].built = false;
        }
    }
    endResetModel();
}

void ProjectTree::build(int node) const
{
    Node &n = m_nodes[node];
    if (n.built || !m_model)
        return;
    n.built = true;
    const CodeModel &m = *m_model;
    QVector<qint32> syms;
    auto ok = [&](qint32 s) {
        const Symbol &x = m.symbols[s];
        return !x.has(Implicit) && !x.has(FromMacro) && !x.has(External) && !x.name.isEmpty();
    };
    if (node == 0 && m_mode == Namespaces) {
        for (qint32 s = 0; s < m.symbols.size(); ++s) {
            if (m.symbols[s].parent < 0 && ok(s) && m.symbols[s].kind != SymbolKind::Macro)
                syms.append(s);
        }
    } else if (n.kind == -2) {
        for (qint32 s : m.files[n.file].symbols) {
            const Symbol &x = m.symbols[s];
            if (!ok(s) || x.kind == SymbolKind::Namespace)
                continue;
            if (m.nodeOwner(s) != s)
                continue;
            syms.append(s);
        }
        // Definitions of methods declared elsewhere: listed under their class there
    } else if (n.symbol >= 0) {
        for (qint32 c : m.symbols[n.symbol].children) {
            if (ok(c))
                syms.append(c);
        }
    }
    std::sort(syms.begin(), syms.end(), [&](qint32 a, qint32 b) {
        const Symbol &x = m.symbols[a], &y = m.symbols[b];
        if (m_mode == Namespaces && (x.kind == SymbolKind::Namespace) != (y.kind == SymbolKind::Namespace))
            return x.kind == SymbolKind::Namespace;
        if (m_mode == Namespaces)
            return x.name < y.name;
        return x.where().line < y.where().line;
    });
    const int parent = node;
    for (qint32 s : std::as_const(syms)) {
        const Symbol &x = m.symbols[s];
        Node c;
        c.parent = parent;
        c.kind = int(x.kind);
        c.symbol = s;
        c.name = x.name + (isFunction(x.kind) ? QStringLiteral("()") : QString());
        c.detail = isFunction(x.kind) ? x.signature : x.type;
        addNode(c);
    }
}

QModelIndex ProjectTree::index(int row, int column, const QModelIndex &parent) const
{
    const int p = parent.isValid() ? int(parent.internalId()) : 0;
    if (p < 0 || p >= m_nodes.size())
        return {};
    build(p);
    if (row < 0 || row >= m_nodes[p].children.size() || column != 0)
        return {};
    return createIndex(row, column, quintptr(m_nodes[p].children[row]));
}

QModelIndex ProjectTree::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};
    const int id = int(child.internalId());
    const int p = m_nodes[id].parent;
    if (p <= 0)
        return {};
    return createIndex(m_nodes[p].row, 0, quintptr(p));
}

int ProjectTree::rowCount(const QModelIndex &parent) const
{
    const int p = parent.isValid() ? int(parent.internalId()) : 0;
    if (p < 0 || p >= m_nodes.size())
        return 0;
    build(p);
    return int(m_nodes[p].children.size());
}

int ProjectTree::columnCount(const QModelIndex &) const
{
    return 1;
}

bool ProjectTree::hasChildren(const QModelIndex &parent) const
{
    const int p = parent.isValid() ? int(parent.internalId()) : 0;
    if (p < 0 || p >= m_nodes.size())
        return false;
    // Built here with the same filter as the rows: no arrow that opens on nothing
    build(p);
    return !m_nodes[p].children.isEmpty();
}

QHash<int, QByteArray> ProjectTree::roleNames() const
{
    return {{Qt::DisplayRole, "display"}, {NameRole, "name"}, {KindRole, "kind"}, {SymbolRole, "symbol"},
            {FileRole, "file"}, {DetailRole, "detail"}, {ExternalRole, "external"}, {UnusedRole, "unused"},
            {IconRole, "iconName"}, {PathRole, "path"}};
}

QVariant ProjectTree::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const Node &n = m_nodes[int(index.internalId())];
    switch (role) {
    case Qt::DisplayRole:
    case NameRole: return n.name;
    case KindRole: return n.kind;
    case SymbolRole: return n.symbol;
    case FileRole: return n.file;
    case DetailRole: return n.detail;
    case ExternalRole: return n.file >= 0 && m_model && m_model->files[n.file].generated;
    case UnusedRole: return n.symbol >= 0 && m_model && Analysis::looksUnused(*m_model, n.symbol);
    case IconRole: return iconFor(n.kind);
    case PathRole: return n.path;
    }
    return {};
}

QModelIndex ProjectTree::indexOfFile(int file)
{
    for (int i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].kind == -2 && m_nodes[i].file == file)
            return createIndex(m_nodes[i].row, 0, quintptr(i));
    }
    return {};
}

QModelIndex ProjectTree::indexOfSymbol(int symbol)
{
    if (!m_model || symbol < 0)
        return {};
    const CodeModel &m = *m_model;
    QVector<qint32> chain = m.ancestors(symbol);
    chain.append(symbol);
    int cur = 0;
    if (m_mode == Files) {
        const qint32 owner = m.nodeOwner(symbol);
        const Location w = m.symbols[owner].where();
        const QModelIndex fi = indexOfFile(w.file);
        if (!fi.isValid())
            return {};
        cur = int(fi.internalId());
        // From the owner down
        const qsizetype at = chain.indexOf(owner);
        chain = chain.mid(at < 0 ? 0 : at);
    }
    for (qint32 s : std::as_const(chain)) {
        build(cur);
        int next = -1;
        for (int c : std::as_const(m_nodes[cur].children)) {
            if (m_nodes[c].symbol == s) {
                next = c;
                break;
            }
        }
        if (next < 0)
            break;
        cur = next;
    }
    if (cur == 0)
        return {};
    return createIndex(m_nodes[cur].row, 0, quintptr(cur));
}

// --- MetricsTable --------------------------------------------------------------------

MetricsTable::MetricsTable(QObject *parent)
    : QAbstractTableModel(parent)
{
    m_priority = new ReviewFocus(this);
    m_priority->setShowAll(true);
    connect(m_priority, &ReviewFocus::changed, this, [this] {
        if (m_mode == Functions || m_mode == Files)
            reload();
    });
    connect(Session::instance(), &Session::modelChanged, this, &MetricsTable::reload);
    connect(Hotspots::instance(), &Hotspots::changed, this, [this] {
        if (m_mode == Files)
            reload();
    });
    reload();
}

int MetricsTable::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

int MetricsTable::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_columns.size());
}

QHash<int, QByteArray> MetricsTable::roleNames() const
{
    return {{Qt::DisplayRole, "display"}, {ValueRole, "value"}, {FractionRole, "fraction"}, {SymbolRole, "symbol"},
            {FileRole, "file"}, {NumericRole, "numeric"}, {HotRole, "hot"}};
}

QStringList MetricsTable::headers() const
{
    QStringList out;
    for (const Column &c : m_columns)
        out << c.title;
    return out;
}

QVariantList MetricsTable::columnWidths() const
{
    QVariantList out;
    for (const Column &c : m_columns)
        out << c.width;
    return out;
}

QVariant MetricsTable::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < m_columns.size())
        return m_columns[section].title;
    return {};
}

QVariant MetricsTable::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size() || index.column() >= m_columns.size())
        return {};
    const Row &r = m_rows[index.row()];
    const int c = index.column();
    const Column &col = m_columns[c];
    const double v = r.values.value(c);
    const bool missing = std::isnan(v);
    switch (role) {
    case Qt::DisplayRole:
        if (c == 0)
            return r.name;
        if (!col.numeric)
            return r.location;
        if (m_mode == Files && (c == 9 || c == 10) && r.binaryHistory)
            return tr("Unknown (binary)");
        if (missing)
            return tr("Unavailable");
        if (m_mode == Files && c == 12)
            return v > 0 ? QDateTime::fromSecsSinceEpoch(qint64(v), QTimeZone::UTC).date().toString(Qt::ISODate) : tr("None");
        if (col.title == tr("Cohesion"))
            return QStringLiteral("%1%").arg(qRound(v * 100));
        if (std::floor(v) != v)
            return QString::number(v, 'f', 2);
        return QString::number(qint64(v));
    case ValueRole: return missing ? QVariant() : QVariant(v);
    case FractionRole: return !missing && col.numeric && c < m_max.size() && m_max[c] > 0 ? v / m_max[c] : 0.0;
    case SymbolRole: return r.symbol;
    case FileRole: return r.file;
    case NumericRole: return col.numeric && c > 0;
    case HotRole: return col.hot > 0 && v >= col.hot;
    }
    return {};
}

void MetricsTable::setMode(int m)
{
    if (m == m_mode)
        return;
    m_mode = m;
    m_sortColumn = m == Files || m == Modules ? 1 : 2;
    m_descending = true;
    reload();
    emit modeChanged();
    emit sortChanged();
}

void MetricsTable::setFilter(const QString &f)
{
    if (f == m_filter)
        return;
    m_filter = f;
    emit filterChanged();
    beginResetModel();
    applySort();
    endResetModel();
    emit countChanged();
}

void MetricsTable::sortBy(int column)
{
    if (column == m_sortColumn)
        m_descending = !m_descending;
    else {
        m_sortColumn = column;
        m_descending = column != 0 && m_columns.value(column).numeric;
    }
    beginResetModel();
    applySort();
    endResetModel();
    emit sortChanged();
}

void MetricsTable::reload()
{
    beginResetModel();
    m_model = Session::instance()->model();
    m_all.clear();
    m_columns.clear();
    auto col = [&](const QString &t, double hot = 0, int width = 86, bool numeric = true) {
        m_columns.append({t, numeric, hot, width});
    };
    const Analysis::Thresholds th;
    switch (m_mode) {
    case Functions:
        col(tr("Function"), 0, 300, false);
        col(tr("Lines"), th.lines);
        col(tr("Complexity"), th.complexity, 112);
        col(tr("Cognitive"), th.cognitive);
        col(tr("Nesting"), th.nesting, 76);
        col(tr("Params"), th.params, 76);
        col(tr("Statements"), 0, 96);
        col(tr("Callers"));
        col(tr("Callees"));
        col(tr("Location"), 0, 240, false);
        col(tr("Review priority"), 0, 130);
        break;
    case Classes:
        col(tr("Class"), 0, 280, false);
        col(tr("Methods"), th.classMethods);
        col(tr("Fields"));
        col(tr("WMC"), th.classWmc, 76);
        col(tr("Lines"));
        col(tr("Depth"), th.inheritance, 76);
        col(tr("Subclasses"), 0, 96);
        col(tr("Uses"), th.classCoupling, 76);
        col(tr("Used by"));
        col(tr("Cohesion"), 0, 90);
        col(tr("Location"), 0, 240, false);
        break;
    case Files:
        col(tr("File"), 0, 320, false);
        col(tr("Lines"), th.fileLines);
        col(tr("Code"));
        col(tr("Symbols"));
        col(tr("Includes"));
        col(tr("Included by"), 0, 100);
        col(tr("Complexity"), 0, 96);
        col(tr("Errors"), 1, 76);
        col(tr("Commits"), 0, 90);
        col(tr("Added"), 0, 90);
        col(tr("Removed"), 0, 90);
        col(tr("Authors"), 0, 90);
        col(tr("Last change (UTC)"), 0, 150);
        col(tr("Max review priority"), 0, 160);
        break;
    case Modules:
        col(tr("Module"), 0, 260, false);
        col(tr("Files"), 0, 76);
        col(tr("Lines"));
        col(tr("Classes"));
        col(tr("Functions"), 0, 96);
        col(tr("Ca"), 0, 64);
        col(tr("Ce"), 0, 64);
        col(tr("Instability"), 0, 100);
        col(tr("Abstractness"), 0, 110);
        col(tr("Distance"), 0.7, 90);
        break;
    }
    if (m_model) {
        const CodeModel &m = *m_model;
        auto reviewable = [&](const Symbol &s) {
            if (s.has(External) || s.has(Generated) || s.has(FromMacro) || s.has(Implicit))
                return false;
            const Location w = s.where();
            return w.file >= 0 && m.files[w.file].project && !m.files[w.file].generated;
        };
        if (m_mode == Functions) {
            for (int i = 0; i < m.symbols.size(); ++i) {
                const Symbol &s = m.symbols[i];
                if (!isFunction(s.kind) || !s.def.valid() || !reviewable(s))
                    continue;
                const FunctionMetrics &mm = s.metrics;
                Row r;
                r.symbol = i;
                r.file = s.def.file;
                r.name = m.displayName(i);
                r.location = m.locationText(s.def);
                r.values = {0, double(mm.lines), double(mm.complexity), double(mm.cognitive), double(mm.nesting),
                            double(mm.params), double(mm.statements), double(s.fanIn), double(s.fanOut), 0};
                const double priority = m_priority->priorityFor(i);
                r.values.append(priority < 0 ? std::numeric_limits<double>::quiet_NaN() : priority);
                m_all.append(r);
            }
        } else if (m_mode == Classes) {
            for (int i = 0; i < m.symbols.size(); ++i) {
                const Symbol &s = m.symbols[i];
                if ((s.kind != SymbolKind::Class && s.kind != SymbolKind::Struct && s.kind != SymbolKind::Union)
                    || !s.has(Defined) || !reviewable(s))
                    continue;
                const Analysis::ClassMetrics c = Analysis::classMetrics(m, i);
                Row r;
                r.symbol = i;
                r.file = s.where().file;
                r.name = m.displayName(i);
                r.location = m.locationText(s.where());
                r.values = {0, double(c.methods), double(c.fields), double(c.wmc), double(c.lines), double(c.depth),
                            double(c.children), double(c.coupling), double(c.usedBy), c.cohesion, 0};
                m_all.append(r);
            }
        } else if (m_mode == Files) {
            QVector<int> errors(m.files.size(), 0), complexity(m.files.size(), 0);
            for (const Diagnostic &d : m.diagnostics) {
                if (d.loc.file >= 0 && d.severity >= Diagnostic::Error)
                    errors[d.loc.file]++;
            }
            for (const Symbol &s : m.symbols) {
                if (isFunction(s.kind) && s.def.valid())
                    complexity[s.def.file] += s.metrics.complexity;
            }
            for (int f = 0; f < m.files.size(); ++f) {
                const File &file = m.files[f];
                if (!file.project)
                    continue;
                Row r;
                r.file = f;
                r.name = file.relative;
                r.location = file.relative;
                r.values = {0, double(file.lines), double(file.codeLines), double(file.symbols.size()),
                            double(file.includes.size()), double(file.includedBy.size()), double(complexity[f]),
                            double(errors[f])};
                const auto *activity = Hotspots::instance()->forFile(file.path);
                const double unknown = std::numeric_limits<double>::quiet_NaN();
                r.values.append(activity ? double(activity->commits) : unknown);
                r.values.append(activity && !activity->binary ? double(activity->added) : unknown);
                r.values.append(activity && !activity->binary ? double(activity->removed) : unknown);
                r.values.append(activity ? double(activity->authors.size()) : unknown);
                r.values.append(activity ? double(activity->lastChanged) : unknown);
                const double priority = m_priority->filePriority(f);
                r.values.append(priority < 0 ? unknown : priority);
                r.binaryHistory = activity && activity->binary;
                m_all.append(r);
            }
        } else {
            for (const Analysis::ModuleMetrics &mm : Analysis::moduleMetrics(m)) {
                if (mm.name.startsWith(QLatin1Char('<')))
                    continue;
                Row r;
                r.name = mm.name;
                r.values = {0, double(mm.files), double(mm.lines), double(mm.classes), double(mm.functions),
                            double(mm.afferent), double(mm.efferent), mm.instability, mm.abstractness, mm.distance};
                m_all.append(r);
            }
        }
    }
    m_max = QVector<double>(m_columns.size(), 0);
    for (const Row &r : std::as_const(m_all)) {
        for (int c = 0; c < r.values.size() && c < m_max.size(); ++c)
            m_max[c] = std::max(m_max[c], r.values[c]);
    }
    applySort();
    endResetModel();
    emit countChanged();
}

void MetricsTable::applySort()
{
    m_rows.clear();
    for (const Row &r : std::as_const(m_all)) {
        if (!m_filter.isEmpty() && !r.name.contains(m_filter, Qt::CaseInsensitive)
            && !r.location.contains(m_filter, Qt::CaseInsensitive))
            continue;
        m_rows.append(r);
    }
    const int c = m_sortColumn;
    const bool numeric = c > 0 && c < m_columns.size() && m_columns[c].numeric;
    std::stable_sort(m_rows.begin(), m_rows.end(), [&](const Row &a, const Row &b) {
        if (numeric) {
            const double x = a.values.value(c), y = b.values.value(c);
            if (std::isnan(x) != std::isnan(y))
                return !std::isnan(x);
            if (std::isnan(x))
                return a.name < b.name;
            if (x != y)
                return m_descending ? x > y : x < y;
            return a.name < b.name;
        }
        const QString &x = c == 0 ? a.name : a.location, &y = c == 0 ? b.name : b.location;
        return m_descending ? x > y : x < y;
    });
}

QVariantMap MetricsTable::rowInfo(int row) const
{
    QVariantMap out;
    if (row < 0 || row >= m_rows.size())
        return out;
    out[QStringLiteral("symbol")] = m_rows[row].symbol;
    out[QStringLiteral("file")] = m_rows[row].file;
    out[QStringLiteral("name")] = m_rows[row].name;
    return out;
}

QString MetricsTable::toCsv() const
{
    QString out;
    QStringList columns = headers();
    QString historyScope;
    if (m_mode == Files) {
        columns << tr("Git history scope");
        const auto &activity = Hotspots::instance()->activity();
        if (Hotspots::instance()->ready()) {
            historyScope = tr("revision=%1; since=%2; until=%3; %4; non-merge commits; %5")
                .arg(activity.revision,
                     QDateTime::fromSecsSinceEpoch(activity.since, QTimeZone::UTC).toString(Qt::ISODate),
                     QDateTime::fromSecsSinceEpoch(activity.until, QTimeZone::UTC).toString(Qt::ISODate),
                     activity.shallow ? tr("partial: shallow clone") : tr("complete for selected history scope"),
                     activity.followedRenames ? tr("Git --follow lineage (heuristic)") : tr("pathname history; renames not followed"));
        } else {
            historyScope = Hotspots::instance()->summary();
        }
        if (m_mode == Files || m_mode == Functions)
            columns << tr("Review priority scope");
    }
    auto quoted = [](QString v) {
        if (v.contains(QLatin1Char(',')) || v.contains(QLatin1Char('"')) || v.contains(QLatin1Char('\n'))
            || v.contains(QLatin1Char('\r')))
            v = QLatin1Char('"') + v.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
        return v;
    };
    for (QString &column : columns)
        column = quoted(column);
    out += columns.join(QLatin1Char(',')) + QLatin1Char('\n');
    for (int r = 0; r < m_rows.size(); ++r) {
        QStringList cells;
        for (int c = 0; c < m_columns.size(); ++c) {
            cells << quoted(data(index(r, c), Qt::DisplayRole).toString());
        }
        if (m_mode == Files)
            cells << quoted(historyScope);
        if (m_mode == Files || m_mode == Functions)
            cells << quoted(prioritySummary());
        out += cells.join(QLatin1Char(',')) + QLatin1Char('\n');
    }
    return out;
}

QString MetricsTable::prioritySummary() const
{
    return m_priority ? m_priority->summary() : tr("Review priority unavailable");
}

// --- FindingsList ----------------------------------------------------------------------

FindingsList::FindingsList(QObject *parent)
    : QAbstractListModel(parent)
{
    connect(Session::instance(), &Session::modelChanged, this, &FindingsList::reload);
    reload();
}

int FindingsList::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> FindingsList::roleNames() const
{
    return {{KindRole, "kind"}, {KindNameRole, "kindName"}, {SeverityRole, "severity"}, {TitleRole, "title"},
            {DetailRole, "detail"}, {LocationRole, "location"}, {SymbolRole, "symbol"}, {FileRole, "file"},
            {LineRole, "line"}, {SymbolsRole, "symbols"}, {IndexRole, "findingIndex"}};
}

QVariant FindingsList::data(const QModelIndex &index, int role) const
{
    if (!m_model || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Analysis::Finding &f = Session::instance()->report().findings[m_rows[index.row()]];
    switch (role) {
    case KindRole: return int(f.kind);
    case KindNameRole: return Analysis::findingKindName(f.kind);
    case SeverityRole: return f.severity;
    case TitleRole: return f.title;
    case DetailRole: return f.detail;
    case LocationRole: return m_model->locationText(f.loc);
    case SymbolRole: return f.symbols.isEmpty() ? -1 : f.symbols.first();
    case FileRole: return f.loc.file >= 0 ? f.loc.file : (f.files.isEmpty() ? -1 : f.files.first());
    case LineRole: return f.loc.line;
    case SymbolsRole: {
        QVariantList l;
        for (qint32 s : f.symbols)
            l << s;
        return l;
    }
    case IndexRole: return m_rows[index.row()];
    }
    return {};
}

QVariantList FindingsList::kinds() const
{
    QVariantList out;
    const Analysis::Report &rep = Session::instance()->report();
    for (int k = 0; k < int(Analysis::FindingKind::Count); ++k) {
        if (rep.perKind[k] == 0)
            continue;
        int serious = 0;
        for (const Analysis::Finding &f : rep.findings) {
            if (int(f.kind) == k && f.severity >= 2)
                serious++;
        }
        QVariantMap e;
        e[QStringLiteral("kind")] = k;
        e[QStringLiteral("name")] = Analysis::findingKindName(Analysis::FindingKind(k));
        e[QStringLiteral("hint")] = Analysis::findingKindHint(Analysis::FindingKind(k));
        e[QStringLiteral("count")] = rep.perKind[k];
        e[QStringLiteral("serious")] = serious;
        out << e;
    }
    return out;
}

QString FindingsList::hint(int kind) const
{
    return Analysis::findingKindHint(Analysis::FindingKind(kind));
}

void FindingsList::setKind(int k)
{
    if (k == m_kind)
        return;
    m_kind = k;
    reload();
}

void FindingsList::setMinSeverity(int s)
{
    if (s == m_minSeverity)
        return;
    m_minSeverity = s;
    reload();
}

void FindingsList::setFilter(const QString &f)
{
    if (f == m_filter)
        return;
    m_filter = f;
    reload();
}

QVariantMap FindingsList::get(int row) const
{
    QVariantMap out;
    const QModelIndex i = index(row, 0);
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        out[QString::fromLatin1(it.value())] = data(i, it.key());
    return out;
}

void FindingsList::reload()
{
    beginResetModel();
    m_model = Session::instance()->model();
    m_rows.clear();
    if (m_model) {
        const Analysis::Report &rep = Session::instance()->report();
        for (int i = 0; i < rep.findings.size(); ++i) {
            const Analysis::Finding &f = rep.findings[i];
            if (m_kind >= 0 && int(f.kind) != m_kind)
                continue;
            if (f.severity < m_minSeverity)
                continue;
            if (!m_filter.isEmpty() && !f.title.contains(m_filter, Qt::CaseInsensitive)
                && !f.detail.contains(m_filter, Qt::CaseInsensitive))
                continue;
            m_rows.append(i);
        }
    }
    endResetModel();
    emit changed();
}

// --- CallTree ----------------------------------------------------------------------------

CallTree::CallTree(QObject *parent)
    : QAbstractItemModel(parent)
{
    connect(Session::instance(), &Session::modelChanged, this, &CallTree::reset);
    reset();
}

void CallTree::setRoot(int s)
{
    if (s == m_root)
        return;
    m_root = s;
    reset();
    emit rootChanged();
}

void CallTree::setCallers(bool v)
{
    if (v == m_callers)
        return;
    m_callers = v;
    reset();
    emit rootChanged();
}

void CallTree::setShowImplicit(bool v)
{
    if (v == m_showImplicit)
        return;
    m_showImplicit = v;
    reset();
    emit rootChanged();
}

void CallTree::setShowExternal(bool v)
{
    if (v == m_showExternal)
        return;
    m_showExternal = v;
    reset();
    emit rootChanged();
}

void CallTree::reset()
{
    beginResetModel();
    m_model = Session::instance()->model();
    m_nodes.clear();
    Node top;   // invisible
    top.built = true;
    m_nodes.append(top);
    if (m_model && m_root >= 0 && m_root < m_model->symbols.size()) {
        Node r;
        r.parent = 0;
        r.symbol = m_root;
        m_nodes.append(r);
        m_nodes[0].children.append(1);
    }
    endResetModel();
}

void CallTree::build(int node) const
{
    Node &n = m_nodes[node];
    if (n.built || !m_model || n.recursive)
        return;
    n.built = true;
    const CodeModel &m = *m_model;
    QSet<int> path;
    for (int p = node; p > 0; p = m_nodes[p].parent)
        path.insert(m_nodes[p].symbol);
    const int sym = n.symbol;
    auto add = [&](int s, int ref, int count, bool dispatch) {
        if (!m_showExternal && m.symbols[s].has(External))
            return;
        Node c;
        c.parent = node;
        c.symbol = s;
        c.ref = ref;
        c.count = count;
        c.dispatch = dispatch;
        c.recursive = path.contains(s);
        c.row = int(m_nodes[node].children.size());
        m_nodes.append(c);
        m_nodes[node].children.append(int(m_nodes.size() - 1));
    };
    // A class: its methods, each with its calls (or callers) below
    if (isType(m.symbols[sym].kind)) {
        for (qint32 f : Analysis::methodsOf(m, sym))
            add(f, -1, 1, false);
        return;
    }
    if (!m_callers) {
        // A virtual method may run any override
        if (m.symbols[sym].has(Virtual) && n.dispatch == false) {
            for (qint32 o : Analysis::overriders(m, sym))
                add(o, -1, 1, true);
        }
        for (const Analysis::CallSite &c : Analysis::callsFrom(m, sym, m_showImplicit))
            add(c.callee, c.ref, c.count, false);
    } else {
        for (const Analysis::CallSite &c : Analysis::callersOf(m, sym, m_showImplicit))
            add(c.callee, c.ref, c.count, false);
        // Calls to the method it overrides may land here
        for (qint32 e : m.outEdges[sym]) {
            if (m.edges[e].kind == RefKind::Override)
                add(m.edges[e].to, -1, 1, true);
        }
    }
}

QModelIndex CallTree::index(int row, int column, const QModelIndex &parent) const
{
    const int p = parent.isValid() ? int(parent.internalId()) : 0;
    if (p < 0 || p >= m_nodes.size())
        return {};
    build(p);
    if (row < 0 || row >= m_nodes[p].children.size() || column != 0)
        return {};
    return createIndex(row, column, quintptr(m_nodes[p].children[row]));
}

QModelIndex CallTree::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};
    const int p = m_nodes[int(child.internalId())].parent;
    if (p <= 0)
        return {};
    return createIndex(m_nodes[p].row, 0, quintptr(p));
}

int CallTree::rowCount(const QModelIndex &parent) const
{
    const int p = parent.isValid() ? int(parent.internalId()) : 0;
    if (p < 0 || p >= m_nodes.size())
        return 0;
    build(p);
    return int(m_nodes[p].children.size());
}

int CallTree::columnCount(const QModelIndex &) const
{
    return 1;
}

bool CallTree::hasChildren(const QModelIndex &parent) const
{
    const int p = parent.isValid() ? int(parent.internalId()) : 0;
    if (p < 0 || p >= m_nodes.size())
        return false;
    if (p == 0)
        return !m_nodes[0].children.isEmpty();
    const Node &n = m_nodes[p];
    if (n.built)
        return !n.children.isEmpty();
    if (n.recursive || !m_model)
        return false;
    const CodeModel &m = *m_model;
    if (isType(m.symbols[n.symbol].kind))
        return !Analysis::methodsOf(m, n.symbol).isEmpty();
    if (!m_callers) {
        if (m.symbols[n.symbol].has(Virtual))
            return true;
        for (qint32 e : m.outEdges[n.symbol]) {
            if (m.edges[e].kind == RefKind::Call)
                return true;
        }
        return false;
    }
    for (qint32 e : m.inEdges[n.symbol]) {
        if (m.edges[e].kind == RefKind::Call)
            return true;
    }
    return false;
}

QHash<int, QByteArray> CallTree::roleNames() const
{
    return {{Qt::DisplayRole, "display"}, {NameRole, "name"}, {SymbolRole, "symbol"}, {KindRole, "kind"},
            {LineRole, "line"}, {CountRole, "count"}, {RecursiveRole, "recursive"}, {VirtualRole, "isVirtual"},
            {ExternalRole, "external"}, {RefRole, "ref"}, {DetailRole, "detail"}, {DispatchRole, "dispatch"},
            {ComplexityRole, "complexity"}};
}

QVariant CallTree::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !m_model)
        return {};
    const Node &n = m_nodes[int(index.internalId())];
    const CodeModel &m = *m_model;
    const Symbol &s = m.symbols[n.symbol];
    switch (role) {
    case Qt::DisplayRole:
    case NameRole: {
        const qint32 owner = m.nodeOwner(n.symbol);
        if (owner != n.symbol)
            return m.symbols[owner].name + QStringLiteral("::") + s.name;
        return s.name;
    }
    case SymbolRole: return n.symbol;
    case KindRole: return int(s.kind);
    case LineRole: return n.ref >= 0 ? m.refs[n.ref].loc.line : 0;
    case CountRole: return n.count;
    case RecursiveRole: return n.recursive;
    case VirtualRole: return s.has(Virtual);
    case ExternalRole: return s.has(External);
    case RefRole: return n.ref;
    case DispatchRole: return n.dispatch;
    case ComplexityRole: return s.metrics.complexity;
    case DetailRole: {
        if (n.ref >= 0)
            return m.locationText(m.refs[n.ref].loc);
        return m.locationText(s.where());
    }
    }
    return {};
}

// --- CodeSnippets ----------------------------------------------------------------------

CodeSnippets::CodeSnippets(QObject *parent)
    : QObject(parent)
{
    connect(Session::instance(), &Session::modelChanged, this, [this] {
        m_texts.clear();
        m_textOrder.clear();
        schedule();
    });
    connect(this, &CodeSnippets::inputChanged, this, &CodeSnippets::schedule);
    connect(Changes::instance(), &Changes::changesChanged, this, &CodeSnippets::schedule);
}

void CodeSnippets::setSymbol(int s)
{
    if (s == m_symbol)
        return;
    m_symbol = s;
    emit inputChanged();
}

void CodeSnippets::setFile(int f)
{
    if (f == m_file)
        return;
    m_file = f;
    emit inputChanged();
}

void CodeSnippets::setLine(int l)
{
    if (l == m_line)
        return;
    m_line = l;
    emit inputChanged();
}

void CodeSnippets::setWholeFile(bool v)
{
    if (v == m_wholeFile)
        return;
    m_wholeFile = v;
    emit inputChanged();
}

void CodeSnippets::setContext(int c)
{
    if (c == m_context)
        return;
    m_context = c;
    emit inputChanged();
}

void CodeSnippets::setColors(const QVariantMap &c)
{
    if (c == m_colors)
        return;
    m_colors = c;
    emit inputChanged();
}

void CodeSnippets::schedule()
{
    if (m_pending)
        return;
    m_pending = true;
    QTimer::singleShot(0, this, [this] {
        m_pending = false;
        rebuild();
    });
}

const CodeSnippets::FileText *CodeSnippets::text(int file)
{
    auto it = m_texts.constFind(file);
    if (it != m_texts.cend())
        return &*it;
    if (!m_model || file < 0 || file >= m_model->files.size())
        return nullptr;
    QFile f(m_model->files[file].path);
    if (!f.open(QIODevice::ReadOnly))
        return nullptr;
    const QByteArray bytes = f.readAll();
    FileText t;
    const QList<QByteArray> raw = bytes.split('\n');
    QString all;
    QVector<qsizetype> starts;
    for (const QByteArray &line : raw) {
        QByteArray l = line;
        if (l.endsWith('\r'))
            l.chop(1);
        t.raw.append(l);
        const QString s = QString::fromUtf8(l);
        starts.append(all.size());
        t.lines.append(s);
        all += s + QLatin1Char('\n');
    }
    t.spans = QVector<QVector<QPair<int, int>>>(t.lines.size());
    t.kinds = QVector<QVector<int>>(t.lines.size());
    // The language by the file's suffix: Python, else C or C++
    const QString suffix = QFileInfo(m_model->files[file].path).suffix().toLower();
    const QString language = suffix == QLatin1String("py") || suffix == QLatin1String("pyi") ? QStringLiteral("python")
                                                                                            : QStringLiteral("cpp");
    const QList<Highlight::Span> spans = Highlight::spans(all, language);
    int line = 0;
    for (const Highlight::Span &sp : spans) {
        qsizetype start = sp.start, end = sp.start + sp.length;
        while (line + 1 < starts.size() && starts[line + 1] <= start)
            ++line;
        int l = line;
        while (start < end && l < starts.size()) {
            const qsizetype lineEnd = starts[l] + t.lines[l].size();
            const qsizetype segEnd = std::min(end, lineEnd);
            if (segEnd > start) {
                t.spans[l].append({int(start - starts[l]), int(segEnd - start)});
                t.kinds[l].append(int(sp.kind));
            }
            ++l;
            if (l < starts.size())
                start = std::max(start, starts[l]);
            else
                break;
        }
    }
    if (m_textOrder.size() > 40) {
        m_texts.remove(m_textOrder.takeFirst());
    }
    m_textOrder.append(file);
    return &*m_texts.insert(file, t);
}

QString CodeSnippets::lineHtml(int file, int line, const FileText &t)
{
    const QString &s = t.lines.value(line - 1);
    const QByteArray &raw = t.raw.value(line - 1);
    const qsizetype n = s.size();
    QVector<int> color(n, -1), link(n, -1);
    QVector<char> mark(n, 0);
    static const char *kindKeys[] = {"keyword", "type", "string", "number", "comment", "preprocessor", "function"};
    const auto &spans = t.spans.value(line - 1);
    const auto &kinds = t.kinds.value(line - 1);
    for (qsizetype i = 0; i < spans.size(); ++i) {
        for (int k = spans[i].first; k < spans[i].first + spans[i].second && k < n; ++k)
            color[k] = kinds[i];
    }
    // The indexer's references on this line
    const CodeModel &m = *m_model;
    if (file < m.refsInFile.size()) {
        const QVector<qint32> &list = m.refsInFile[file];
        auto it = std::lower_bound(list.cbegin(), list.cend(), line, [&](qint32 r, int l) { return m.refs[r].loc.line < l; });
        for (; it != list.cend() && m.refs[*it].loc.line == line; ++it) {
            const Reference &r = m.refs[*it];
            if (r.to < 0 || (r.flags & RefImplicit))
                continue;
            const int byteCol = std::max(0, r.loc.col - 1);
            const qsizetype at = QString::fromUtf8(raw.left(byteCol)).size();
            if (at >= n)
                continue;
            qsizetype end = at;
            if (s[end] == QLatin1Char('~'))
                ++end;
            while (end < n && (s[end].isLetterOrNumber() || s[end] == QLatin1Char('_')))
                ++end;
            if (end == at)
                continue;
            for (qsizetype k = at; k < end; ++k) {
                if (link[k] < 0)
                    link[k] = r.to;
                if (r.to == m_symbol)
                    mark[k] = 1;
            }
        }
    }
    const QString markColor = m_colors.value(QStringLiteral("mark")).toString();
    const QString linkColor = m_colors.value(QStringLiteral("symbol")).toString();
    QString out;
    qsizetype i = 0;
    while (i < n) {
        const int c = color[i], l = link[i];
        const char mk = mark[i];
        qsizetype j = i + 1;
        while (j < n && color[j] == c && link[j] == l && mark[j] == mk)
            ++j;
        QString chunk;
        for (qsizetype k = i; k < j; ++k) {
            const QChar ch = s[k];
            if (ch == QLatin1Char('\t'))
                chunk += QStringLiteral("    ");
            else if (ch == QLatin1Char('<'))
                chunk += QStringLiteral("&lt;");
            else if (ch == QLatin1Char('>'))
                chunk += QStringLiteral("&gt;");
            else if (ch == QLatin1Char('&'))
                chunk += QStringLiteral("&amp;");
            else
                chunk += ch;
        }
        QString colorName = c >= 0 && c < 7 ? m_colors.value(QString::fromLatin1(kindKeys[c])).toString() : QString();
        if (l >= 0 && colorName.isEmpty())
            colorName = linkColor;
        if (mk)
            chunk = QStringLiteral("<span style=\"background-color:%1;\">%2</span>").arg(markColor, chunk);
        if (l >= 0) {
            out += QStringLiteral("<a href=\"s:%1\" style=\"color:%2; text-decoration:none;\">%3</a>")
                       .arg(l)
                       .arg(colorName.isEmpty() ? m_colors.value(QStringLiteral("text")).toString() : colorName, chunk);
        } else if (!colorName.isEmpty()) {
            out += QStringLiteral("<span style=\"color:%1;\">%2</span>").arg(colorName, chunk);
        } else {
            out += chunk;
        }
        i = j;
    }
    return out;
}

QVariantMap CodeSnippets::snippet(int file, int from, int to, const QString &title, const QSet<int> &markLines,
                                  bool definition)
{
    QVariantMap out;
    const FileText *t = text(file);
    if (!t)
        return out;
    from = std::max(1, from);
    to = std::min(int(t->lines.size()), to);
    if (to < from)
        return out;
    const QString numberColor = m_colors.value(QStringLiteral("lineNumber")).toString();
    const QString markerColor = m_colors.value(QStringLiteral("marker")).toString();
    const QString changedColor = m_colors.value(QStringLiteral("changed")).toString();
    // Lines the selected commits changed get a bar in the gutter
    const QSet<qint32> *changed = Changes::instance()->changedLines(file);
    const int digits = int(QString::number(to).size());
    QString html;
    html.reserve((to - from + 1) * 120);
    for (int l = from; l <= to; ++l) {
        const bool marked = markLines.contains(l);
        const bool isChanged = changed && changed->contains(l);
        html += QStringLiteral("<span style=\"color:%1;\">%2</span>")
                    .arg(isChanged ? changedColor : marked ? markerColor : numberColor,
                         QString::number(l).rightJustified(digits, QLatin1Char(' ')).replace(QLatin1Char(' '), QStringLiteral("&nbsp;"))
                             + (isChanged ? QStringLiteral("▍") : QStringLiteral(" ")));
        html += QStringLiteral("<span style=\"color:%1;\">%2</span>")
                    .arg(markerColor, marked ? QStringLiteral("▸ ") : QStringLiteral("&nbsp; "));
        html += lineHtml(file, l, *t);
        if (l < to)
            html += QStringLiteral("<br>");
    }
    const File &f = m_model->files[file];
    out[QStringLiteral("file")] = file;
    out[QStringLiteral("path")] = f.relative;
    out[QStringLiteral("name")] = QFileInfo(f.path).fileName();
    out[QStringLiteral("title")] = title;
    out[QStringLiteral("from")] = from;
    out[QStringLiteral("to")] = to;
    out[QStringLiteral("html")] = QStringLiteral("<div style=\"white-space:pre;\">") + html + QStringLiteral("</div>");
    out[QStringLiteral("definition")] = definition;
    out[QStringLiteral("marks")] = int(markLines.size());
    out[QStringLiteral("lines")] = to - from + 1;
    return out;
}

QVariantMap CodeSnippets::fileSnippet(int file, int focusLine)
{
    m_model = Session::instance()->model();
    if (!m_model)
        return {};
    const FileText *t = text(file);
    if (!t)
        return {};
    QSet<int> marks;
    if (focusLine > 0)
        marks.insert(focusLine);
    if (m_symbol >= 0) {
        for (qint32 r : m_model->refsTo[m_symbol]) {
            if (m_model->refs[r].loc.file == file)
                marks.insert(m_model->refs[r].loc.line);
        }
    }
    const int total = int(t->lines.size());
    const int window = 1500;
    const int from = std::max(1, std::min(focusLine - 500, total - window + 1));
    const int to = std::min(total, from + window - 1);
    QString title = m_model->files[file].relative;
    if (from > 1 || to < total)
        title += tr("  ·  lines %1–%2 of %3").arg(from).arg(to).arg(total);
    QVariantMap s = snippet(file, from, to, title, marks, false);
    s[QStringLiteral("focusLine")] = focusLine;
    return s;
}

void CodeSnippets::rebuild()
{
    m_model = Session::instance()->model();
    m_snippets.clear();
    m_referenceCount = m_fileCount = m_hiddenCount = 0;
    if (!m_model) {
        emit snippetsChanged();
        return;
    }
    const CodeModel &m = *m_model;
    if (m_symbol < 0 || m_symbol >= m.symbols.size()) {
        if (m_file >= 0 && m_file < m.files.size())
            m_snippets << fileSnippet(m_file, m_line);
        m_fileCount = m_snippets.isEmpty() ? 0 : 1;
        emit snippetsChanged();
        return;
    }
    const Symbol &s = m.symbols[m_symbol];
    if (m_wholeFile) {
        const Location w = s.where();
        if (w.file >= 0)
            m_snippets << fileSnippet(w.file, w.line);
        m_fileCount = 1;
        emit snippetsChanged();
        return;
    }
    // Ranges already shown, per file
    QHash<int, QVector<QPair<int, int>>> shown;
    auto covered = [&](int file, int line) {
        for (const auto &[a, b] : shown.value(file)) {
            if (line >= a && line <= b)
                return true;
        }
        return false;
    };
    QSet<int> files;
    int totalLines = 0;
    const int maxDefLines = 90;
    if (s.def.valid()) {
        const int to = std::min(s.def.endLine > 0 ? s.def.endLine : s.def.line, s.def.line + maxDefLines);
        QSet<int> marks = {s.decl.file == s.def.file ? s.decl.line : s.def.line};
        QVariantMap sn = snippet(s.def.file, s.def.line, to, tr("Definition"), marks, true);
        if (!sn.isEmpty()) {
            if (s.def.endLine > to)
                sn[QStringLiteral("truncated")] = s.def.endLine - to;
            m_snippets << sn;
            shown[s.def.file].append({s.def.line, to});
            files.insert(s.def.file);
            totalLines += to - s.def.line + 1;
        }
    }
    if (s.decl.valid() && !covered(s.decl.file, s.decl.line)) {
        QVariantMap sn = snippet(s.decl.file, s.decl.line - m_context, s.decl.line + m_context,
                                 tr("Declaration"), {s.decl.line}, !s.def.valid());
        if (!sn.isEmpty()) {
            m_snippets << sn;
            shown[s.decl.file].append({s.decl.line - m_context, s.decl.line + m_context});
            files.insert(s.decl.file);
        }
    }
    // Uses, by file (the definition's first), merged where they are close
    QMap<int, QVector<qint32>> byFile;
    for (qint32 r : m.refsTo[m_symbol]) {
        const Reference &ref = m.refs[r];
        if (ref.flags & RefImplicit)
            continue;
        byFile[ref.loc.file].append(r);
        m_referenceCount++;
    }
    QVector<int> fileOrder = byFile.keys().toVector();
    std::sort(fileOrder.begin(), fileOrder.end(), [&](int a, int b) {
        const bool da = a == s.def.file, db = b == s.def.file;
        if (da != db)
            return da;
        return m.files[a].relative < m.files[b].relative;
    });
    for (int file : std::as_const(fileOrder)) {
        QVector<qint32> refs = byFile.value(file);
        std::sort(refs.begin(), refs.end(), [&](qint32 a, qint32 b) { return m.refs[a].loc.line < m.refs[b].loc.line; });
        int i = 0;
        while (i < refs.size()) {
            const int line = m.refs[refs[i]].loc.line;
            if (covered(file, line)) {
                ++i;
                continue;
            }
            int from = line - m_context, to = line + m_context;
            QSet<int> marks = {line};
            int j = i + 1;
            while (j < refs.size() && m.refs[refs[j]].loc.line - m_context <= to + 1) {
                to = std::max(to, m.refs[refs[j]].loc.line + m_context);
                marks.insert(m.refs[refs[j]].loc.line);
                ++j;
            }
            if (m_snippets.size() >= 120 || totalLines > 3000) {
                m_hiddenCount += j - i;
                i = j;
                continue;
            }
            const qint32 in = m.refs[refs[i]].from;
            QString title = in >= 0 ? m.displayName(in) : tr("file level");
            const QStringList kinds = [&] {
                QSet<QString> k;
                for (int x = i; x < j; ++x)
                    k.insert(refKindName(m.refs[refs[x]].kind));
                return QStringList(k.cbegin(), k.cend());
            }();
            QVariantMap sn = snippet(file, from, to, title, marks, false);
            if (!sn.isEmpty()) {
                sn[QStringLiteral("kinds")] = kinds.join(QStringLiteral(", "));
                sn[QStringLiteral("container")] = in;
                m_snippets << sn;
                shown[file].append({from, to});
                files.insert(file);
                totalLines += to - from + 1;
            }
            i = j;
        }
    }
    m_fileCount = int(files.size());
    emit snippetsChanged();
}
