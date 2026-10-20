#pragma once

#include "MCTPConstants.hpp"
#include "MCTPDiscovery.hpp"
#include "MCTPReactorConfig.hpp"
#include "PeriodicTask.hpp"
#include "Utils.hpp"

#include <sdbusplus/asio/connection.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

/**
 * @brief Queries MCTP bridge routing tables via D-Bus
 *
 * This replaces the getRoutingTable() free function that was
 * previously defined in MCTPReactorMain.cpp, which itself was
 * a C++ port of mctp-get-routing-table.sh.
 */
class MCTPRoutingTableDiscovery : public MCTPDiscovery
{
  public:
    MCTPRoutingTableDiscovery(
        const std::shared_ptr<sdbusplus::asio::connection>& bus,
        const RoutingTableConfig& config);
    ~MCTPRoutingTableDiscovery() override = default;

    void run() override;
    std::string_view name() const override
    {
        return "RoutingTable";
    }

    /// Re-trigger discovery, refresh the routing table, and resume the
    /// periodic task when host comes up.
    void onHostOn(uint8_t /*hostIndex*/) override
    {
        notifyHostOn();
        if (task)
        {
            task->resume();
        }
    }
    /// Pause the periodic task and drop all known endpoints when host goes
    /// away.
    void onHostOff(uint8_t /*hostIndex*/) override
    {
        if (task)
        {
            task->pause();
        }
        removeAllEndpoint();
    }

    void notifyHostOn();

  private:
    /**
     * @brief Start (or restart) the periodic routing table discovery task
     *        that repeatedly invokes run() at the given interval. Called
     *        once from the constructor.
     */
    void startTask(std::chrono::seconds interval);

    const RoutingTableConfig& config;
    std::chrono::steady_clock::time_point hostOnTime{};

    // Periodic routing table discovery task, started via startTask()
    std::optional<PeriodicTask> task;
};
