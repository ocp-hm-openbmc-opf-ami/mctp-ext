#include "MCTPRoutingTableDiscovery.hpp"

#include "MCTPConstants.hpp"
#include "Utils.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/message.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

PHOSPHOR_LOG2_USING;

using namespace std::chrono_literals;

MCTPRoutingTableDiscovery::MCTPRoutingTableDiscovery(
    const std::shared_ptr<sdbusplus::asio::connection>& bus,
    const RoutingTableConfig& config) : MCTPDiscovery(bus), config(config)
{
    startTask(config.pollingInterval);
}

void MCTPRoutingTableDiscovery::startTask(std::chrono::seconds interval)
{
    task.emplace(bus->get_io_context(), interval, [this]() { run(); });
}

void MCTPRoutingTableDiscovery::run()
{
    auto now = std::chrono::steady_clock::now();
    if (hostOnTime.time_since_epoch().count() != 0 &&
        now - hostOnTime < std::chrono::minutes(1))
    {
        debug("Routing table discovery skipped, within 2 min after host on");
        return;
    }

    try
    {
        auto methodCall = bus->new_method_call(
            mctp::dbus::service.data(), mctp::dbus::basePath.data(),
            "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");

        auto reply = bus->call(methodCall, 3000ms);
        ManagedObjectType objectMap;
        reply.read(objectMap);

        for (const auto& [path, interfaces] : objectMap)
        {
            if (interfaces.find(mctp::dbus::bridgeInterface.data()) !=
                interfaces.end())
            {
                try
                {
                    auto routingCall = bus->new_method_call(
                        mctp::dbus::service.data(), path.str.c_str(),
                        mctp::dbus::bridgeInterface.data(), "GetRoutingTable");

                    // GetRoutingTable returns SD_BUS_NO_RESULT in mctpd;
                    // it triggers async routing table retrieval internally.
                    bus->call(routingCall, 3000ms);
                }
                catch (const std::exception& e)
                {
                    warning(
                        "Failed to get routing table from {BRIDGE_PATH}: {EXCEPTION}",
                        "BRIDGE_PATH", path.str, "EXCEPTION", e);
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        warning("Failed to enumerate MCTP bridges: {EXCEPTION}", "EXCEPTION",
                e);
    }
}

void MCTPRoutingTableDiscovery::notifyHostOn()
{
    hostOnTime = std::chrono::steady_clock::now();
}
