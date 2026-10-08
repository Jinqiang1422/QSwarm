#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>
#include <memory>

#include "BaseClasses/VehicleTest.h"

class SwarmManager;
class Vehicle;

/// Swarm 多机集成测试夹具：每个用例前连接主 MockLink、隔离集群约束/任务配置并创建 SwarmManager，
/// 用例后断开附加 MockLink 并恢复配置。只含夹具，不含测试函数。数据行的 tag 含 "ArduPilot" 时，
/// 主 MockLink 切到 ArduPilot（否则为 PX4）。
class SwarmVehicleTest : public VehicleTest
{
    Q_OBJECT

public:
    using VehicleTest::VehicleTest;
    ~SwarmVehicleTest() override;

protected slots:
    void init() override;
    void cleanup() override;

protected:
    struct Reposition
    {
        QGeoCoordinate target;
        double speed = 0;

        [[nodiscard]] bool valid() const { return target.isValid(); }
    };

    /// 收到的最后一条 COMMAND_INT；仅当它是 MAV_FRAME_GLOBAL（AMSL）坐标系下的 MAV_CMD_DO_REPOSITION
    /// 时才解码，否则返回无效值。编队指令总是经 COMMAND_INT、MAV_FRAME_GLOBAL 下发。
    [[nodiscard]] static Reposition lastFormationReposition(MockLink* link);
    /// 覆盖长机的地速遥测。MockLink 不会发送 VFR_HUD，所以这个覆盖会一直生效到测试结束、
    /// 飞行器被拆除为止（不会被下一帧遥测改回）。
    static void setLeaderGroundSpeed(Vehicle* vehicle, double speed);

    /// 追加 count 架 PX4 MockLink 并等待初始连接完成
    void addVehicles(int count);
    /// 全体起飞到 10 m 并等待；followers 返回主机以外的成员（按 id 升序）
    void takeOffAll(QList<Vehicle*>& followers);

    std::unique_ptr<SwarmManager> _manager;
    QPointer<MockLink> _secondaryLink;
    QList<QPointer<MockLink>> _extraLinks;

private:
    QString _configPath;
    QByteArray _previousConfig;
    bool _hadConfig = false;
};
