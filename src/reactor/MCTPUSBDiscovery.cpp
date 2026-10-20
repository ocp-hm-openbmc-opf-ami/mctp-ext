#include "MCTPUSBDiscovery.hpp"

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
#include "MCTPEndpoint.hpp"
#include "MCTPReactor.hpp"
#endif

#include <phosphor-logging/lg2.hpp>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

PHOSPHOR_LOG2_USING;

// USB Class code for MCTP (not defined in standard libusb headers)
static constexpr uint8_t LIBUSB_CLASS_MCTP = 0x14;

MCTPUSBDiscovery::MCTPUSBDiscovery(
    const std::shared_ptr<sdbusplus::asio::connection>& bus,
    const USBDiscoveryConfig& config) : MCTPDiscovery(bus), config(config)
{
    debug("MCTPUSBDiscovery initialized: pollingInterval={POLL}", "POLL",
          config.pollingInterval.count());

    startTask(config.pollingInterval);

    // Initialize libusb hotplug support
    if (config.hotplugEnabled)
    {
        initializeHotplug();
        startHotplugPollTask();
    }
}

MCTPUSBDiscovery::~MCTPUSBDiscovery()
{
    if (config.hotplugEnabled)
        shutdownHotplug();
}

void MCTPUSBDiscovery::startTask(std::chrono::seconds interval)
{
    task.emplace(bus->get_io_context(), interval, [this]() { run(); });
}

void MCTPUSBDiscovery::startHotplugPollTask()
{
    hotplugPollTask.emplace(bus->get_io_context(),
                            std::chrono::milliseconds(100),
                            [this]() { handleLibusbEvents(); });
}

void MCTPUSBDiscovery::run()
{
    debug("Starting USB device enumeration");

    try
    {
        if (!config.targets.empty())
        {
            enumerateTargets();
        }
        else
        {
            enumerateMCTPNetlinkInterfaces();
        }
    }
    catch (const std::exception& e)
    {
        warning("USB discovery failed: {ERROR}", "ERROR", e.what());
    }
}

namespace fs = std::filesystem;

MCTPUSBDiscovery::SetupEndpointResponse MCTPUSBDiscovery::setupEndpoint(
    const std::string& interfaceName)
{
    std::string objPath = std::format(
        "{}{}", std::string(mctp::dbus::interfacesPath), interfaceName);

    debug("Calling SetupEndpoint on {PATH}", "PATH", objPath);

    std::vector<uint8_t> address = {};
    auto m = bus->new_method_call(
        std::string(mctp::dbus::service).c_str(), objPath.c_str(),
        std::string(mctp::dbus::busOwnerInterface).c_str(), "SetupEndpoint");
    m.append(address);

    auto reply = bus->call(m);

    SetupEndpointResponse response{};
    reply.read(response.eid, response.networkId, response.interface,
               response.probed);

    debug("SetupEndpoint returned: EID={EID}, net={NET}, intf={INTF}", "EID",
          response.eid, "NET", response.networkId, "INTF", response.interface);

    return response;
}

