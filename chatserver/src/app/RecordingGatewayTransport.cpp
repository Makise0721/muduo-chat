#include "app/RecordingGatewayTransport.hpp"

#include <utility>

void RecordingGatewayTransport::scriptStaleEpoch()
{
    scriptStale_ = true;
    scriptUnreachable_ = false;
}

void RecordingGatewayTransport::scriptUnreachable()
{
    scriptStale_ = false;
    scriptUnreachable_ = true;
}

void RecordingGatewayTransport::clearScript()
{
    scriptStale_ = false;
    scriptUnreachable_ = false;
}

GatewayDeliverResult RecordingGatewayTransport::deliver(const DeliveryRoute& route,
                                                        const DeliveryAttempt& attempt)
{
    Record rec;
    rec.route = route;
    rec.attempt = attempt;
    if (scriptStale_) {
        scriptStale_ = false;  // 一次性脚本：触发即清除
        rec.result.staleEpoch = true;
        rec.result.error = "scripted stale epoch (dropped)";
    } else if (scriptUnreachable_) {
        scriptUnreachable_ = false;  // 一次性脚本：触发即清除
        rec.result.unreachable = true;
        rec.result.error = "scripted unreachable";
    } else {
        rec.result.ok = true;
    }
    records_.push_back(rec);
    return rec.result;
}
