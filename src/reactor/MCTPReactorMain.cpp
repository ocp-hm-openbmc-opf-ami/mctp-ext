#include "config.h"

#include "DBusAssociationServer.hpp"
#include "HostStateMonitor.hpp"
#include "MCTPConfigMonitor.hpp"
#include "MCTPConstants.hpp"
#include "MCTPEndpoint.hpp"
#include "MCTPI2CDiscovery.hpp"
#include "MCTPI3CDiscovery.hpp"
#include "MCTPPCIeDiscovery.hpp"
#include "MCTPReactor.hpp"
#include "MCTPReactorConfig.hpp"
#include "MCTPRoutingTableDiscovery.hpp"
#include "MCTPUSBDiscovery.hpp"
#include "PeriodicTask.hpp"
#include "ReactorDebugMonitor.hpp"
#include "Utils.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <cstdint>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

PHOSPHOR_LOG2_USING;

static std::shared_ptr<MCTPDevice> deviceFromConfig(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const SensorData& config)
{
    try
    {
        std::optional<SensorBaseConfigMap> iface;
        iface = I2CMCTPDDevice::match(config);
        if (iface)
        {
            info("Creating I2CMCTPDDevice");
            return I2CMCTPDDevice::from(connection, *iface);
        }

        iface = I3CMCTPDDevice::match(config);
        if (iface)
        {
            info("Creating I3CMCTPDDevice");
            return I3CMCTPDDevice::from(connection, *iface);
        }
    }
    catch (const std::invalid_argument& ex)
    {
        error("Unable to create device: {EXCEPTION}", "EXCEPTION", ex);
    }

    return {};
}

static void addInventory(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const std::shared_ptr<MCTPReactor>& reactor, sdbusplus::message_t& msg)
{
    auto [path,
          exposed] = msg.unpack<sdbusplus::message::object_path, SensorData>();
    try
    {
        reactor->manageMCTPDevice(path, deviceFromConfig(connection, exposed));
    }
    catch (const std::logic_error& e)
    {
        error(
            "Addition of inventory at '{INVENTORY_PATH}' caused an invalid program state: {EXCEPTION}",
            "INVENTORY_PATH", path, "EXCEPTION", e);
    }
    catch (const std::system_error& e)
    {
        error(
            "Failed to manage device described by inventory at '{INVENTORY_PATH}: {EXCEPTION}'",
            "INVENTORY_PATH", path, "EXCEPTION", e);
    }
}

static void removeInventory(const std::shared_ptr<MCTPReactor>& reactor,
                            sdbusplus::message_t& msg)
{
    auto [path, removed] =
        msg.unpack<sdbusplus::message::object_path, std::set<std::string>>();
    try
    {
        if (I2CMCTPDDevice::match(removed) || I3CMCTPDDevice::match(removed))
        {
            reactor->unmanageMCTPDevice(path.str);
        }
    }
    catch (const std::logic_error& e)
    {
        error(
            "Removal of inventory at '{INVENTORY_PATH}' caused an invalid program state: {EXCEPTION}",
            "INVENTORY_PATH", path, "EXCEPTION", e);
    }
    catch (const std::system_error& e)
    {
        error(
            "Failed to unmanage device described by inventory at '{INVENTORY_PATH}: {EXCEPTION}'",
            "INVENTORY_PATH", path, "EXCEPTION", e);
    }
}

// static void manageMCTPEntity(
//     const std::shared_ptr<sdbusplus::asio::connection>& connection,
//     const std::shared_ptr<MCTPReactor>& reactor, ManagedObjectType& entities)
// {
//     for (const auto& [path, config] : entities)
//     {
//         try
//         {
//             reactor->manageMCTPDevice(path,
//                                       deviceFromConfig(connection, config));
//         }
//         catch (const std::logic_error& e)
//         {
//             error(
//                 "Addition of inventory at '{INVENTORY_PATH}' caused an
//                 invalid program state: {EXCEPTION}", "INVENTORY_PATH", path,
//                 "EXCEPTION", e);
//         }
//         catch (const std::system_error& e)
//         {
//             error(
//                 "Failed to manage device described by inventory at
//                 '{INVENTORY_PATH}: {EXCEPTION}'", "INVENTORY_PATH", path,
//                 "EXCEPTION", e);
//         }
//     }
// }

