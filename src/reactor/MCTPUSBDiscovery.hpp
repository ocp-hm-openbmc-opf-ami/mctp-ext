#pragma once

#include "config.h"

#include "MCTPConstants.hpp"
#include "MCTPDiscovery.hpp"
#include "MCTPReactorConfig.hpp"

#include <libusb-1.0/libusb.h>

#include <sdbusplus/asio/connection.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>

class MCTPReactor;

/**
 * @brief USB device discovery for MCTP endpoints
 *
 * Enumerates USB MCTP devices by:
 * 1. Querying AssignEndpoint D-Bus method on USB interfaces
 * 2. Retrieving pool start/size information from bridge endpoints
 * 3. Bringing up bridge devices and populating EntityManager
 *
 * Follows patterns from nvidia/nvl32.cpp device enumeration
 */
class MCTPUSBDiscovery : public MCTPDiscovery
{
  public:
    MCTPUSBDiscovery(const std::shared_ptr<sdbusplus::asio::connection>& bus,
                     const USBDiscoveryConfig& config);
    ~MCTPUSBDiscovery() override;

    void run() override;
    std::string_view name() const override
    {
        return "USB";
    }

    /// Reset USB enumeration state on host power-on.
    void onHostOn() override
    {
        resetDiscoveryState();
    }

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
    void setReactor(const std::shared_ptr<MCTPReactor>& r)
    {
        reactor = r;
    }
#endif

    /**
     * @brief Process pending libusb hotplug events
     */
    int handleLibusbEvents();

    /**
     * @brief Shutdown libusb hotplug monitoring and close device handles
     */
    void shutdownHotplug();

    /**
     * @brief Reset USB discovery state when host state changes
     * Also closes any open device handles
     */
    void resetDiscoveryState()
    {
        // Close open device handles
        for (auto& [path, handle] : deviceHandles)
        {
            if (handle)
            {
                libusb_close(handle);
            }
        }
        deviceHandles.clear();
        deviceEids.clear();
        assignedEndpoints.clear();
    }

  private:
    const USBDiscoveryConfig& config;

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
    std::shared_ptr<MCTPReactor> reactor;
    void manageDeviceViaReactor(const std::string& interfaceName,
                                const std::string& trackingKey);
#endif

    struct SetupEndpointResponse
    {
        std::uint8_t eid;
        std::int32_t networkId;
        std::string interface;
        bool probed;
    };

    /**
     * @brief Call SetupEndpoint D-Bus method on a named MCTP interface
     * @param interfaceName Interface name (e.g., "mctpusb0")
     * @return SetupEndpointResponse with assigned EID and network info
     */
    SetupEndpointResponse setupEndpoint(const std::string& interfaceName);

    /**
     * @brief Enumerate configured USB targets and call SetupEndpoint for each
     */
    void enumerateTargets();

    /**
     * @brief Enumerate all mctpusb netlink interfaces and assign endpoints
     * Scans /sys/class/net for all interfaces matching "mctpusb*" pattern
     * and calls assignEndpoint D-Bus method for each
     */
    void enumerateMCTPNetlinkInterfaces();

    // Hotplug support methods
    /**
     * @brief Initialize libusb hotplug monitoring
     */
    int initializeHotplug();

    /**
     * @brief Check if device matches configured bridge device
     */
    bool isMatchingUSBDevice(libusb_device* device);

    /**
     * @brief Check if device has MCTP USB interface
     */
    bool isMCTPUSBInterface(libusb_device* device);

    /**
     * @brief Handle device arrival event
     */
    int handleDeviceArrived(libusb_device* device);

    /**
     * @brief Handle device departure event
     */
    int handleDeviceDeparted(libusb_device* device);

    /**
     * @brief Construct bus_id-port_path string from libusb device
     * Format: "bus_id-port1.port2.port3"
     * @param device USB device
     * @return bus_id-port_path string, empty string if parsing fails
     */
    std::string constructBusPortPath(libusb_device* device);
    /**
     * @brief Search sysfs for MCTP netlink name by matching port path
     * Searches /sys/bus/usb/devices/ for a device matching the port path
     * and finds the MCTP netlink interface in its net directory
     * @param busPortPath Bus ID and port path (e.g., "1-2-3")
     * @return MCTP netlink name (e.g., "mctpusb0"), empty string if not found
     */
    std::string searchMCTPNetlinkByPortPath(const std::string& busPortPath);

    /**
     * @brief Static hotplug callback dispatcher
     */
    static int hotplugCallback(libusb_context* ctx, libusb_device* device,
                               libusb_hotplug_event event, void* userData);

    // Hotplug state
    libusb_context* usbContext = nullptr;
    libusb_hotplug_callback_handle hotplugHandle = 0;
    bool hotplugInitialized = false;
    std::map<std::string, libusb_device_handle*> deviceHandles;
    std::map<std::string, uint8_t> deviceEids; // Map busPortPath -> EID
    std::set<std::string>
        assignedEndpoints; // Track successfully assigned endpoints
};