void MCTPUSBDiscovery::enumerateTargets()
{
    debug("Enumerating {COUNT} configured USB targets", "COUNT",
          config.targets.size());

    // Query available interfaces once before the loop
    auto links = getLinksViaNetlink("mctpusb");
    std::set<std::string> availableLinks(links.begin(), links.end());

    for (const auto& target : config.targets)
    {
        if (target.interface.empty())
        {
            warning("USB target {NAME} has no interface configured, skipping",
                    "NAME", target.name);
            continue;
        }

        debug("Setting up endpoint for target {NAME} on interface {INTF}",
              "NAME", target.name, "INTF", target.interface);

        // Check if target interface exists
        if (!availableLinks.contains(target.interface))
        {
            warning(
                "USB target {NAME}: interface {INTF} does not exist, skipping",
                "NAME", target.name, "INTF", target.interface);
            continue;
        }

        // Ensure interface is up and local EID is configured
        {
            uint8_t eid = target.ownEid.empty()
                ? config.ownEID
                : static_cast<uint8_t>(
                      std::stoul(target.ownEid, nullptr, 0));
            if (!ensureInterfaceReady(target.interface, eid, config.usbNet,
                                      config.mtu))
            {
                warning(
                    "USB target {NAME}: interface {INTF} not ready, skipping",
                    "NAME", target.name, "INTF", target.interface);
                continue;
            }
        }

        try
        {
#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
            manageDeviceViaReactor(target.interface, target.interface);
#else
            auto response = setupEndpoint(target.interface);

            if (response.eid > 0)
            {
                debug("Target {NAME} assigned EID={EID}, net={NET}", "NAME",
                      target.name, "EID", static_cast<int>(response.eid), "NET",
                      response.networkId);

                deviceEids[target.interface] = response.eid;
                assignedEndpoints.insert(target.interface);
            }
            else
            {
                warning("SetupEndpoint failed for target {NAME}", "NAME",
                        target.name);
            }
#endif
        }
        catch (const std::exception& e)
        {
            warning("Error setting up target {NAME}: {ERROR}", "NAME",
                    target.name, "ERROR", e.what());
        }
    }
}

void MCTPUSBDiscovery::enumerateMCTPNetlinkInterfaces()
{
    debug("Enumerating MCTP netlink interfaces using direct netlink API");

    auto interfaces = getLinksViaNetlink("mctpusb");
    if (interfaces.empty())
    {
        debug("No MCTP USB netlink interfaces found");
        return;
    }

    for (const auto& ifname : interfaces)
    {
        debug("Found MCTP netlink interface: {NAME}", "NAME", ifname);

        // Ensure interface is up and local EID is configured
        if (!ensureInterfaceReady(ifname, config.ownEID, config.usbNet,
                                  config.mtu))
        {
            warning("Interface {NAME} not ready, skipping", "NAME", ifname);
            continue;
        }

        try
        {
#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
            manageDeviceViaReactor(ifname, ifname);
#else
            auto response = setupEndpoint(ifname);

            if (response.eid > 0)
            {
                debug(
                    "SetupEndpoint successful for interface {NAME}, EID={EID}",
                    "NAME", ifname, "EID", static_cast<int>(response.eid));

                // Store EID for later use when device departs
                deviceEids[ifname] = response.eid;
                assignedEndpoints.insert(ifname);
                debug("Stored EID={EID} for interface {NAME}", "EID",
                      static_cast<int>(response.eid), "NAME", ifname);
            }
            else
            {
                warning("SetupEndpoint failed for interface {NAME}", "NAME",
                        ifname);
            }
#endif
        }
        catch (const std::exception& e)
        {
            warning("Error assigning endpoint for interface {NAME}: {ERROR}",
                    "NAME", ifname, "ERROR", e.what());
        }
    }
}

int MCTPUSBDiscovery::initializeHotplug()
{
    if (hotplugInitialized)
    {
        debug("Hotplug already initialized");
        return LIBUSB_SUCCESS;
    }

    int rc = libusb_init(&usbContext);
    if (rc != LIBUSB_SUCCESS)
    {
        error("Failed to initialize libusb: {ERROR}", "ERROR",
              libusb_strerror(rc));
        return rc;
    }

    auto events = static_cast<libusb_hotplug_event>(
        LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT);

    rc = libusb_hotplug_register_callback(
        usbContext, events, LIBUSB_HOTPLUG_ENUMERATE, LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, hotplugCallback,
        this, &hotplugHandle);

    if (rc != LIBUSB_SUCCESS)
    {
        error("Failed to register hotplug callback: {ERROR}", "ERROR",
              libusb_strerror(rc));
        libusb_exit(usbContext);
        usbContext = nullptr;
        return rc;
    }

    hotplugInitialized = true;
    debug("USB hotplug monitoring initialized");
    return LIBUSB_SUCCESS;
}