static void exitReactor(boost::asio::io_context* io, sdbusplus::message_t& msg)
{
    auto name = msg.unpack<std::string>();
    info("Shutting down mctpreactor, lost dependency '{SERVICE_NAME}'",
         "SERVICE_NAME", name);
    io->stop();
}

int main(int /*argc*/, char* /*argv*/[])
try
{
    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);

    DBusAssociationServer associationServer(systemBus);
    auto reactor = std::make_shared<MCTPReactor>(associationServer);
    reactor->buildDiscovery(io, systemBus);

    info("Reactor initialized with {HOST_COUNT} host monitor(s)",
         "HOST_COUNT", MCTPDiscovery::hostMonitorCount());

    // Setup signal handler for graceful shutdown on SIGTERM
    boost::asio::signal_set signals(io, SIGTERM, SIGINT);
    signals.async_wait(
        [&io, reactor](const boost::system::error_code& ec, int signum) {
            if (!ec)
            {
                info("Received signal {SIGNAL}, shutting down gracefully",
                     "SIGNAL", signum);
                if (reactor->config.usb.enabled &&
                    reactor->config.usb.hotplugEnabled &&
                    reactor->discovery.usbDiscovery)
                {
                    info("Closing USB device handles");
                    reactor->discovery.usbDiscovery->shutdownHotplug();
                }
                io.stop();
            }
        });

    using namespace sdbusplus::bus::match;

    const std::string entityManagerNameLostSpec =
        rules::nameOwnerChanged(mctp::dbus::entityManagerService.data());

    auto entityManagerNameLostMatch = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*systemBus), entityManagerNameLostSpec,
        [](sdbusplus::message_t& msg) {
            auto name = msg.unpack<std::string>();
            warning(
                "Dependency '{SERVICE_NAME}' left the bus, but mctpreactor will continue running",
                "SERVICE_NAME", name);
        });

    const std::string mctpdNameLostSpec =
        rules::nameOwnerChanged(mctp::dbus::service.data());

    auto mctpdNameLostMatch = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*systemBus), mctpdNameLostSpec,
        std::bind_front(exitReactor, &io));

    const std::string interfacesRemovedMatchSpec =
        rules::sender(mctp::dbus::entityManagerService.data()) +
        rules::interfacesRemovedAtPath(mctp::dbus::inventoryBasePath.data());

    auto interfacesRemovedMatch = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*systemBus), interfacesRemovedMatchSpec,
        std::bind_front(removeInventory, reactor));

    const std::string interfacesAddedMatchSpec =
        rules::sender(mctp::dbus::entityManagerService.data()) +
        rules::interfacesAddedAtPath(mctp::dbus::inventoryBasePath.data());

    auto interfacesAddedMatch = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*systemBus), interfacesAddedMatchSpec,
        std::bind_front(addInventory, systemBus, reactor));

    // Live reload: rebuild the discovery stack when Entity Manager publishes
    // updated MCTP configuration.
    MCTPConfigMonitor configMonitor(
        systemBus, io, [&io, systemBus, reactor]() {
            boost::asio::post(io, [systemBus, reactor, &io]() {
                info("Live-reloading MCTP reactor configuration");
                if (reactor->config.usb.enabled &&
                    reactor->config.usb.hotplugEnabled &&
                    reactor->discovery.usbDiscovery)
                {
                    reactor->discovery.usbDiscovery->shutdownHotplug();
                }
                reactor->resetDiscovery();
                try
                {
                    reactor->buildDiscovery(io, systemBus);
                    info("MCTP reactor configuration reloaded");
                }
                catch (const std::exception& e)
                {
                    error("Failed to rebuild discovery stack: {ERR}", "ERR",
                          e.what());
                }
            });
        });

    // Monitor /var/run/mctp_trace_on for runtime log level changes
    ReactorDebugMonitor debugMonitor(io);
    debugMonitor.start();

    io.run();

    return EXIT_SUCCESS;
}
catch (const std::exception& e)
{
    error("mctpreactor terminated with unhandled exception: {WHAT}", "WHAT",
          e.what());
    return EXIT_FAILURE;
}
catch (...)
{
    error("mctpreactor terminated with unknown unhandled exception");
    return EXIT_FAILURE;
}
