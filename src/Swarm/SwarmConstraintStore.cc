#include "SwarmConstraintStore.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>

SwarmConstraintStore::SwarmConstraintStore(const QString& path, QObject* parent) : QObject(parent), _path(path)
{
    // Only first construction may create an unrestricted default. Runtime deletion is an error.
    if (!QFile::exists(_path)) {
        QDir().mkpath(QFileInfo(_path).absolutePath());
        QSaveFile file(_path);
        const auto bytes = QJsonDocument(SwarmConstraintChecker::configToJson({})).toJson();
        if (file.open(QIODevice::WriteOnly)) {
            if (file.write(bytes) == bytes.size())
                file.commit();
            else
                file.cancelWriting();
        }
    }
    connect(&_watcher, &QFileSystemWatcher::fileChanged, this, &SwarmConstraintStore::refresh);
    connect(&_watcher, &QFileSystemWatcher::directoryChanged, this, &SwarmConstraintStore::refresh);
    refresh();
}

void SwarmConstraintStore::_watchPaths()
{
    const auto directory = QFileInfo(_path).absolutePath();
    if (!_watcher.directories().contains(directory) && QDir(directory).exists())
        _watcher.addPath(directory);
    if (!_watcher.files().contains(_path) && QFile::exists(_path))
        _watcher.addPath(_path);
}

void SwarmConstraintStore::refresh()
{
    _watchPaths();
    QFile file(_path);
    const bool readable = file.open(QIODevice::ReadOnly);
    const QByteArray bytes = readable ? file.readAll() : QByteArray();
    const QByteArray identity = (readable ? QByteArray("read:") : QByteArray("error:")) +
                                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    if (_revision && identity == _identity)
        return;
    _identity = identity;
    ++_revision;
    SwarmConstraintChecker::Config next;
    if (!readable) {
        next.error = tr("无法读取围栏配置");
    } else {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            next.error = tr("围栏配置 JSON 损坏或格式错误");
        else
            next = SwarmConstraintChecker::configFromJson(document.object());
    }
    _error = next.error;
    if (_error.isEmpty())
        _lastValid = next;
    emit changed();
}

SwarmConstraintChecker::Config SwarmConstraintStore::snapshot()
{
    refresh();
    auto config = _lastValid;
    config.error = _error;
    return config;
}
