#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QVariantMap>
#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>

#include "analysis.h"
#include "model.h"
#include "indexer.h"
#include "project.h"

// The open project: finds out how to compile it, indexes it with clang on
// worker threads (or loads the index from the cache), keeps the analysis,
// and knows the active symbol every view shows, with a history to go back
// and forth. The one object the QML talks to about the code.
class Session : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int state READ state NOTIFY stateChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY projectChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
    Q_PROPERTY(QString root READ root NOTIFY modelChanged)
    Q_PROPERTY(QStringList notes READ notes NOTIFY projectChanged)
    Q_PROPERTY(QString error READ error NOTIFY projectChanged)
    Q_PROPERTY(bool hasModel READ hasModel NOTIFY modelChanged)
    Q_PROPERTY(bool stale READ stale NOTIFY staleChanged)
    Q_PROPERTY(int revision READ revision NOTIFY modelChanged)
    Q_PROPERTY(QVariantMap stats READ stats NOTIFY modelChanged)
    Q_PROPERTY(QStringList recentProjects READ recentProjects NOTIFY recentChanged)
    Q_PROPERTY(QString extraArgs READ extraArgs WRITE setExtraArgs NOTIFY projectChanged)
    Q_PROPERTY(QString editorCommand READ editorCommand WRITE setEditorCommand NOTIFY editorCommandChanged)

    Q_PROPERTY(int activeSymbol READ activeSymbol NOTIFY activeChanged)
    Q_PROPERTY(int activeFile READ activeFile NOTIFY activeChanged)
    Q_PROPERTY(QVariantMap activeInfo READ activeInfo NOTIFY activeChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY activeChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY activeChanged)
    Q_PROPERTY(QVariantList bookmarks READ bookmarks NOTIFY bookmarksChanged)
    Q_PROPERTY(bool activeBookmarked READ activeBookmarked NOTIFY activeBookmarkedChanged)

public:
    enum State { Idle, Discovering, Indexing, Ready };
    Q_ENUM(State)

    // Not default-constructible, so QML takes the one create() hands out
    explicit Session(QObject *parent);
    ~Session() override;

    static Session *instance();
    // QML gets the one instance main() made
    static Session *create(QQmlEngine *, QJSEngine *)
    {
        Session *s = instance();
        QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
        return s;
    }

    int state() const { return m_state; }
    qreal progress() const { return m_progress; }
    QString progressText() const { return m_progressText; }
    QString projectPath() const { return m_projectPath; }
    QString projectName() const;
    QString root() const;
    QStringList notes() const { return m_notes; }
    QString error() const { return m_error; }
    bool hasModel() const { return bool(m_model); }
    bool stale() const { return m_stale; }
    int revision() const { return m_revision; }
    QVariantMap stats() const;
    QStringList recentProjects() const;
    QString extraArgs() const { return m_extraArgs; }
    void setExtraArgs(const QString &args);
    QString editorCommand() const;
    void setEditorCommand(const QString &command);

    int activeSymbol() const { return m_active.symbol; }
    int activeFile() const { return m_active.file; }
    QVariantMap activeInfo() const;
    bool canGoBack() const { return !m_back.isEmpty(); }
    bool canGoForward() const { return !m_forward.isEmpty(); }
    QVariantList bookmarks() const;
    bool activeBookmarked() const;

    std::shared_ptr<const Unravel::CodeModel> model() const { return m_model; }
    const Unravel::Analysis::Report &report() const { return m_report; }

    // Project
    Q_INVOKABLE void open(const QString &pathOrUrl);
    Q_INVOKABLE void reindex();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void close();
    Q_INVOKABLE void forgetRecent(const QString &path);
    Q_INVOKABLE QString toLocalPath(const QString &pathOrUrl) const;

    // Navigation
    Q_INVOKABLE void activate(int symbol);
    Q_INVOKABLE void activateFile(int file, int line = 0);
    Q_INVOKABLE void activateReference(int ref);
    Q_INVOKABLE void back();
    Q_INVOKABLE void forward();
    Q_INVOKABLE QVariantList history() const;
    Q_INVOKABLE void toggleBookmark();
    Q_INVOKABLE void removeBookmark(int index);
    // A symbol worth starting with: main, or the most connected
    Q_INVOKABLE int startSymbol() const;

    // Facts for the views
    Q_INVOKABLE QVariantMap symbolInfo(int symbol) const;
    Q_INVOKABLE QVariantMap fileInfo(int file) const;
    Q_INVOKABLE QVariantMap referenceInfo(int ref) const;
    Q_INVOKABLE QVariantList references(int symbol, int limit = 500) const;
    Q_INVOKABLE QString kindName(int kind) const;
    Q_INVOKABLE int symbolByName(const QString &qualified) const;
    Q_INVOKABLE QVariantList moduleMetrics() const;
    Q_INVOKABLE QVariantMap classMetrics(int symbol) const;
    Q_INVOKABLE QString reportMarkdown() const;
    Q_INVOKABLE bool saveReport(const QString &pathOrUrl) const;
    Q_INVOKABLE void copyToClipboard(const QString &text) const;
    Q_INVOKABLE bool writeText(const QString &pathOrUrl, const QString &text) const;
    Q_INVOKABLE void openInEditor(int file, int line) const;
    Q_INVOKABLE void openFolder(int file) const;
    Q_INVOKABLE QString cachePath() const;
    Q_INVOKABLE QVariantList diagnostics(int limit = 5000) const;

    // The line at which `focus` sits in the code the view shows (for scrolling)
    int focusLine() const { return m_active.line; }

