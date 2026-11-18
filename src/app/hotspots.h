#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>

#include "history.h"

class Hotspots : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int days READ days WRITE setDays NOTIFY windowChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool partial READ partial NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString revision READ revision WRITE setRevision NOTIFY windowChanged)
    Q_PROPERTY(bool followRenames READ followRenames WRITE setFollowRenames NOTIFY windowChanged)
    Q_PROPERTY(bool blameBusy READ blameBusy NOTIFY blameChanged)
    Q_PROPERTY(QString blameText READ blameText NOTIFY blameChanged)

public:
    explicit Hotspots(QObject *parent);
    ~Hotspots() override;

    static Hotspots *instance();
    static Hotspots *create(QQmlEngine *, QJSEngine *)
    {
        Hotspots *h = instance();
        QJSEngine::setObjectOwnership(h, QJSEngine::CppOwnership);
        return h;
    }

    int days() const { return m_days; }
    void setDays(int days);
    QString revision() const { return m_revision; }
    void setRevision(const QString &revision);
    bool followRenames() const { return m_followRenames; }
    void setFollowRenames(bool value);
    bool busy() const { return m_busy; }
    bool ready() const { return !m_busy && m_activity.ready(); }
    bool partial() const { return m_activity.shallow; }
    QString summary() const;
    QString error() const { return m_activity.error; }
    const Unravel::History::Activity &activity() const { return m_activity; }
    const Unravel::History::FileActivity *forFile(const QString &path) const;

    Q_INVOKABLE void refresh(bool bypassCache = false);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void loadBlame(int file, int firstLine);
    Q_INVOKABLE void cancelBlame();
    bool blameBusy() const { return m_blameBusy; }
    QString blameText() const { return m_blameText; }

signals:
    void changed();
    void windowChanged();
    void blameChanged();

private:
    void modelChanged();
    QString settingsKey() const;

    QString m_root;
    int m_days = 180;
    QString m_revision = QStringLiteral("HEAD");
    bool m_followRenames = false;
    bool m_busy = false;
    quint64 m_generation = 0;
    Unravel::History::Activity m_activity;
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::shared_ptr<std::atomic_bool> m_blameCancel;
    quint64 m_blameGeneration = 0;
    bool m_blameBusy = false;
    QString m_blameText;
};
