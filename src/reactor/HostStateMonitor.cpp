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

HostStateMonitor::HostStateMonitor(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    std::vector<PeriodicTask*> taskList, std::function<void()> hostOffCb,
    std::function<void()> hostOnCb, std::function<void()> platformResetCb) :
    bus(connection),
    hostStateMatch(
        static_cast<sdbusplus::bus_t&>(*connection),
        "type='signal',interface='" + std::string(properties::interface) +
            "',path='" + std::string(power::path) + "',arg0='" +
            std::string(power::interface) + "'",
        [this](sdbusplus::message_t& msg) { onHostStateChanged(msg); }),
    platformResetMatch(
        static_cast<sdbusplus::bus_t&>(*connection),
        "type='signal',interface='" + std::string(properties::interface) +
            "',path='" + std::string(platformState::path) + "'",
        [this](sdbusplus::message_t& msg) { onPlatformStateChanged(msg); }),
    debounceTimer(connection->get_io_context()),
    initialStateRetryTimer(connection->get_io_context()),
    tasks(std::move(taskList)), onHostOff(std::move(hostOffCb)),
    onHostOn(std::move(hostOnCb)), onPlatformReset(std::move(platformResetCb))
{
    queryInitialState();
}

void HostStateMonitor::queryInitialState()
{
    try
    {
        auto method =
            bus->new_method_call(power::busname, power::path,
                                 properties::interface, properties::get);
        method.append(power::interface, power::property);

        auto reply = bus->call(method);
        std::variant<std::string> value;
        reply.read(value);

        bool on = std::get<std::string>(value).ends_with(".Running");
        info("Host state initial query: {STATE}", "STATE",
             on ? "Running" : "Off");
        initialStateRetryAttempt = 0;
        initialStateRetryTimer.cancel();
        handleStateChange(on);
    }
    catch (const std::exception& e)
    {
        warning("Failed to query initial host state: {EXCEPTION}", "EXCEPTION",
                e);
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

    info("Retrying host state query in {DELAY}s (attempt {ATTEMPT})", "DELAY",
         delaySec, "ATTEMPT", initialStateRetryAttempt);

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

        auto findState = values.find(power::property);
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
            info("Host powered off, pausing discovery tasks");
            handleStateChange(false);
            return;
        }

        // Host on: debounce with 10s delay (matching Utils.cpp pattern)
        debounceTimer.expires_after(std::chrono::seconds(10));
        debounceTimer.async_wait([this](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                return;
            }
            if (ec)
            {
                warning("Debounce timer error: {ERROR}", "ERROR", ec.message());
                return;
            }
            info("Host powered on (debounced), resuming discovery tasks");
            handleStateChange(true);
        });
    }
    catch (const std::exception& e)
    {
        warning("Failed to handle host state signal: {EXCEPTION}", "EXCEPTION",
                e);
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

        info("eSPI platform reset detected, "
             "triggering cleanup and mctpd restart");
        handlePlatformReset();
    }
    catch (const std::exception& e)
    {
        warning("Failed to handle platform state signal: {EXCEPTION}",
                "EXCEPTION", e);
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
        for (auto* task : tasks)
        {
            task->pause();
        }
        if (onHostOff)
        {
            onHostOff();
        }
    }
    else
    {
        if (onHostOn)
        {
            onHostOn();
        }
        for (auto* task : tasks)
        {
            task->resume();
        }
    }
}

void HostStateMonitor::handlePlatformReset()
{
    // Pause all tasks first
    for (auto* task : tasks)
    {
        task->pause();
    }

    // Run platform-reset-specific cleanup and re-init
    if (onPlatformReset)
    {
        onPlatformReset();
    }

    // Resume tasks after reset handling
    for (auto* task : tasks)
    {
        task->resume();
    }
}
