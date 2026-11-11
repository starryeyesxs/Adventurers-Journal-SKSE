#pragma once

#include <cstdint>
#include <string>

struct QuestTimes
{
    bool hasAccepted = false;
    bool hasCompleted = false;
    bool hasTracked = false;
    bool tracked = false;
    float acceptedDays = 0.0f;
    float completedDays = 0.0f;
    std::uint64_t completionOrder = 0;
    std::string acceptedDate;
    std::string completedDate;
};

inline bool BackfillQuestTime(QuestTimes& times, bool completed, float days, const std::string& date)
{
    if (completed) {
        if (times.hasCompleted) {
            return false;
        }
        times.hasCompleted = true;
        times.completedDays = days;
        times.completedDate = date;
    } else {
        if (times.hasAccepted) {
            return false;
        }
        times.hasAccepted = true;
        times.acceptedDays = days;
        times.acceptedDate = date;
    }
    return true;
}