void MCTPUSBDiscovery::shutdownHotplug()
{
    if (!hotplugInitialized)
    {
        return;
    }

    // Deregister hotplug callback
    if (usbContext && hotplugHandle)
    {
        libusb_hotplug_deregister_callback(usbContext, hotplugHandle);
    }

    // Close all tracked device handles
    for (auto& [path, handle] : deviceHandles)
    {
        if (handle)
        {
            libusb_close(handle);
            debug("Closed USB device at {PATH}", "PATH", path);
        }
    }
    deviceHandles.clear();

    // Exit libusb context
    if (usbContext)
    {
        libusb_exit(usbContext);
        usbContext = nullptr;
    }

    hotplugInitialized = false;
    debug("USB hotplug monitoring shut down");
}

int MCTPUSBDiscovery::handleLibusbEvents()
{
    if (!hotplugInitialized || !usbContext)
    {
        return 0;
    }

    struct timeval tv = {0, 0};
    int rc = libusb_handle_events_timeout(usbContext, &tv);

    if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NO_DEVICE)
    {
        debug("libusb_handle_events failed: {ERROR}", "ERROR",
              libusb_strerror(rc));
    }

    return rc == LIBUSB_SUCCESS ? 1 : 0;
}

bool MCTPUSBDiscovery::isMCTPUSBInterface(libusb_device* device)
{
    libusb_config_descriptor* config = nullptr;
    int ret = libusb_get_active_config_descriptor(device, &config);

    if (ret != LIBUSB_SUCCESS)
    {
        debug("Failed to get config: {ERROR}", "ERROR", libusb_error_name(ret));
        return false;
    }

    bool found = false;

    for (int i = 0; i < config->bNumInterfaces; ++i)
    {
        const libusb_interface* interfaces = config->interface + i;
        for (int j = 0; j < interfaces->num_altsetting; ++j)
        {
            const libusb_interface_descriptor* interface =
                interfaces->altsetting + j;
            if (interface->bInterfaceClass == LIBUSB_CLASS_MCTP)
            {
                found = true;
                goto out;
            }
        }
    }

out:
    libusb_free_config_descriptor(config);
    return found;
}

bool MCTPUSBDiscovery::isMatchingUSBDevice(libusb_device* device)
{
    // Just verify it has MCTP interface
    return isMCTPUSBInterface(device);
}

int MCTPUSBDiscovery::handleDeviceArrived(libusb_device* device)
{
    debug("USB device arrived");

    // Construct bus_id-port_path string
    std::string busPortPath = constructBusPortPath(device);
    if (busPortPath.empty())
    {
        warning("Failed to construct bus-port path for arriving device");
        return -1;
    }

    debug("Device bus-port path: {PATH}", "PATH", busPortPath);

    // Search sysfs for MCTP netlink name by matching port path
    std::string netlinkName = searchMCTPNetlinkByPortPath(busPortPath);
    if (!netlinkName.empty())
    {
        debug("Found MCTP netlink interface: {NAME} for device {PORT}", "NAME",
              netlinkName, "PORT", busPortPath);
    }
    else
    {
        warning(
            "Could not find MCTP netlink interface in sysfs for port path: {PORT}",
            "PORT", busPortPath);
        return LIBUSB_SUCCESS;
    }

    // Try to find matching USB target config
    std::optional<uint8_t> staticEndpointId;
    std::string ownEid;
    for (const auto& target : config.targets)
    {
        if (target.usbPath == busPortPath)
        {
            if (!target.staticEndpointId.empty())
            {
                staticEndpointId = static_cast<uint8_t>(
                    std::stoul(target.staticEndpointId, nullptr, 0));
            }
            ownEid = target.ownEid;
            break;
        }
    }

    // Ensure interface is up and local EID is configured
    {
        uint8_t eid = ownEid.empty()
            ? config.ownEID
            : static_cast<uint8_t>(std::stoul(ownEid, nullptr, 0));
        if (!ensureInterfaceReady(netlinkName, eid, config.usbNet, config.mtu))
        {
            warning("Hotplug: interface {INTF} not ready, skipping", "INTF",
                    netlinkName);
            return LIBUSB_SUCCESS;
        }
    }

    try
    {
        debug("USB device arrived at {PATH}", "PATH", busPortPath);

        // Open device handle
        libusb_device_handle* handle = nullptr;
        int rc = libusb_open(device, &handle);

        if (rc != LIBUSB_SUCCESS)
        {
            error("Failed to open USB device: {ERROR}", "ERROR",
                  libusb_strerror(rc));
            return rc;
        }

        // Track the opened handle
        deviceHandles[busPortPath] = handle;
        debug("Opened USB device at {PATH}, handle tracking enabled", "PATH",
              busPortPath);

        // Use netlink name for device identifier
        std::string assignIdentifier = netlinkName;

        // Use SetupEndpoint via D-Bus to discover the device
        {
#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
            manageDeviceViaReactor(assignIdentifier, busPortPath);
#else
            auto response = setupEndpoint(assignIdentifier);

            debug(
                "SetupEndpoint successful for EID={EID}, net={NET}, device={PATH}",
                "EID", static_cast<int>(response.eid), "NET",
                response.networkId, "PATH", busPortPath);

            // Store EID for later use when device departs
            deviceEids[busPortPath] = response.eid;
            assignedEndpoints.insert(busPortPath);
            debug("Stored EID={EID} for device {PATH}", "EID",
                  static_cast<int>(response.eid), "PATH", busPortPath);
#endif
        }
    }
    catch (const std::exception& e)
    {
        warning("Failed to handle device arrival: {ERROR}", "ERROR", e.what());
        return -1;
    }

    return LIBUSB_SUCCESS;
}

