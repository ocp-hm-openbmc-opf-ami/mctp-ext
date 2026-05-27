#include "config.h"

#include "DBusAssociationServer.hpp"
#include "HostStateMonitor.hpp"
#include "MCTPConstants.hpp"
#include "MCTPI2CDiscovery.hpp"
#include "MCTPI3CDiscovery.hpp"
#include "MCTPUSBDiscovery.hpp"
#include "MCTPEndpoint.hpp"
#include "MCTPPCIeDiscovery.hpp"
#include "MCTPReactor.hpp"
#include "MCTPReactorConfig.hpp"
#include "MCTPRoutingTableDiscovery.hpp"
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

static void manageMCTPEntity(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const std::shared_ptr<MCTPReactor>& reactor, ManagedObjectType& entities)
{
    for (const auto& [path, config] : entities)
    {
        try
        {
            reactor->manageMCTPDevice(path,
                                      deviceFromConfig(connection, config));
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
}

static void exitReactor(boost::asio::io_context* io, sdbusplus::message_t& msg)
{
    auto name = msg.unpack<std::string>();
    info("Shutting down mctpreactor, lost dependency '{SERVICE_NAME}'",
         "SERVICE_NAME", name);
    io->stop();
}

int main(int /*argc*/, char* /*argv*/[])
{
    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);

    // Load config: try Entity Manager D-Bus first, then JSON file, then TOML
    MCTPReactorConfig config;
    if (std::filesystem::exists(MCTPD_JSON_FILE_DEFAULT))
    {
        config = MCTPReactorConfig::fromJsonFile();
    }
    else
    {
        config = MCTPReactorConfig::fromTomlFile();
    }

    DBusAssociationServer associationServer(systemBus);
    auto reactor = std::make_shared<MCTPReactor>(associationServer);

    // Create discovery modules with config
    std::shared_ptr<MCTPI2CDiscovery> i2cDiscovery;
    if (config.i2c.enabled)
    {
        config.i2c.localEid = config.localEid;
        i2cDiscovery =
            std::make_shared<MCTPI2CDiscovery>(systemBus, config.i2c);
#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
        i2cDiscovery->setReactor(reactor);
#endif
    }
    std::shared_ptr<MCTPPCIeDiscovery> pcieDiscovery;
    if (config.pcie.enabled)
    {
        config.pcie.localEid = config.localEid;
        pcieDiscovery =
            std::make_shared<MCTPPCIeDiscovery>(systemBus, config.pcie);
    }
    std::shared_ptr<MCTPUSBDiscovery> usbDiscovery;
    if (config.usb.enabled)
    {
        config.usb.localEid = config.localEid;
        usbDiscovery =
            std::make_shared<MCTPUSBDiscovery>(systemBus, config.usb);
#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
        usbDiscovery->setReactor(reactor);
#endif
    }
    std::shared_ptr<MCTPI3CDiscovery> i3cDiscovery;
    if (config.i3c.enabled)
    {
        i3cDiscovery =
            std::make_shared<MCTPI3CDiscovery>(systemBus, config.i3c);
    }
    auto routingTableDiscovery =
        std::make_shared<MCTPRoutingTableDiscovery>(systemBus);

    // Periodic tasks
    PeriodicTask reactorTick(io, config.reactorTickPeriod,
                             [reactor]() { reactor->tick(); });
    
    // Create discovery tasks only if enabled in config
    std::vector<PeriodicTask*> discoveryTasks;
    
    std::optional<PeriodicTask> i2cHotplugTask;
    if (config.i2c.enabled)
    {
        i2cHotplugTask.emplace(io, config.i2c.pollingInterval,
                               [i2cDiscovery]() { i2cDiscovery->run(); });
        discoveryTasks.push_back(&i2cHotplugTask.value());
    }

    std::optional<PeriodicTask> pcieTask;
    if (config.pcie.enabled && config.pcie.pcieRole == "bus-owner")
    {
        pcieTask.emplace(io, config.pcie.pollingInterval,
                         [pcieDiscovery]() { pcieDiscovery->run(); });
        discoveryTasks.push_back(&pcieTask.value());
    }

    std::optional<PeriodicTask> usbTask;
    if (config.usb.enabled)
    {
        usbTask.emplace(io, config.usb.pollingInterval,
                        [usbDiscovery]() { usbDiscovery->run(); });
        discoveryTasks.push_back(&usbTask.value());
    }
    
    // USB hotplug event handling - run frequently to process device hotplug events
    std::optional<PeriodicTask> usbHotplugTask;
    if (config.usb.enabled && config.usb.hotplugEnabled)
    {
        usbHotplugTask.emplace(io, std::chrono::milliseconds(100),
                               [usbDiscovery]() { usbDiscovery->handleLibusbEvents(); });
    }
    
    std::optional<PeriodicTask> routingTableTask;
    // Routing table discovery is always created (no enabled flag)
    routingTableTask.emplace(io, config.routingTable.pollingInterval,
                             [routingTableDiscovery]() { routingTableDiscovery->run(); });
    discoveryTasks.push_back(&routingTableTask.value());

    // Each discovery module auto-registered itself with MCTPDiscovery on
    // construction. Host-state events fan out to every registered instance
    // through the static dispatchers; subclasses handle the events via
    // their onHostOn/onHostOff/onPlatformReset overrides.
    HostStateMonitor hostMonitor(
        systemBus,
        discoveryTasks,
        []() { MCTPDiscovery::dispatchHostOff(); },
        []() { MCTPDiscovery::dispatchHostOn(); },
        []() {
            info("Handling platform reset: dispatching to discovery modules");
            MCTPDiscovery::dispatchPlatformReset();
        }
    );

    // Setup signal handler for graceful shutdown on SIGTERM
    boost::asio::signal_set signals(io, SIGTERM, SIGINT);
    signals.async_wait([&io, &config
                        , &usbDiscovery
                       ](const boost::system::error_code& ec,
                                             int signum) {
        if (!ec)
        {
            info("Received signal {SIGNAL}, shutting down gracefully", "SIGNAL",
                 signum);
            if (config.usb.enabled && config.usb.hotplugEnabled)
            {
                // Close USB handles and shutdown hotplug
                info("Closing USB device handles");
                usbDiscovery->shutdownHotplug();
            }
            
            // Stop the io_context to exit the event loop
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
        // Trailing slash on path: Listen for signals on the inventory subtree
        rules::interfacesRemovedAtPath(mctp::dbus::inventoryBasePath.data());

    auto interfacesRemovedMatch = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*systemBus), interfacesRemovedMatchSpec,
        std::bind_front(removeInventory, reactor));

    const std::string interfacesAddedMatchSpec =
        rules::sender(mctp::dbus::entityManagerService.data()) +
        // Trailing slash on path: Listen for signals on the inventory subtree
        rules::interfacesAddedAtPath(mctp::dbus::inventoryBasePath.data());

    auto interfacesAddedMatch = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*systemBus), interfacesAddedMatchSpec,
        std::bind_front(addInventory, systemBus, reactor));

    //systemBus->request_name(mctp::dbus::reactorService.data());

    //boost::asio::post(io, [reactor, systemBus]() {
    //    auto gsc = std::make_shared<GetSensorConfiguration>(
    //        systemBus, std::bind_front(manageMCTPEntity, systemBus, reactor));
    //    std::vector<std::string_view> types{"MCTPI2CTarget", "MCTPI3CTarget"};
    //    gsc->getConfiguration(types);
    //});

    // Monitor /var/run/mctp_trace_on for runtime log level changes
    ReactorDebugMonitor debugMonitor(io);
    debugMonitor.start();

    io.run();

    return EXIT_SUCCESS;
}
