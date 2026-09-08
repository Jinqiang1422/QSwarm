#pragma once

#include <QtCore/QElapsedTimer>
#include <functional>
#include <utility>

/// Process-local deadlines share an epoch; wall time is reserved for wire timestamps and audit records.
class SwarmClock
{
public:
    static qint64 nowMs()
    {
        if (_testClock) {
            return _testClock();
        }
        static const QElapsedTimer timer = [] {
            QElapsedTimer value;
            value.start();
            return value;
        }();
        return timer.elapsed();
    }

    static void setTestClock(std::function<qint64()> clock) { _testClock = std::move(clock); }

private:
    inline static std::function<qint64()> _testClock;
};
