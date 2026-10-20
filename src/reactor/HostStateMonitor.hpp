#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

/**
 * @brief D-Bus namespace properties for power state monitoring
 */
struct HostPowerProperties
{
    std::string busname;
    std::string interface;
    std::string path;
    std::string property;
};

/**
 * @brief Get power properties for a specific host
 * @param hostIndex 0 for host0, 1 for host1, 2 for host2
 * @return HostPowerProperties configured for the host
 */
HostPowerProperties getHostPowerProperties(uint8_t hostIndex);

/**
 * @brief Monitors host power state and eSPI platform reset,
 *        controls discovery task lifecycle
 *
 * Supports single-host and multi-host configurations.
 *
 * Single-host mode (default, MULTI_HOST_MODE_SUPPORT=false):
 * - Monitors Host0 only via power namespace
 * - Single HostStateMonitor instance with hostIndex=0
 *
 * Multi-host mode (MULTI_HOST_MODE_SUPPORT=true):
 * - Monitors Host1 and Host2 only via power1 and power2 namespaces
 * - Two HostStateMonitor instances with hostIndex=1 and hostIndex=2
 * - Host0 is not monitored in multi-host mode
 *
 * Subscribes to:
 * 1. CurrentHostState on appropriate host path
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
     * @param hostOffCb        Optional callback(uint8_t hostIndex) for cleanup when host goes off
     * @param hostOnCb         Optional callback(uint8_t hostIndex) for re-init when host comes on
     * @param platformResetCb  Optional callback for eSPI platform reset cleanup
     * @param hostIndex        Host index (0, 1, 2) for multi-host support
     */
    HostStateMonitor(
        const std::shared_ptr<sdbusplus::asio::connection>& connection,
        std::function<void(uint8_t)> hostOffCb = nullptr,
        std::function<void(uint8_t)> hostOnCb = nullptr,
        std::function<void()> platformResetCb = nullptr,
        uint8_t hostIndex = 0);

    ~HostStateMonitor() = default;
    HostStateMonitor(const HostStateMonitor&) = delete;
    HostStateMonitor(HostStateMonitor&&) = delete;
    HostStateMonitor& operator=(const HostStateMonitor&) = delete;
    HostStateMonitor& operator=(HostStateMonitor&&) = delete;

    bool isHostOn() const
    {
        return hostOn;
    }

    /// @return The host index (0, 1, or 2) this monitor is tracking
    uint8_t getHostIndex() const { return hostIndex; }

  private:
    void queryInitialState();
    void scheduleInitialStateRetry();
    void onHostStateChanged(sdbusplus::message_t& msg);
    void onPlatformStateChanged(sdbusplus::message_t& msg);
    void handleStateChange(bool newHostOn);
    void handlePlatformReset();

    std::shared_ptr<sdbusplus::asio::connection> bus;
    uint8_t hostIndex = 0;
    HostPowerProperties hostPowerProps;
    sdbusplus::bus::match_t hostStateMatch;
    sdbusplus::bus::match_t platformResetMatch;
    boost::asio::steady_timer debounceTimer;
    boost::asio::steady_timer initialStateRetryTimer;

    std::function<void(uint8_t)> onHostOff;
    std::function<void(uint8_t)> onHostOn;
    std::function<void()> onPlatformReset;
    bool hostOn = false;
    unsigned int initialStateRetryAttempt = 0;
};
