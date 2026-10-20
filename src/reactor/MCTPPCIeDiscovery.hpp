#pragma once

#include "MCTPConstants.hpp"
#include "MCTPDiscovery.hpp"
#include "MCTPReactorConfig.hpp"
#include "PeriodicTask.hpp"

#include <sdbusplus/asio/connection.hpp>

#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

/**
 * @brief PCIe MCTP endpoint discovery and setup
 */
class MCTPPCIeDiscovery : public MCTPDiscovery
{
  public:
    MCTPPCIeDiscovery(const std::shared_ptr<sdbusplus::asio::connection>& bus,
                      const PCIeDiscoveryConfig& config);
    ~MCTPPCIeDiscovery() override = default;

    void run() override;
    std::string_view name() const override
    {
        return "PCIe";
    }

    /// Bring the corresponding mctppciN interface(s) up, re-configure
    /// local EID, and start (or resume) the bus-owner discovery task on
    /// host power-on. Only interfaces whose hostIndex matches the host
    /// that just powered on are re-initialized. The periodic task is only
    /// started/needed if at least one enabled interface has the
    /// "bus-owner" role.
    void onHostOn(uint8_t hostIndex) override
    {
        resetDiscoveryState(hostIndex);
        if (!task)
        {
            if (needsBusOwnerTask())
            {
                startTask(config.pollingInterval);
            }
        }
        else
        {
            task->resume();
        }
    }

    /// Pause the bus-owner discovery task on host power-off.
    void onHostOff(uint8_t /*hostIndex*/) override
    {
        if (task)
        {
            task->pause();
        }
    }

    /**
     * @brief Re-initialize the PCIe interface(s) belonging to the given
     *        host index (ensureInterfaceReady()).
     * @param hostIndex Host index (0, 1, 2) whose interfaces should be
     *                  reset; interfaces for other hosts are left alone.
     */
    void resetDiscoveryState(uint8_t hostIndex);

  private:
    /// Bus-owner role flow: ask mctpd to SetupEndpoint downstream.
    void runBusOwnerMode();

    /**
     * @brief Start (or restart) the periodic bus-owner discovery task
     *        that repeatedly invokes run() at the given interval. Called
     *        from onHostOn() the first time a host powers on, if needed.
     */
    void startTask(std::chrono::seconds interval);

    /// Bring up config.interfaces[idx] via ensureInterfaceReady(); on
    /// failure, reschedule itself after PCIE_DISCOVERY_DELAY_TIME seconds
    /// via retryTimers[idx] until it succeeds or the object is destroyed.
    void bringUpInterface(size_t idx);

    /// @return true if at least one enabled interface has the
    ///         "bus-owner" role, i.e. the periodic discovery task is needed.
    bool needsBusOwnerTask() const
    {
        for (const auto& iface : config.interfaces)
        {
            if (iface.enabled && iface.role == "bus-owner")
            {
                return true;
            }
        }
        return false;
    }

    const PCIeDiscoveryConfig& config;

    // Per-interface last-reset timestamp (indexed like config.interfaces),
    // used to apply the post-reset cooldown independently per interface.
    std::vector<std::chrono::steady_clock::time_point> lastResetTimes;

    // Per-interface retry timer (indexed like config.interfaces), used to
    // retry bringUpInterface() until it succeeds when ensureInterfaceReady()
    // fails (e.g. transient netlink/driver race at boot).
    std::vector<boost::asio::steady_timer> retryTimers;

    // Periodic bus-owner discovery task, started via startTask()
    std::optional<PeriodicTask> task;
};
