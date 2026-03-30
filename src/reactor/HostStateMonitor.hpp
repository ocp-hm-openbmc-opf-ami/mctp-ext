#pragma once

#include "PeriodicTask.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief Monitors host power state and eSPI platform reset,
 *        controls discovery task lifecycle
 *
 * Subscribes to:
 * 1. CurrentHostState on /xyz/openbmc_project/state/host0
 *    - Host off: pause discovery tasks immediately
 *    - Host on: resume after 10s debounce
 *
 * 2. ESpiPlatformReset on /xyz/openbmc_project/misc/platform_state
 *    - Reset detected (true): clean up markers, restart mctpd
 *      (reactor exits via nameOwnerChanged, systemd restarts both)
 *
 * This replaces mctp-host-state.sh and the host-monitoring parts
 * of mctp-discovery.sh.
 */
class HostStateMonitor
{
  public:
    /**
     * @param connection       D-Bus connection
     * @param taskList         Discovery PeriodicTask pointers to pause/resume
     * @param hostOffCb        Optional callback for cleanup when host goes off
     * @param hostOnCb         Optional callback for re-init when host comes on
     * @param platformResetCb  Optional callback for eSPI platform reset cleanup
     */
    HostStateMonitor(
        const std::shared_ptr<sdbusplus::asio::connection>& connection,
        std::vector<PeriodicTask*> taskList,
        std::function<void()> hostOffCb = nullptr,
        std::function<void()> hostOnCb = nullptr,
        std::function<void()> platformResetCb = nullptr);

    ~HostStateMonitor() = default;
    HostStateMonitor(const HostStateMonitor&) = delete;
    HostStateMonitor(HostStateMonitor&&) = delete;
    HostStateMonitor& operator=(const HostStateMonitor&) = delete;
    HostStateMonitor& operator=(HostStateMonitor&&) = delete;

    bool isHostOn() const { return hostOn; }

  private:
    void queryInitialState();
    void scheduleInitialStateRetry();
    void onHostStateChanged(sdbusplus::message_t& msg);
    void onPlatformStateChanged(sdbusplus::message_t& msg);
    void handleStateChange(bool newHostOn);
    void handlePlatformReset();

    std::shared_ptr<sdbusplus::asio::connection> bus;
    sdbusplus::bus::match_t hostStateMatch;
    sdbusplus::bus::match_t platformResetMatch;
    boost::asio::steady_timer debounceTimer;
    boost::asio::steady_timer initialStateRetryTimer;

    std::vector<PeriodicTask*> tasks;
    std::function<void()> onHostOff;
    std::function<void()> onHostOn;
    std::function<void()> onPlatformReset;
    bool hostOn = false;
    unsigned int initialStateRetryAttempt = 0;
};