signals:
    void stateChanged();
    void progressChanged();
    void projectChanged();
    void modelChanged();
    void staleChanged();
    void recentChanged();
    void activeChanged();
    void bookmarksChanged();
    void activeBookmarkedChanged();
    void editorCommandChanged();
    void message(const QString &text);

private:
    struct Place {
        int symbol = -1;
        int file = -1;
        int line = 0;
        bool operator==(const Place &o) const { return symbol == o.symbol && file == o.file && line == o.line; }
    };
    struct Result {
        quint64 generation = 0;
        bool cached = false;      // loaded from the cache
        bool loadFailed = false;  // there was a cache, it couldn't be read
        bool stale = false;
        std::shared_ptr<Unravel::CodeModel> model;
        Unravel::Analysis::Report report;
        Unravel::IndexResult result;
        Unravel::ProjectSetup setup;
    };

    void setState(State s);
    void setProgress(qreal p, const QString &text);
    void startIndexing(bool useCache);
    void runCMake(const QString &root);
    void indexingFinished();
    void adopt(std::shared_ptr<Unravel::CodeModel> model, Unravel::Analysis::Report report);
    static bool isStale(const Unravel::CodeModel &m);
    void stopCMake();
    void go(const Place &place, bool record);
    void addRecent(const QString &path);
    void loadBookmarks();
    void saveBookmarks() const;
    QString indexFile() const;
    QString settingsKey() const;

    State m_state = Idle;
    qreal m_progress = 0;
    QString m_progressText;
    QString m_projectPath;
    QString m_buildDir;     // where the compile database is, when made by us
    QStringList m_notes;
    QString m_error;
    QString m_extraArgs;
    bool m_stale = false;
    int m_revision = 0;
    std::shared_ptr<const Unravel::CodeModel> m_model;
    Unravel::Analysis::Report m_report;
    std::atomic<bool> m_cancel{false};
    QFutureWatcher<Result> m_watcher;
    QProcess *m_cmake = nullptr;
    quint64 m_generation = 0;   // what an answer from a worker or CMake must belong to
    Place m_active;
    QList<Place> m_back, m_forward;
    QStringList m_bookmarkUsrs;
};
