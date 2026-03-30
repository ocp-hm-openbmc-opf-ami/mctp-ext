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

        info("=== MCTP Bridge Objects ===");
        info("Service: {SERVICE}", "SERVICE", mctp::dbus::service);
        info("Interface: {INTERFACE}", "INTERFACE",
             mctp::dbus::bridgeInterface);

        for (const auto& [path, interfaces] : objectMap)
        {
            if (interfaces.find(mctp::dbus::bridgeInterface.data()) !=
                interfaces.end())
            {
                info("Found bridge: {BRIDGE_PATH}", "BRIDGE_PATH", path.str);

                try
                {
                    auto routingCall = bus->new_method_call(
                        mctp::dbus::service.data(), path.str.c_str(),
                        mctp::dbus::bridgeInterface.data(), "GetRoutingTable");

                    // GetRoutingTable returns SD_BUS_NO_RESULT in mctpd;
                    // it triggers async routing table retrieval internally.
                    bus->call(routingCall, 3000ms);

                    info("Triggered routing table retrieval for {BRIDGE_PATH}",
                         "BRIDGE_PATH", path.str);
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