int MCTPUSBDiscovery::handleDeviceDeparted(libusb_device* device)
{
    debug("USB device departed");

    // Construct bus_id-port_path string
    std::string busPortPath = constructBusPortPath(device);
    if (busPortPath.empty())
    {
        warning("Failed to construct bus-port path for departing device");
        return -1;
    }

    debug("Device bus-port path: {PATH}", "PATH", busPortPath);

    try
    {
        debug("USB device departed: {PATH}", "PATH", busPortPath);

        // Close and remove device handle
        auto it = deviceHandles.find(busPortPath);
        if (it != deviceHandles.end())
        {
            if (it->second)
            {
                libusb_close(it->second);
                debug("Closed USB device at {PATH}", "PATH", busPortPath);
            }
            deviceHandles.erase(it);
        }

        // Retrieve stored EID for this device
        auto eidIt = deviceEids.find(busPortPath);
        if (eidIt != deviceEids.end())
        {
            uint8_t eid = eidIt->second;

            // Call removeEndpoint to handle D-Bus removal
            removeEndpoint(config.usbNet, eid);

            // Clean up stored EID
            deviceEids.erase(eidIt);
        }
        else
        {
            warning("No stored EID found for departing device: {PATH}", "PATH",
                    busPortPath);
        }
    }
    catch (const std::exception& e)
    {
        warning("Error handling device departure: {ERROR}", "ERROR", e.what());
    }

    return LIBUSB_SUCCESS;
}

int MCTPUSBDiscovery::hotplugCallback(
    libusb_context* ctx, libusb_device* device, libusb_hotplug_event event,
    void* userData)
{
    (void)ctx;

    if (!userData)
    {
        return LIBUSB_SUCCESS;
    }

    MCTPUSBDiscovery* discovery = static_cast<MCTPUSBDiscovery*>(userData);

    if (!discovery->isMatchingUSBDevice(device))
    {
        return LIBUSB_SUCCESS;
    }

    debug("Hotplug event {EVENT} for matching device", "EVENT",
          (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED ? "ARRIVED"
                                                        : "DEPARTED"));

    switch (event)
    {
        case LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED:
            return discovery->handleDeviceArrived(device);
        case LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT:
            return discovery->handleDeviceDeparted(device);
        default:
            return LIBUSB_SUCCESS;
    }
}

