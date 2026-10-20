#include "HostStateMonitor.hpp"

#include "Utils.hpp"

#include <boost/container/flat_map.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/message.hpp>

#include <chrono>
#include <string>
#include <variant>

PHOSPHOR_LOG2_USING;

namespace platformState
{
constexpr const char* path = "/xyz/openbmc_project/misc/platform_state";
constexpr const char* interface = "xyz.openbmc_project.State.PlatformState";
constexpr const char* property = "ESpiPlatformReset";
} // namespace platformState

/**
 * @brief Get power properties for a specific host
 * @param hostIndex 0 for host0, 1 for host1, 2 for host2
 * @return HostPowerProperties configured for the host
 */
HostPowerProperties getHostPowerProperties(uint8_t hostIndex)
{
    switch (hostIndex)
    {
#ifdef MULTI_HOST_MODE_SUPPORT
        case 1:
            return {
                .busname = power1::busname,
                .interface = power1::interface,
                .path = power1::path,
                .property = power1::property,
            };
        case 2:
            return {
                .busname = power2::busname,
                .interface = power2::interface,
                .path = power2::path,
                .property = power2::property,
            };
#endif
        case 0:
        default:
            return {
                .busname = power::busname,
                .interface = power::interface,
                .path = power::path,
                .property = power::property,
            };
    }
}

HostStateMonitor::HostStateMonitor(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    std::function<void(uint8_t)> hostOffCb,
    std::function<void(uint8_t)> hostOnCb,
    std::function<void()> platformResetCb,
    uint8_t hostIndex) :
    bus(connection),
    hostIndex(hostIndex),
    hostPowerProps(getHostPowerProperties(hostIndex)),
    hostStateMatch(
        static_cast<sdbusplus::bus_t&>(*connection),
        "type='signal',interface='" + std::string(properties::interface) +
            "',path='" + hostPowerProps.path + "',arg0='" +
            hostPowerProps.interface + "'",
        [this](sdbusplus::message_t& msg) { onHostStateChanged(msg); }),
    platformResetMatch(
        static_cast<sdbusplus::bus_t&>(*connection),
        "type='signal',interface='" + std::string(properties::interface) +
            "',path='" + std::string(platformState::path) + "'",
        [this](sdbusplus::message_t& msg) { onPlatformStateChanged(msg); }),
    debounceTimer(connection->get_io_context()),
    initialStateRetryTimer(connection->get_io_context()),
    onHostOff(std::move(hostOffCb)),
    onHostOn(std::move(hostOnCb)), onPlatformReset(std::move(platformResetCb))
{
    queryInitialState();
}

void HostStateMonitor::queryInitialState()
{
    try
    {
        auto method = bus->new_method_call(
            hostPowerProps.busname.c_str(), hostPowerProps.path.c_str(),
            properties::interface, properties::get);
        method.append(hostPowerProps.interface, hostPowerProps.property);

        auto reply = bus->call(method);
        std::variant<std::string> value;
        reply.read(value);

        bool on = std::get<std::string>(value).ends_with(".Running");
        info("Host{HOST} state initial query: {STATE}",
             "HOST", hostIndex, "STATE", on ? "Running" : "Off");
        initialStateRetryAttempt = 0;
        initialStateRetryTimer.cancel();
        handleStateChange(on);
    }
    catch (const std::exception& e)
    {
        warning("Failed to query initial host{HOST} state: {EXCEPTION}",
                "HOST", hostIndex, "EXCEPTION", e);
        scheduleInitialStateRetry();
    }
}

void HostStateMonitor::scheduleInitialStateRetry()
{
    constexpr unsigned int maxDelaySec = 30;
    constexpr unsigned int baseDelaySec = 2;
    unsigned int delaySec = std::min(
        maxDelaySec, baseDelaySec << std::min(4u, initialStateRetryAttempt));
    initialStateRetryAttempt++;

    info("Retrying host{HOST} state query in {DELAY}s (attempt {ATTEMPT})",
         "HOST", hostIndex, "DELAY", delaySec, "ATTEMPT", initialStateRetryAttempt);

    initialStateRetryTimer.expires_after(std::chrono::seconds(delaySec));
    initialStateRetryTimer.async_wait(
        [this](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                return;
            }
            if (ec)
            {
                warning("Initial state retry timer error: {ERROR}", "ERROR",
                        ec.message());
                return;
            }
            queryInitialState();
        });
}

void HostStateMonitor::onHostStateChanged(sdbusplus::message_t& msg)
{
    try
    {
        std::string objectName;
        boost::container::flat_map<std::string, std::variant<std::string>>
            values;
        msg.read(objectName, values);

        auto findState = values.find(hostPowerProps.property);
        if (findState == values.end())
        {
            return;
        }

        bool on =
            std::get<std::string>(findState->second).ends_with(".Running");

        if (!on)
        {
            // Host off: act immediately
            debounceTimer.cancel();
            info("Host{HOST} powered off, pausing discovery tasks",
                 "HOST", hostIndex);
            handleStateChange(false);
            return;
        }

        // Host on: debounce with 10s delay (matching Utils.cpp pattern)
        debounceTimer.expires_after(std::chrono::seconds(10));
        debounceTimer.async_wait(
            [this](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    return;
                }
                if (ec)
                {
                    warning("Debounce timer error: {ERROR}",
                            "ERROR", ec.message());
                    return;
                }
                info("Host{HOST} powered on (debounced), resuming discovery tasks",
                     "HOST", hostIndex);
                handleStateChange(true);
            });
    }
    catch (const std::exception& e)
    {
        warning("Failed to handle host{HOST} state signal: {EXCEPTION}",
                "HOST", hostIndex, "EXCEPTION", e);
    }
}

void HostStateMonitor::onPlatformStateChanged(sdbusplus::message_t& msg)
{
    try
    {
        std::string objectName;
        boost::container::flat_map<std::string, std::variant<bool>> values;
        msg.read(objectName, values);

        auto findReset = values.find(platformState::property);
        if (findReset == values.end())
        {
            return;
        }

        bool resetActive = std::get<bool>(findReset->second);
        if (!resetActive)
        {
            return;
        }

        info("eSPI platform reset detected (host{HOST}), "
             "triggering cleanup and mctpd restart",
             "HOST", hostIndex);
        handlePlatformReset();
    }
    catch (const std::exception& e)
    {
        warning("Failed to handle platform state signal for host{HOST}: {EXCEPTION}",
                "HOST", hostIndex, "EXCEPTION", e);
    }
}

void HostStateMonitor::handleStateChange(bool newHostOn)
{
    if (newHostOn == hostOn)
    {
        return;
    }

    hostOn = newHostOn;

    if (!hostOn)
    {
        if (onHostOff)
        {
            onHostOff(hostIndex);
        }
    }
    else
    {
        if (onHostOn)
        {
            onHostOn(hostIndex);
        }
    }
}

void HostStateMonitor::handlePlatformReset()
{
    // Run platform-reset-specific cleanup and re-init. Each MCTPDiscovery
    // subclass is responsible for pausing/resuming its own periodic task
    // (if any) around this via its onHostOff()/onHostOn() hooks, which are
    // not invoked here; platform reset is a distinct in-band event.
    if (onPlatformReset)
    {
        onPlatformReset();
    }
}
