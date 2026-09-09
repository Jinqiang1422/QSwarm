#pragma once

#include <QtCore/QFileSystemWatcher>
#include <QtCore/QObject>

#include "SwarmConstraintChecker.h"

/// One versioned configuration for dispatch, alerts and map overlays.
class SwarmConstraintStore : public QObject
{
    Q_OBJECT
public:
    explicit SwarmConstraintStore(const QString& path, QObject* parent = nullptr);
    SwarmConstraintChecker::Config snapshot();

    SwarmConstraintChecker::Config lastValid() const { return _lastValid; }

    quint64 revision() const { return _revision; }

    QString error() const { return _error; }

    void refresh();

signals:
    void changed();

private:
    void _watchPaths();
    QString _path;
    QFileSystemWatcher _watcher;
    QByteArray _identity;
    SwarmConstraintChecker::Config _lastValid;
    QString _error;
    quint64 _revision = 0;
};
