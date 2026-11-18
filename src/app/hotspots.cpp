#include "hotspots.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFutureWatcher>
#include <QSettings>
#include <QTimeZone>
#include <QStandardPaths>
#include <QtConcurrent>

#include <algorithm>

#include "session.h"

using namespace Unravel;

namespace {
Hotspots *g_hotspots = nullptr;
}

Hotspots::Hotspots(QObject *parent)
    : QObject(parent)
{
    if (!g_hotspots)
        g_hotspots = this;
    connect(Session::instance(), &Session::modelChanged, this, &Hotspots::modelChanged);
    auto clearBlame = [this] {
        cancelBlame();
        m_blameText.clear();
        emit blameChanged();
    };
    connect(Session::instance(), &Session::activeChanged, this, clearBlame);
    connect(Session::instance(), &Session::projectChanged, this, clearBlame);
    modelChanged();
}

Hotspots::~Hotspots()
{
    if (m_cancel)
        m_cancel->store(true);
    if (m_blameCancel)
        m_blameCancel->store(true);
    if (g_hotspots == this)
        g_hotspots = nullptr;
}

Hotspots *Hotspots::instance()
{
    if (!g_hotspots)
        g_hotspots = new Hotspots(qApp);
    return g_hotspots;
}

QString Hotspots::settingsKey() const
{
    const QByteArray hash = QCryptographicHash::hash(m_root.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("hotspots/%1/days").arg(QString::fromLatin1(hash));
}

void Hotspots::modelChanged()
{
    cancelBlame();
    m_blameText.clear();
    emit blameChanged();
    const QString root = Session::instance()->root();
    if (root != m_root) {
        m_root = root;
        m_revision = QStringLiteral("HEAD");
        m_days = std::clamp(QSettings().value(settingsKey(), 180).toInt(), 1, 3650);
        emit windowChanged();
    }
    refresh();
}

void Hotspots::setDays(int days)
{
    days = std::clamp(days, 1, 3650);
    if (days == m_days)
        return;
    m_days = days;
    if (!m_root.isEmpty())
        QSettings().setValue(settingsKey(), days);
    emit windowChanged();
    refresh();
}

void Hotspots::setRevision(const QString &revision)
{
    if (revision == m_revision)
        return;
    m_revision = revision;
    emit windowChanged();
    refresh();
}

void Hotspots::setFollowRenames(bool value)
{
    if (m_followRenames == value)
        return;
    m_followRenames = value;
    emit windowChanged();
    refresh();
}

void Hotspots::refresh(bool bypassCache)
{
    if (m_cancel)
        m_cancel->store(true);
    const quint64 generation = ++m_generation;
    m_activity = {};
    m_busy = !m_root.isEmpty();
    if (!m_busy) {
        emit changed();
        return;
    }
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    const QString root = m_root;
    const qint64 until = QDateTime::currentSecsSinceEpoch() / 86400 * 86400 + 86399;
    const qint64 since = until - qint64(m_days) * 86400 + 1;
    const QString revision = m_revision;
    const bool followRenames = m_followRenames;
    const QString cacheDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/history");
    auto *watcher = new QFutureWatcher<History::Activity>(this);
    connect(watcher, &QFutureWatcher<History::Activity>::finished, this, [this, watcher, generation] {
        const History::Activity activity = watcher->result();
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        m_activity = activity;
        m_busy = false;
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run([root, since, until, cancel, revision, cacheDirectory, bypassCache, followRenames] {
        return History::fileActivity(root, revision, since, until, cancel.get(), cacheDirectory, bypassCache, followRenames);
    }));
    emit changed();
}

void Hotspots::cancel()
{
    if (!m_busy)
        return;
    if (m_cancel)
        m_cancel->store(true);
    ++m_generation;
    m_activity = {};
    m_activity.cancelled = true;
    m_busy = false;
    emit changed();
}

void Hotspots::cancelBlame()
{
    if (m_blameCancel)
        m_blameCancel->store(true);
    ++m_blameGeneration;
    if (m_blameBusy) {
        m_blameBusy = false;
        m_blameText = tr("Blame cancelled");
        emit blameChanged();
    }
}

void Hotspots::loadBlame(int file, int firstLine)
{
    cancelBlame();
    const auto model = Session::instance()->model();
    if (!model || file < 0 || file >= model->files.size() || !model->files[file].project) {
        m_blameText = tr("Select an indexed project source location");
        emit blameChanged();
        return;
    }
    m_blameCancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_blameCancel;
    const quint64 generation = ++m_blameGeneration;
    const QString root = model->root, path = model->files[file].path;
    m_blameBusy = true;
    m_blameText = tr("Reading worktree line attribution against HEAD (up to 80 lines)...");
    auto *watcher = new QFutureWatcher<History::Blame>(this);
    connect(watcher, &QFutureWatcher<History::Blame>::finished, this, [this, watcher, generation] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != m_blameGeneration)
            return;
        m_blameBusy = false;
        m_blameText = result.cancelled ? tr("Blame cancelled")
            : !result.error.isEmpty() ? result.error
            : tr("Worktree attribution against HEAD; independent of the history window/revision.\nLine | commit | contributor | summary\n\n") + result.text;
        emit blameChanged();
    });
    watcher->setFuture(QtConcurrent::run([root, path, firstLine, cancel] {
        return History::blame(root, path, firstLine, cancel.get());
    }));
    emit blameChanged();
}

QString Hotspots::summary() const
{
    if (m_root.isEmpty())
        return tr("Open a project to read its Git history");
    if (m_busy)
        return tr("Reading Git history...");
    if (!m_activity.error.isEmpty())
        return m_activity.error;
    if (m_activity.cancelled)
        return tr("Git history cancelled. Refresh to retry.");
    if (!m_activity.ready())
        return tr("No Git repository");
    const QString from = QDateTime::fromSecsSinceEpoch(m_activity.since, QTimeZone::UTC).date().toString(Qt::ISODate);
    const QString to = QDateTime::fromSecsSinceEpoch(m_activity.until, QTimeZone::UTC).date().toString(Qt::ISODate);
    return tr("Revision %1 | %2 to %3 (UTC) | %4%5%6%7")
        .arg(m_activity.revision.left(8), from, to,
             m_activity.followedRenames ? tr("Git --follow lineage (heuristic)") : tr("pathname history"),
             m_activity.shallow ? tr(" | partial: shallow clone") : QString(),
             m_activity.cached ? tr(" | cached") : QString(),
             m_activity.cacheWarning.isEmpty() ? QString() : QStringLiteral(" | ") + m_activity.cacheWarning);
}

const History::FileActivity *Hotspots::forFile(const QString &path) const
{
    if (!ready())
        return nullptr;
    const QString relative = QDir(m_activity.repository).relativeFilePath(path);
    if (relative == QLatin1String("..") || relative.startsWith(QLatin1String("../")) || QDir::isAbsolutePath(relative))
        return nullptr;
    const auto it = m_activity.files.constFind(relative);
    static const History::FileActivity zero;
    return it == m_activity.files.cend() ? &zero : &*it;
}
