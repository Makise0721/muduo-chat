#include "app/RecordingPublisher.hpp"

RecordingPublisher::RecordingPublisher() = default;

std::vector<OutboxPublishOutcome> RecordingPublisher::publish(
    const std::vector<OutboxPublishRequest>& batch, int64_t deadlineMs)
{
    batches_.push_back(batch);
    lastDeadlineMs_ = deadlineMs;
    std::vector<OutboxPublishOutcome> outcomes;
    outcomes.reserve(batch.size());
    for (size_t i = 0; i < batch.size(); ++i) {
        const OutboxPublishRequest& req = batch[i];
        requests_.push_back(req);
        OutboxPublishOutcome oc;
        const PublishFailure failure = failureFor(req);
        if (failure == PublishFailure::None) {
            oc.ok = true;
            oc.failure = PublishFailure::None;
        } else {
            oc.ok = false;
            oc.failure = failure;
            oc.error = "recording publisher injected failure";
        }
        outcomes.push_back(oc);
    }
    lastOutcomes_ = outcomes;
    return outcomes;
}

void RecordingPublisher::failEvent(uint64_t eventId, PublishFailure failure)
{
    failByEvent_[eventId] = failure;
}

void RecordingPublisher::failTopic(const std::string& topic, PublishFailure failure)
{
    failByTopic_[topic] = failure;
}

void RecordingPublisher::clearFailures()
{
    failByEvent_.clear();
    failByTopic_.clear();
}

size_t RecordingPublisher::publishCalls() const
{
    return batches_.size();
}

const std::vector<std::vector<OutboxPublishRequest> >& RecordingPublisher::batches() const
{
    return batches_;
}

const std::vector<OutboxPublishRequest>& RecordingPublisher::requests() const
{
    return requests_;
}

const std::vector<OutboxPublishOutcome>& RecordingPublisher::lastOutcomes() const
{
    return lastOutcomes_;
}

int64_t RecordingPublisher::lastDeadlineMs() const
{
    return lastDeadlineMs_;
}

PublishFailure RecordingPublisher::failureFor(const OutboxPublishRequest& req) const
{
    std::map<uint64_t, PublishFailure>::const_iterator it = failByEvent_.find(req.event.id);
    if (it != failByEvent_.end()) {
        return it->second;
    }
    std::map<std::string, PublishFailure>::const_iterator jt = failByTopic_.find(req.topic);
    if (jt != failByTopic_.end()) {
        return jt->second;
    }
    return PublishFailure::None;
}
