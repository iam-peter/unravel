#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "history.h"
#include "model.h"

// The project's git history and the code a selection of commits changed:
// the commits to pick from, and for the picked ones the files, lines and
// symbols they touched, mapped onto the current index. The views use it
// to show only that code; the code view marks the changed lines.
class Changes : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool available READ available NOTIFY repositoryChanged)
    Q_PROPERTY(QString repository READ repository NOTIFY repositoryChanged)
    Q_PROPERTY(QVariantList commits READ commits NOTIFY commitsChanged)
    Q_PROPERTY(QStringList selected READ selected NOTIFY selectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool active READ active NOTIFY changesChanged)
    Q_PROPERTY(QVariantList symbols READ symbols NOTIFY changesChanged)
    Q_PROPERTY(QVariantList files READ files NOTIFY changesChanged)
    Q_PROPERTY(QVariantList findings READ findings NOTIFY changesChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY changesChanged)
    Q_PROPERTY(QString error READ error NOTIFY changesChanged)
    Q_PROPERTY(int revision READ revision NOTIFY changesChanged)

public:
    // Not default-constructible, so QML takes the one create() hands out
    explicit Changes(QObject *parent);
    static Changes *instance();
    static Changes *create(QQmlEngine *, QJSEngine *)
    {
        Changes *c = instance();
        QJSEngine::setObjectOwnership(c, QJSEngine::CppOwnership);
        return c;
    }

    bool available() const { return !m_repo.isEmpty(); }
    QString repository() const { return m_repo; }
    QVariantList commits() const { return m_commitList; }
    QStringList selected() const { return m_selected; }
    bool busy() const { return m_busy; }
    bool active() const { return !m_touched.isEmpty() || !m_lines.isEmpty(); }
    QVariantList symbols() const { return m_symbolList; }
    QVariantList files() const { return m_fileList; }
    QVariantList findings() const { return m_findingList; }
    QString summary() const { return m_summary; }
    QString error() const { return m_error; }
    int revision() const { return m_revision; }

    // For C++: what the selection touched, by symbol id; the changed lines of a file
    const QHash<qint32, Unravel::History::Touched> &touched() const { return m_touched; }
    bool isTouched(qint32 symbol) const { return m_touched.contains(symbol); }
    const QSet<qint32> *changedLines(qint32 file) const
    {
        const auto it = m_lines.constFind(file);
        return it == m_lines.cend() ? nullptr : &*it;
    }

    Q_INVOKABLE void refresh();
    // Pick one commit, add or remove one (Ctrl), or all between two rows (Shift)
    Q_INVOKABLE void select(const QString &hash);
    Q_INVOKABLE void toggle(const QString &hash);
    Q_INVOKABLE void selectRange(int fromRow, int toRow);
    Q_INVOKABLE void clear();
    // Hashes, their prefixes or "worktree"
    Q_INVOKABLE void selectRevisions(const QStringList &revisions);
    Q_INVOKABLE bool isTouchedSymbol(int symbol) const { return isTouched(symbol); }
    // The change for a review: commits, files, symbols and findings in Markdown
    Q_INVOKABLE QString markdown() const;

signals:
    void repositoryChanged();
    void commitsChanged();
    void selectionChanged();
    void busyChanged();
    void changesChanged();

private:
    struct Result {
        quint64 generation = 0;
        Unravel::History::ChangeSet changes;
        Unravel::History::Mapping mapping;
    };
    void modelChanged();
    void setSelection(const QStringList &hashes);
    void compute();
    void computed();
    void updateCommitList();

    QString m_repo;
    QVector<Unravel::History::Commit> m_log;
    QVariantList m_commitList;
    QStringList m_selected;
    bool m_busy = false;
    QHash<qint32, Unravel::History::Touched> m_touched;
    QHash<qint32, QSet<qint32>> m_lines;
    QVariantList m_symbolList, m_fileList, m_findingList;
    QString m_summary, m_error;
    int m_revision = 0;
    quint64 m_generation = 0;
    QFutureWatcher<Result> m_watcher;
    std::shared_ptr<const Unravel::CodeModel> m_model;
};
