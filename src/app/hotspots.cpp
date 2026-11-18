#include "hotspots.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFutureWatcher>
#include <QSettings>
#include <QTimeZone>
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
    modelChanged();
}

Hotspots::~Hotspots()
{
    if (m_cancel)
        m_cancel->store(true);
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
    const QString root = Session::instance()->root();
    if (root != m_root) {
        m_root = root;
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

void Hotspots::refresh()
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
    const qint64 until = QDateTime::currentSecsSinceEpoch();
    const qint64 since = until - qint64(m_days) * 86400;
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
    watcher->setFuture(QtConcurrent::run([root, since, until, cancel] {
        return History::fileActivity(root, QStringLiteral("HEAD"), since, until, cancel.get());
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
    return tr("HEAD %1 | %2 to %3 (UTC) | pathname history%4")
        .arg(m_activity.revision.left(8), from, to, m_activity.shallow ? tr(" | partial: shallow clone") : QString());
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