std::string MCTPUSBDiscovery::constructBusPortPath(libusb_device* device)
{
    uint8_t busId = libusb_get_bus_number(device);

    // Get port numbers
    uint8_t ports[7] = {0};
    int portCount = libusb_get_port_numbers(device, ports, sizeof(ports));

    if (portCount <= 0)
    {
        warning("Failed to get port numbers from USB device");
        return "";
    }

    // Construct port path string using dashes (e.g., "1-2-3")
    std::string portPath;
    for (int i = 0; i < portCount; ++i)
    {
        if (i == 0)
        {
            // Add first port number after bus ID
            portPath = std::to_string(ports[i]);
        }
        else
        {
            // Add subsequent port numbers with dashes
            portPath += "-" + std::to_string(ports[i]);
        }
    }

    // Construct final string as "bus_id-port1-port2-port3"
    std::string result = std::to_string(busId) + "-" + portPath;

    debug("Constructed bus-port path: {PATH}", "PATH", result);
    return result;
}

std::string MCTPUSBDiscovery::searchMCTPNetlinkByPortPath(
    const std::string& busPortPath)
{
    // Search /sys/bus/usb/devices/ for device matching busPortPath
    std::string sysDevicesPath = "/sys/bus/usb/devices";
    std::string netDeviceName;

    try
    {
        if (!fs::exists(sysDevicesPath))
        {
            warning("Sysfs USB devices path not found: {PATH}", "PATH",
                    sysDevicesPath);
            return "";
        }

        debug("Searching for MCTP netlink device matching port path: {PATH}",
              "PATH", busPortPath);

        // Iterate through USB devices in sysfs
        for (const auto& entry : fs::directory_iterator(sysDevicesPath))
        {
            const auto& devicePath = entry.path();
            auto deviceName = devicePath.filename().string();

            // Extract substring before ':' and convert '.' to '-'
            size_t colonPos = deviceName.find(':');
            if (colonPos != std::string::npos)
            {
                deviceName = deviceName.substr(0, colonPos);
            }
            std::replace(deviceName.begin(), deviceName.end(), '.', '-');

            // Check if this device path matches our busPortPath
            if (deviceName != busPortPath)
            {
                continue;
            }

            // Found matching USB device, now search for net interface
            fs::path netPath = devicePath / "net";

            if (!fs::exists(netPath))
            {
                debug("No net directory found for device: {NAME}", "NAME",
                      deviceName);
                continue;
            }

            debug("Found net directory for device: {NAME}", "NAME", deviceName);

            // Search for MCTP interface in net directory
            for (const auto& netEntry : fs::directory_iterator(netPath))
            {
                const auto& netName = netEntry.path().filename().string();

                // Look for MCTP interface (typically starts with "mctpusb")
                if (netName.starts_with("mctpusb"))
                {
                    debug(
                        "Found MCTP netlink interface: {NAME} for port path {PORT}",
                        "NAME", netName, "PORT", busPortPath);
                    return netName;
                }
            }

            warning(
                "No MCTP netlink interface found in net directory for device: {NAME}",
                "NAME", deviceName);
        }

        warning("No matching USB device found for port path: {PATH}", "PATH",
                busPortPath);
    }
    catch (const std::exception& e)
    {
        error("Error searching for MCTP netlink device: {ERROR}", "ERROR",
              e.what());
    }

    return "";
}

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
void MCTPUSBDiscovery::manageDeviceViaReactor(const std::string& interfaceName,
                                              const std::string& trackingKey)
{
    if (!reactor)
    {
        warning("Reactor not set, falling back to setupEndpoint for {INTF}",
                "INTF", interfaceName);
        auto response = setupEndpoint(interfaceName);
        if (response.eid > 0)
        {
            deviceEids[trackingKey] = response.eid;
            assignedEndpoints.insert(trackingKey);
        }
        return;
    }

    try
    {
        auto device = std::make_shared<MCTPDDevice>(bus, interfaceName,
                                                    std::vector<uint8_t>{});
        std::string path = std::format(
            "/xyz/openbmc_project/mctp/discovery/usb/{}", interfaceName);
        debug("Managing USB device via reactor: intf={INTF}, path={PATH}",
              "INTF", interfaceName, "PATH", path);
        reactor->manageMCTPDevice(path, device);
    }
    catch (const std::exception& e)
    {
        warning("Failed to manage USB device via reactor: {ERROR}", "ERROR",
                e.what());
    }
}
#endif

