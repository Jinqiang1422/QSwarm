#include "SwarmAcceptanceServer.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QMetaEnum>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QVariantMap>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <cmath>

#include "Fact.h"
#include "FirmwarePlugin.h"
#include "FormationController.h"
#include "MissionItem.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "QmlObjectListModel.h"
#include "SwarmCOPModel.h"
#include "SwarmManager.h"
#include "SwarmMissionPlanner.h"
#include "SwarmPerceptionBridge.h"
#include "SwarmTaskController.h"
#include "SwarmTaskLibrary.h"
#include "SwarmTypes.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"

namespace {
constexpr int kDefaultAcceptancePort = 15610;
constexpr qsizetype kMinimumTokenBytes = 16;
constexpr qsizetype kMaximumTokenBytes = 512;
constexpr qsizetype kMaximumRequestBytes = 64 * 1024;

QJsonObject errorResponse(const QString& error)
{
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
}

QGeoCoordinate requestCoordinate(const QJsonObject& request)
{
    return {request.value(QStringLiteral("center_lat")).toDouble(),
            request.value(QStringLiteral("center_lon")).toDouble()};
}

QString readAcceptanceToken()
{
    const QString path = qEnvironmentVariable("QGC_SWARM_ACCEPTANCE_TOKEN_FILE");
    if (path.isEmpty()) {
        return {};
    }

    const QFileInfo fileInfo(path);
    if (!fileInfo.isFile() || fileInfo.isSymLink()) {
        return {};
    }
    constexpr QFileDevice::Permissions unsafePermissions = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                                           QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                                           QFileDevice::WriteOther | QFileDevice::ExeOther;
    const QFileDevice::Permissions permissions = fileInfo.permissions();
    if (!(permissions & QFileDevice::ReadOwner) || (permissions & unsafePermissions)) {
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QByteArray encoded = file.read(kMaximumTokenBytes + 3);
    if (encoded.endsWith("\r\n")) {
        encoded.chop(2);
    } else if (encoded.endsWith('\n')) {
        encoded.chop(1);
    }
    if (encoded.size() < kMinimumTokenBytes || encoded.size() > kMaximumTokenBytes || encoded.contains('\0') ||
        encoded.contains('\r') || encoded.contains('\n')) {
        return {};
    }

    const QString token = QString::fromUtf8(encoded);
    return token.toUtf8() == encoded ? token : QString();
}
}  // namespace

SwarmAcceptanceServer::SwarmAcceptanceServer(SwarmManager* manager)
    : QObject(manager), _manager(manager), _server(new QTcpServer(this))
{
    (void) connect(_server, &QTcpServer::newConnection, this, &SwarmAcceptanceServer::_onNewConnection);
    connect(manager, &SwarmManager::actionDispatched, this, [this](quint64, int vehicleId, const QString& type) {
        const auto key = QString::number(vehicleId);
        auto counts = _dispatchCounts.value(key).toObject();
        counts[type] = counts.value(type).toInt() + 1;
        _dispatchCounts[key] = counts;
    });
    connect(manager, &SwarmManager::executionFinished, this, [this](quint64 id, bool success, const QString& reason) {
        _executionResult = {{QStringLiteral("execution_id"), QString::number(id)},
                            {QStringLiteral("success"), success},
                            {QStringLiteral("reason"), reason}};
    });
    connect(_manager, &SwarmManager::swarmCommandFeedback, this, [this](const QString& m) { _lastFeedback = m; });
}

bool SwarmAcceptanceServer::startFromEnvironment()
{
    _token = readAcceptanceToken();
    if (_token.isEmpty()) {
        return false;
    }

    bool portOk = false;
    int port = qEnvironmentVariableIntValue("QGC_SWARM_ACCEPTANCE_PORT", &portOk);
    if (!portOk) {
        port = kDefaultAcceptancePort;
    }
    if (port < 1024 || port > 65535) {
        return false;
    }
    return _server->listen(QHostAddress::LocalHost, quint16(port));
}

void SwarmAcceptanceServer::stop()
{
    _server->close();
    const QList<QTcpSocket*> sockets = _buffers.keys();
    _buffers.clear();
    for (QTcpSocket* socket : sockets) {
        if (!socket) {
            continue;
        }
        (void) QObject::disconnect(socket, nullptr, this, nullptr);
        socket->abort();
        socket->deleteLater();
    }
    _token.clear();
}

void SwarmAcceptanceServer::_onNewConnection()
{
    while (_server->hasPendingConnections()) {
        QTcpSocket* socket = _server->nextPendingConnection();
        if (!socket) {
            continue;
        }
        _buffers.insert(socket, {});
        (void) connect(socket, &QTcpSocket::readyRead, this, &SwarmAcceptanceServer::_onReadyRead);
        (void) connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            _buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void SwarmAcceptanceServer::_onReadyRead()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket || !_buffers.contains(socket)) {
        return;
    }

    QByteArray& buffer = _buffers[socket];
    buffer.append(socket->readAll());
    if (buffer.size() > kMaximumRequestBytes) {
        _writeResponse(socket, errorResponse(QStringLiteral("request_too_large")));
        socket->disconnectFromHost();
        return;
    }

    qsizetype newline = -1;
    while ((newline = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (line.isEmpty()) {
            continue;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            _writeResponse(socket, errorResponse(QStringLiteral("invalid_json")));
            continue;
        }
        _writeResponse(socket, _handleRequest(document.object()));
    }
}

QJsonObject SwarmAcceptanceServer::_handleRequest(const QJsonObject& request)
{
    if (request.value(QStringLiteral("token")).toString() != _token) {
        return errorResponse(QStringLiteral("unauthorized"));
    }

    const QString command = request.value(QStringLiteral("command")).toString();
    if (command == QStringLiteral("status")) {
        return _status();
    }
    if (command == QStringLiteral("quit")) {
        QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
        return {{QStringLiteral("ok"), true}};
    }
    if (command == QStringLiteral("configure_px4_sitl")) {
        return _configurePX4SITL();
    }
    if (command == QStringLiteral("configure_execution_test")) {
        if (request.contains(QStringLiteral("autonomy_level"))) {
            const int level = request.value(QStringLiteral("autonomy_level")).toInt();
            if (level < 1 || level > 3)
                return errorResponse(QStringLiteral("invalid_autonomy_level"));
            _manager->setAutonomyLevel(level);
        }
        if (request.contains(QStringLiteral("allowed_types"))) {
            QStringList types;
            for (const auto& value : request.value(QStringLiteral("allowed_types")).toArray())
                types.append(value.toString());
            _manager->setL3AllowedTypes(types);
        }
        if (request.contains(QStringLiteral("command_interval_ms")))
            _manager->setCommandIntervalMs(request.value(QStringLiteral("command_interval_ms")).toInt());
        if (request.value(QStringLiteral("authorize_targets")).toBool())
            _manager->authorizeTaskTargets(_manager->boundTargetIds());
        if (request.contains(QStringLiteral("constraints"))) {
            const auto config = request.value(QStringLiteral("constraints")).toObject();
            if (!SwarmConstraintChecker::configFromJson(config).error.isEmpty())
                return errorResponse(QStringLiteral("invalid_constraints"));
            QSaveFile file(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                           QStringLiteral("/SwarmConfig/fences.json"));
            const auto bytes = QJsonDocument(config).toJson();
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                return errorResponse(QStringLiteral("cannot_write_constraints"));
        }
        return {{QStringLiteral("ok"), true}, {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("plan_coverage")) {
        const QGeoCoordinate center = requestCoordinate(request);
        const bool accepted = _manager->planAndUploadCoverage(
            center, request.value(QStringLiteral("length_m")).toDouble(120.0),
            request.value(QStringLiteral("width_m")).toDouble(80.0),
            request.value(QStringLiteral("angle_deg")).toDouble(0.0),
            request.value(QStringLiteral("assign_mode")).toString(QStringLiteral("sequential")));
        return {{QStringLiteral("ok"), accepted}, {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("clear_missions")) {
        const bool accepted = _manager->clearAllMissions();
        return {{QStringLiteral("ok"), accepted}, {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("arm_all")) {
        _manager->armAll(true);
        return {{QStringLiteral("ok"), true}};
    }
    if (command == QStringLiteral("takeoff_all")) {
        _manager->takeoffAll(request.value(QStringLiteral("altitude_m")).toDouble(30.0));
        return {{QStringLiteral("ok"), true}};
    }
    if (command == QStringLiteral("start_formation")) {
        FormationController* formation = _manager->formation();
        const QString type = request.value(QStringLiteral("type")).toString(QStringLiteral("wedge"));
        if (type == QStringLiteral("column")) {
            formation->setFormationType(FormationController::FormationColumn);
        } else if (type == QStringLiteral("line")) {
            formation->setFormationType(FormationController::FormationLine);
        } else if (type == QStringLiteral("circle")) {
            formation->setFormationType(FormationController::FormationCircle);
        } else if (type == QStringLiteral("wedge")) {
            formation->setFormationType(FormationController::FormationWedge);
        } else {
            return errorResponse(QStringLiteral("invalid_formation_type"));
        }
        formation->setSpacingMeters(request.value(QStringLiteral("spacing_m")).toDouble(10.0));
        formation->setVerticalStepMeters(request.value(QStringLiteral("vertical_step_m")).toDouble(3.0));
        const bool accepted = _manager->startFormation(request.value(QStringLiteral("leader_id")).toInt());
        return {{QStringLiteral("ok"), accepted}, {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("stop_formation")) {
        _manager->stopFormation();
        return {{QStringLiteral("ok"), true}, {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("goto")) {
        // Same entry point as the Fly View map "Go to location" action.
        Vehicle* vehicle =
            MultiVehicleManager::instance()->getVehicleById(request.value(QStringLiteral("vehicle_id")).toInt());
        if (!vehicle) {
            return errorResponse(QStringLiteral("vehicle_not_found"));
        }
        // Not every gotoLocation() path emits swarmCommandFeedback (e.g. the native fallback); clear the last
        // message first so the response does not report a stale feedback from an earlier command.
        _lastFeedback.clear();
        const bool accepted = _manager->gotoLocation(vehicle, requestCoordinate(request));
        return {{QStringLiteral("ok"), accepted},
                {QStringLiteral("message"), _lastFeedback},
                {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("formation_route")) {
        // Same entry point as the swarm panel "编队按航线飞" button: confirmation summary, then start.
        const QString summary = _manager->formationRouteSummary();
        _lastFeedback.clear();
        const bool accepted = _manager->startFormationRoute();
        return {{QStringLiteral("ok"), accepted},
                {QStringLiteral("summary"), summary},
                {QStringLiteral("message"), _lastFeedback},
                {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("upload_route")) {
        // SITL preparation for the formation route scenario: writes a waypoint-only route.
        Vehicle* vehicle =
            MultiVehicleManager::instance()->getVehicleById(request.value(QStringLiteral("vehicle_id")).toInt());
        if (!vehicle) {
            return errorResponse(QStringLiteral("vehicle_not_found"));
        }
        MissionManager* mission = vehicle->missionManager();
        if (!mission || !vehicle->homePosition().isValid()) {
            return errorResponse(QStringLiteral("vehicle_not_ready"));
        }
        if (mission->inProgress()) {
            return errorResponse(QStringLiteral("mission_busy"));
        }
        const QJsonArray waypoints = request.value(QStringLiteral("waypoints")).toArray();
        if (waypoints.isEmpty()) {
            return errorResponse(QStringLiteral("empty_route"));
        }
        // Validate every entry before allocating any MissionItem, so a bad request never leaks mission items.
        for (qsizetype i = 0; i < waypoints.size(); ++i) {
            const QJsonValue entry = waypoints.at(i);
            const QJsonObject point = entry.toObject();
            const double lat = point.value(QStringLiteral("lat")).toDouble(qQNaN());
            const double lon = point.value(QStringLiteral("lon")).toDouble(qQNaN());
            const double altRel = point.value(QStringLiteral("alt_rel_m")).toDouble(qQNaN());
            if (!entry.isObject() || !SwarmTypes::isValidLatLon(lat, lon) || !std::isfinite(altRel) || altRel <= 0) {
                return errorResponse(QStringLiteral("invalid_waypoint_%1").arg(i));
            }
        }
        const QGeoCoordinate home = vehicle->homePosition();
        // Index 0 is the Home placeholder; PlanManager strips it for firmwares that keep Home on board.
        QList<MissionItem*> items{new MissionItem(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL, 0, 0, 0, 0,
                                                  home.latitude(), home.longitude(), home.altitude(), true, false)};
        for (const QJsonValue& value : waypoints) {
            const QJsonObject point = value.toObject();
            items.append(new MissionItem(items.size(), MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 0, 0, 0,
                                         NAN, point.value(QStringLiteral("lat")).toDouble(),
                                         point.value(QStringLiteral("lon")).toDouble(),
                                         point.value(QStringLiteral("alt_rel_m")).toDouble(), true, false));
        }
        // Tracked per vehicle rather than reported synchronously: writeMissionItems() only confirms the upload
        // later, via sendComplete(). Scripts poll status().route_uploads for the outcome.
        const int vehicleId = vehicle->id();
        _routeUploads[vehicleId] = QStringLiteral("pending");
        connect(
            mission, &MissionManager::sendComplete, this,
            [this, vehicleId](bool error) {
                _routeUploads[vehicleId] = error ? QStringLiteral("failed") : QStringLiteral("ok");
            },
            Qt::SingleShotConnection);
        mission->writeMissionItems(items);
        return {{QStringLiteral("ok"), true}};
    }
    if (command == QStringLiteral("start_mission")) {
        _manager->startMissionAll();
        return {{QStringLiteral("ok"), true}};
    }
    if (command == QStringLiteral("pause_all")) {
        _manager->pauseAll();
        return {{QStringLiteral("ok"), true}};
    }
    if (command == QStringLiteral("rtl_all")) {
        _manager->rtlAll();
        return {{QStringLiteral("ok"), true}};
    }
    if (command == QStringLiteral("set_flight_mode")) {
        return _setFlightMode(request);
    }
    if (command == QStringLiteral("execute_actions")) {
        return _executeActions(request);
    }
    if (command == QStringLiteral("start_task")) {
        QStringList warnings;
        SwarmTaskController* taskController = _manager->taskController();
        const QList<SwarmTaskController::Stage> stages = taskController->parseStages(
            request.value(QStringLiteral("stages")).toArray(), requestCoordinate(request), warnings);
        const bool accepted = taskController->startTask(request.value(QStringLiteral("name")).toString(), stages, true);
        return {{QStringLiteral("ok"), accepted},
                {QStringLiteral("warnings"), QJsonArray::fromStringList(warnings)},
                {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("proceed_task")) {
        _manager->taskController()->proceed();
        return {{QStringLiteral("ok"), true}, {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("abort_task")) {
        _manager->taskController()->abort(QStringLiteral("acceptance_test"));
        return {{QStringLiteral("ok"), true}, {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("retry_task")) {
        _manager->taskController()->retryBlockedStage();
        return {{QStringLiteral("ok"), true}, {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("templates")) {
        _manager->taskLibrary()->reload();
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("templates"), QJsonArray::fromVariantList(_manager->taskLibrary()->templates())}};
    }
    if (command == QStringLiteral("run_template")) {
        const auto center = requestCoordinate(request);
        const bool accepted =
            _manager->taskLibrary()->runTemplate(request.value(QStringLiteral("name")).toString(),
                                                 request.value(QStringLiteral("parameters")).toObject().toVariantMap(),
                                                 center.latitude(), center.longitude());
        return {{QStringLiteral("ok"), accepted}, {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("set_selection")) {
        auto* vehicles = MultiVehicleManager::instance();
        const auto ids = request.value(QStringLiteral("vehicle_ids")).toArray();
        for (const auto& id : ids) {
            if (!vehicles->getVehicleById(id.toInt())) {
                return errorResponse(QStringLiteral("vehicle_not_found"));
            }
        }
        vehicles->deselectAllVehicles();
        for (const auto& id : ids) {
            vehicles->selectVehicle(id.toInt());
        }
        return {{QStringLiteral("ok"), true}, {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("land_all")) {
        _manager->landAll();
        return {{QStringLiteral("ok"), true}};
    }
    if (command == QStringLiteral("emergency_disarm_all")) {
        _manager->emergencyDisarmAll();
        return {{QStringLiteral("ok"), true}};
    }
    if (command == QStringLiteral("set_perception")) {
        if (request.value(QStringLiteral("enabled")).toBool()) {
            return {{QStringLiteral("ok"), _manager->startPerception()}};
        }
        _manager->stopPerception();
        return {{QStringLiteral("ok"), true}};
    }
    return errorResponse(QStringLiteral("unknown_command"));
}

QJsonObject SwarmAcceptanceServer::_configurePX4SITL() const
{
    constexpr double preflightDisarmSeconds = 60.0;
    const QString parameterName = QStringLiteral("COM_DISARM_PRFLT");
    QJsonArray configured;
    QJsonArray missing;
    QmlObjectListModel* vehicles = MultiVehicleManager::instance()->vehicles();
    for (int i = 0; i < vehicles->count(); ++i) {
        Vehicle* vehicle = qobject_cast<Vehicle*>(vehicles->get(i));
        ParameterManager* parameters = vehicle ? vehicle->parameterManager() : nullptr;
        if (!vehicle || !parameters ||
            !parameters->parameterExists(ParameterManager::defaultComponentId, parameterName)) {
            if (vehicle) {
                missing.append(vehicle->id());
            }
            continue;
        }
        Fact* parameter = parameters->getParameter(ParameterManager::defaultComponentId, parameterName);
        parameter->setRawValue(preflightDisarmSeconds);
        configured.append(vehicle->id());
    }

    return {
        {QStringLiteral("ok"), vehicles->count() > 0 && configured.count() == vehicles->count()},
        {QStringLiteral("parameter"), parameterName},
        {QStringLiteral("value"), preflightDisarmSeconds},
        {QStringLiteral("configured_vehicle_ids"), configured},
        {QStringLiteral("missing_vehicle_ids"), missing},
    };
}

QJsonObject SwarmAcceptanceServer::_setFlightMode(const QJsonObject& request) const
{
    Vehicle* vehicle =
        MultiVehicleManager::instance()->getVehicleById(request.value(QStringLiteral("vehicle_id")).toInt());
    if (!vehicle) {
        return errorResponse(QStringLiteral("vehicle_not_found"));
    }

    QString mode = request.value(QStringLiteral("mode")).toString();
    const QString kind = request.value(QStringLiteral("mode_kind")).toString();
    FirmwarePlugin* firmwarePlugin = vehicle->firmwarePlugin();
    if (mode.isEmpty() && kind == QStringLiteral("manual") && firmwarePlugin) {
        mode = firmwarePlugin->takeControlFlightMode();
    } else if (mode.isEmpty() && kind == QStringLiteral("pause")) {
        mode = vehicle->pauseFlightMode();
    } else if (mode.isEmpty() && kind == QStringLiteral("mission")) {
        mode = vehicle->missionFlightMode();
    } else if (mode.isEmpty() && kind == QStringLiteral("rtl")) {
        mode = vehicle->rtlFlightMode();
    }
    if (mode.isEmpty() || !vehicle->flightModes().contains(mode)) {
        return errorResponse(QStringLiteral("flight_mode_not_supported"));
    }

    vehicle->setFlightMode(mode);
    return {{QStringLiteral("ok"), true}, {QStringLiteral("requested_mode"), mode}};
}

QJsonObject SwarmAcceptanceServer::_executeActions(const QJsonObject& request) const
{
    const QString origin = request.value(QStringLiteral("origin")).toString(QStringLiteral("operator"));
    const QSet<QString> allowedOrigins = {
        QStringLiteral("operator"), QStringLiteral("ai"),       QStringLiteral("ai_confirmed"),
        QStringLiteral("task"),     QStringLiteral("template"),
    };
    if (!allowedOrigins.contains(origin)) {
        return errorResponse(QStringLiteral("invalid_origin"));
    }

    QStringList warnings;
    const QList<SwarmAction> actions = _manager->parseAndValidate(request.value(QStringLiteral("actions")).toArray(),
                                                                  requestCoordinate(request), warnings);
    if (actions.isEmpty()) {
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("error"), QStringLiteral("no_valid_actions")},
                {QStringLiteral("warnings"), QJsonArray::fromStringList(warnings)}};
    }
    const bool confirmed = origin == QStringLiteral("operator") || origin == QStringLiteral("ai_confirmed") ||
                           origin == QStringLiteral("template");
    const bool accepted = _manager->executeActions(
        actions, origin,
        confirmed ? SwarmManager::ExecutionApproval::OperatorConfirmed : SwarmManager::ExecutionApproval::Automatic);
    return {{QStringLiteral("ok"), accepted},
            {QStringLiteral("execution_id"), QString::number(_manager->lastExecutionId())},
            {QStringLiteral("action_count"), actions.count()},
            {QStringLiteral("warnings"), QJsonArray::fromStringList(warnings)}};
}

QJsonObject SwarmAcceptanceServer::_status() const
{
    QJsonArray vehiclesJson;
    MultiVehicleManager* multiVehicleManager = MultiVehicleManager::instance();
    QmlObjectListModel* vehicles = multiVehicleManager->vehicles();
    for (int i = 0; i < vehicles->count(); ++i) {
        Vehicle* vehicle = qobject_cast<Vehicle*>(vehicles->get(i));
        if (!vehicle) {
            continue;
        }
        VehicleFactGroup* facts = qobject_cast<VehicleFactGroup*>(vehicle->vehicleFactGroup());
        FirmwarePlugin* firmwarePlugin = vehicle->firmwarePlugin();
        const QGeoCoordinate coordinate = vehicle->coordinate();
        vehiclesJson.append(QJsonObject{
            {QStringLiteral("id"), vehicle->id()},
            {QStringLiteral("armed"), vehicle->armed()},
            {QStringLiteral("initial_connect_complete"), vehicle->isInitialConnectComplete()},
            {QStringLiteral("ready_to_fly_available"), vehicle->readyToFlyAvailable()},
            {QStringLiteral("ready_to_fly"), vehicle->readyToFly()},
            {QStringLiteral("all_sensors_healthy"), vehicle->allSensorsHealthy()},
            {QStringLiteral("flight_mode"), vehicle->flightMode()},
            {QStringLiteral("take_control_mode"), firmwarePlugin ? firmwarePlugin->takeControlFlightMode() : QString()},
            {QStringLiteral("lat"), coordinate.latitude()},
            {QStringLiteral("lon"), coordinate.longitude()},
            {QStringLiteral("alt_rel_m"), facts ? facts->altitudeRelative()->rawValue().toDouble() : 0.0},
            {QStringLiteral("alt_amsl_m"), facts ? facts->altitudeAMSL()->rawValue().toDouble() : 0.0},
            {QStringLiteral("heading_deg"), facts ? facts->heading()->rawValue().toDouble() : 0.0},
            {QStringLiteral("ground_speed_mps"), facts ? facts->groundSpeed()->rawValue().toDouble() : 0.0},
            {QStringLiteral("lease_state"), _manager->controlLeaseState(vehicle->id())},
            {QStringLiteral("automation_allowed"), _manager->automationAllowed(vehicle->id())},
        });
    }

    QJsonArray tracks;
    SwarmCOPModel* copModel = _manager->copModel();
    for (int row = 0; row < copModel->trackCount(); ++row) {
        QVariantMap track = copModel->trackAt(row);
        track[QStringLiteral("categoryId")] = copModel->data(copModel->index(row), SwarmCOPModel::CategoryRole).toInt();
        tracks.append(QJsonObject::fromVariantMap(track));
    }

    FormationController* formation = _manager->formation();
    QJsonObject formationSlots;
    const QHash<int, int> slotAssignment = formation->slotAssignment();
    for (auto it = slotAssignment.cbegin(); it != slotAssignment.cend(); ++it)
        formationSlots.insert(QString::number(it.key()), it.value());
    QJsonObject routeUploads;
    for (auto it = _routeUploads.cbegin(); it != _routeUploads.cend(); ++it)
        routeUploads.insert(QString::number(it.key()), it.value());
    SwarmTaskController* taskController = _manager->taskController();
    SwarmPerceptionBridge* perception = _manager->perception();
    const int windowCount = QGuiApplication::topLevelWindows().count();
    const QJsonObject rendered = _renderedItemCounts();
    return {
        {QStringLiteral("ok"), true},
        {QStringLiteral("ui"),
         QJsonObject{{QStringLiteral("window_count"), windowCount},
                     {QStringLiteral("qml_ready"), windowCount > 0},
                     {QStringLiteral("situation_layer_count"), rendered.value(QStringLiteral("situation_layers"))}}},
        {QStringLiteral("vehicle_count"), vehiclesJson.count()},
        {QStringLiteral("dispatch_counts"), _dispatchCounts},
        {QStringLiteral("route_uploads"), routeUploads},
        {QStringLiteral("execution"), _executionResult},
        {QStringLiteral("execution_states"), QJsonObject::fromVariantMap(_manager->executionStates())},
        {QStringLiteral("execution_message"), _manager->executionMessage()},
        {QStringLiteral("perception_diagnostics"), QJsonObject::fromVariantMap(_manager->perceptionDiagnostics())},
        {QStringLiteral("vehicles"), vehiclesJson},
        {QStringLiteral("formation"),
         QJsonObject{{QStringLiteral("active"), formation->active()},
                     {QStringLiteral("leader_id"), formation->leaderVehicleId()},
                     {QStringLiteral("follower_count"), formation->followerCount()},
                     {QStringLiteral("type"), formation->formationType()},
                     {QStringLiteral("spacing_m"), formation->spacingMeters()},
                     {QStringLiteral("vertical_step_m"), formation->verticalStepMeters()},
                     {QStringLiteral("heading_deg"), formation->formationHeading()},
                     {QStringLiteral("slots"), formationSlots},
                     {QStringLiteral("transit"), QJsonObject::fromVariantMap(formation->transitStatus())}}},
        {QStringLiteral("mission"),
         QJsonObject{
             {QStringLiteral("vehicle_count"), _manager->missionPlanner()->lastPlanVehicleCount()},
             {QStringLiteral("waypoint_count"), _manager->missionPlanner()->lastPlanWaypointCount()},
             {QStringLiteral("status"), QJsonObject::fromVariantMap(_manager->missionPlanner()->missionStatus())}}},
        {QStringLiteral("task"),
         QJsonObject{{QStringLiteral("running"), taskController->running()},
                     {QStringLiteral("can_retry"), taskController->canRetry()},
                     {QStringLiteral("blocked"), taskController->blocked()},
                     {QStringLiteral("task_id"), taskController->taskId()},
                     {QStringLiteral("outcome"),
                      QString::fromLatin1(QMetaEnum::fromType<SwarmTaskController::Outcome>().valueToKey(
                          static_cast<int>(taskController->outcome())))},
                     {QStringLiteral("result"), QJsonObject::fromVariantMap(taskController->result())},
                     {QStringLiteral("dispatches"), QJsonArray::fromVariantList(taskController->dispatchRecords())},
                     {QStringLiteral("persistence_error"), taskController->persistenceError()},
                     {QStringLiteral("name"), taskController->taskName()},
                     {QStringLiteral("stage_index"), taskController->currentStageIndex()},
                     {QStringLiteral("state"), taskController->stateText()},
                     {QStringLiteral("waiting_for_proceed"), taskController->waitingForProceed()}}},
        {QStringLiteral("perception"), QJsonObject{{QStringLiteral("listening"), perception->listening()},
                                                   {QStringLiteral("service_online"), perception->serviceOnline()},
                                                   {QStringLiteral("heartbeat_hz"), perception->heartbeatHz()},
                                                   {QStringLiteral("last_track_count"), perception->lastTrackCount()},
                                                   {QStringLiteral("wire_latency_ms"), perception->wireLatencyMs()}}},
        {QStringLiteral("cop"),
         QJsonObject{{QStringLiteral("track_count"), copModel->trackCount()},
                     {QStringLiteral("heatmap_cell_count"), copModel->heatmapCellCount()},
                     {QStringLiteral("tracks"), tracks},
                     {QStringLiteral("rendered_track_count"), rendered.value(QStringLiteral("tracks"))},
                     {QStringLiteral("rendered_heatmap_count"), rendered.value(QStringLiteral("heatmap"))},
                     {QStringLiteral("rendered_threat_count"), rendered.value(QStringLiteral("threats"))}}},
    };
}

QJsonObject SwarmAcceptanceServer::_renderedItemCounts() const
{
    int situationLayers = 0;
    int heatmap = 0;
    int threats = 0;
    QSet<const QQuickItem*> visitedItems;
    QSet<QString> visibleTrackIds;

    const auto countVisualItems = [&](const auto& self, QQuickItem* item) -> void {
        if (!item || visitedItems.contains(item) || !item->isVisible()) {
            return;
        }
        visitedItems.insert(item);
        const QString name = item->objectName();
        if (name == QStringLiteral("swarmSituationLayer")) {
            ++situationLayers;
        } else if (name.startsWith(QStringLiteral("swarmTrackMarker_"))) {
            visibleTrackIds.insert(name);
        } else if (name == QStringLiteral("swarmHeatmapCell")) {
            ++heatmap;
        } else if (name == QStringLiteral("swarmThreatZone")) {
            ++threats;
        }
        for (QQuickItem* child : item->childItems()) {
            self(self, child);
        }
    };

    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        QQuickWindow* quickWindow = qobject_cast<QQuickWindow*>(window);
        if (!quickWindow || !quickWindow->isVisible()) {
            continue;
        }
        countVisualItems(countVisualItems, quickWindow->contentItem());
    }
    return {{QStringLiteral("situation_layers"), situationLayers},
            {QStringLiteral("tracks"), visibleTrackIds.size()},
            {QStringLiteral("heatmap"), heatmap},
            {QStringLiteral("threats"), threats}};
}

void SwarmAcceptanceServer::_writeResponse(QTcpSocket* socket, const QJsonObject& response) const
{
    if (!socket) {
        return;
    }
    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    socket->write("\n");
}
