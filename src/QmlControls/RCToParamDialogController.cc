#include "RCToParamDialogController.h"

#include <cmath>

#include "Fact.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "QGCLoggingCategory.h"
#include "Vehicle.h"

QGC_LOGGING_CATEGORY(RCToParamDialogControllerLog, "QMLControls.RCToParamDialogController")

QMap<QString, FactMetaData*> RCToParamDialogController::_metaDataMap;

RCToParamDialogController::RCToParamDialogController(QObject* parent)
    : QObject(parent),
      _scaleFact(new Fact(0, _scaleFactName, FactMetaData::valueTypeDouble, this)),
      _centerFact(new Fact(0, _centerFactName, FactMetaData::valueTypeDouble, this)),
      _minFact(new Fact(0, _minFactName, FactMetaData::valueTypeDouble, this)),
      _maxFact(new Fact(0, _maxFactName, FactMetaData::valueTypeDouble, this))
{
    // qCDebug(RCToParamDialogControllerLog) << Q_FUNC_INFO << this;

    if (_metaDataMap.isEmpty()) {
        _metaDataMap = FactMetaData::createMapFromJsonFile(QStringLiteral(":/json/RCToParamDialog.FactMetaData.json"),
                                                           nullptr /* QObject parent */);
    }

    _scaleFact->setMetaData(_metaDataMap[_scaleFactName], true /* setDefaultFromMetaData */);
    _centerFact->setMetaData(_metaDataMap[_centerFactName]);
    _minFact->setMetaData(_metaDataMap[_minFactName]);
    _maxFact->setMetaData(_metaDataMap[_maxFactName]);
}

RCToParamDialogController::~RCToParamDialogController()
{
    // qCDebug(RCToParamDialogControllerLog) << Q_FUNC_INFO << this;`
}

void RCToParamDialogController::setTuningFact(Fact* tuningFact)
{
    bind(MultiVehicleManager::instance()->activeVehicle(), tuningFact);
}

Fact* RCToParamDialogController::tuningFact() const
{
    return _tuningFact.data();
}

bool RCToParamDialogController::_bindingValid() const
{
    Vehicle* vehicle = _vehicle.data();
    if (!vehicle || !_tuningFact || !MultiVehicleManager::instance()->activeVehicleAvailable() ||
        vehicle != MultiVehicleManager::instance()->activeVehicle()) {
        return false;
    }
    ParameterManager* parameters = vehicle->parameterManager();
    return parameters && parameters->parameterExists(_tuningFact->componentId(), _tuningFact->name()) &&
           parameters->getParameter(_tuningFact->componentId(), _tuningFact->name()) == _tuningFact;
}

void RCToParamDialogController::_invalidate()
{
    for (const auto& connection : std::as_const(_bindingConnections)) {
        disconnect(connection);
    }
    _bindingConnections.clear();
    _vehicle.clear();
    _tuningFact.clear();
    _ready = false;
    emit readyChanged(false);
    emit tuningFactChanged(nullptr);
}

void RCToParamDialogController::bind(Vehicle* vehicle, Fact* fact)
{
    _invalidate();
    _vehicle = vehicle;
    _tuningFact = fact;
    if (!vehicle || !_bindingValid()) {
        _invalidate();
        return;
    }
    emit tuningFactChanged(fact);

    _centerFact->setRawValue(_tuningFact->rawValue().toDouble());
    _minFact->setRawValue(_tuningFact->rawMin().toDouble());
    _maxFact->setRawValue(_tuningFact->rawMax().toDouble());

    _bindingConnections.append(
        connect(fact, &Fact::vehicleUpdated, this, &RCToParamDialogController::_parameterUpdated));
    _bindingConnections.append(connect(fact, &QObject::destroyed, this, &RCToParamDialogController::_invalidate));
    _bindingConnections.append(connect(vehicle, &QObject::destroyed, this, &RCToParamDialogController::_invalidate));
    _bindingConnections.append(connect(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged,
                                       this, &RCToParamDialogController::_invalidate));
    _bindingConnections.append(connect(MultiVehicleManager::instance(),
                                       &MultiVehicleManager::activeVehicleAvailableChanged, this,
                                       [this](bool available) {
                                           if (!available)
                                               _invalidate();
                                       }));
    _bindingConnections.append(
        connect(MultiVehicleManager::instance(), &MultiVehicleManager::vehicleRemoved, this, [this](Vehicle* removed) {
            if (_vehicle == removed) {
                _invalidate();
            }
        }));
    vehicle->parameterManager()->refreshParameter(fact->componentId(), fact->name());
}

void RCToParamDialogController::_parameterUpdated()
{
    _ready = _bindingValid();
    emit readyChanged(_ready);
}

bool RCToParamDialogController::apply(const QString& scale, const QString& center, int tuningId, const QString& minimum,
                                      const QString& maximum)
{
    Vehicle* vehicle = _vehicle.data();
    if (!vehicle || !_ready || !_bindingValid() || tuningId < 0 || tuningId > 2) {
        return false;
    }
    const QStringList inputs{scale, center, minimum, maximum};
    const QList<Fact*> facts{_scaleFact, _centerFact, _minFact, _maxFact};
    QList<double> values;
    for (int i = 0; i < inputs.size(); ++i) {
        bool ok = false;
        const double value = inputs[i].toDouble(&ok);
        if (!ok || !std::isfinite(value) || !facts[i]->validate(inputs[i], false).isEmpty()) {
            return false;
        }
        values.append(value);
    }
    if (values[2] > values[3] || values[1] < values[2] || values[1] > values[3]) {
        return false;
    }
    vehicle->sendParamMapRC(_tuningFact->name(), values[0], values[1], tuningId, values[2], values[3]);
    return true;
}
