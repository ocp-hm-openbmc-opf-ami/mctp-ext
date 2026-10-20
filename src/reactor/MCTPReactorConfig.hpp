#pragma once

#include "config.h"

#include <sdbusplus/asio/connection.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct I2CDeviceConfig
{
    std::string name;
    std::string bus;
    std::string address;
    std::string staticEndpointId;
    std::string pollingInterval;
    std::string ignoreMessageTypes;
};

struct I2CDiscoveryConfig
{
    bool enabled = MCTP_I2C_ENABLED;
    bool arpEnabled = MCTP_I2C_ARP_ENABLED;
    std::uint16_t i2cNet = DEFAULT_I2C_NET;
    std::uint8_t ownEID = DEFAULT_LOCAL_EID;
    std::uint32_t mtu = DEFAULT_MTU; // MCTP minimum baseline MTU
    std::set<std::string> arpIgnoreList = {DEFAULT_I2C_ARP_IGNORELIST};
    std::chrono::seconds pollingInterval{DEFAULT_I2C_POLL_INTERVAL};
    std::vector<I2CDeviceConfig> devices = DEFAULT_I2C_DEVICES;
};

/**
 * @brief Configuration for a single I3C device
 * Populated from [[i3c.devices]] sections in mctpd.conf
 */
struct I3CDeviceConfig
{
    std::string name;             // Device name
    std::uint8_t busNum = 0;      // I3C bus number
    std::string pidMask;          // PID regex (matches sysfs `pid` string)
    std::string devicePid;        // Device PID hex string
    std::string physicalLinkName; // Optional override of mctpi3cN link name
    std::string role;             // Device role (e.g., "bus-owner")
    // BMC I3C physical mode; false means the BMC is the controller.
    // This must not be inferred from the MCTP role.
    bool isTarget = false;
    bool isSecondaryBusOwner = false; // Is secondary bus owner
    std::uint8_t ownEID = 0;         // Configured local EID for this device
    std::vector<std::uint8_t> eidPool; // EID pool {start, end} from MctpBusOwnerConfig / EIDPool
    std::string staticEndpointId;    // Static EID to assign
    // --- shared-EID / endpoint pool request (mirrors oks MctpMultiValConfig)
    bool useSharedEID = false;       // Endpoint role: wait for / copy a
                                     // network-shared EID instead of
                                     // pre-installing a local EID.
    std::uint8_t requestEIDPool = 0; // Endpoint role: pool size to request
                                     // from upstream bus owner via mctpd
                                     // Interface1.RequestPoolSize. 0 = none.
    std::uint16_t netOverride = 0;   // Optional per-device MCTP network ID
                                     // override (0 = use I3C transport net).
    // When true, skip AssignEndpoint; mctpd assigns EID after pool-ID from BO.
    bool waitForPoolId = false;
};

/**
 * @brief Configuration for a single I3C physical interface / link.
 * Populated from each MCTPI3CConfiguration entry so that multiple physical
 * I3C links (e.g. mctpi3c4 and mctpi3c5), each with their own PhysicalLinkName
 * and MCTP network, can coexist. MCTPI3CTarget devices are mapped onto the
 * matching interface by bus number (parsed from PhysicalLinkName).
 */
struct I3CInterfaceConfig
{
    std::string name;                 // MCTPI3CConfiguration.Name
    std::string physicalLinkName;     // e.g. mctpi3c4 / mctpi3c5
    int busNumber = -1;               // Bus parsed from physicalLinkName
    std::uint16_t i3cNet = DEFAULT_I3C_NET;
    std::uint32_t mtu = DEFAULT_MTU;  // MCTP minimum baseline MTU
    std::uint8_t ownEID = 0;          // Optional link-wide OwnEID default for
                                       // devices that don't set their own
    bool enabled = true;
};

struct I3CDiscoveryConfig
{
    bool enabled = MCTP_I3C_ENABLED;
    std::uint16_t i3cNet = DEFAULT_I3C_NET;
    std::uint32_t mtu = DEFAULT_MTU; // MCTP minimum baseline MTU
    bool waitForEid = MCTP_WAIT_FOR_EID_ENABLED;
    std::vector<I3CDeviceConfig> devices = DEFAULT_I3C_DEVICES;
    // One entry per MCTPI3CConfiguration (physical link). Empty for
    // single-link legacy configs that do not declare a PhysicalLinkName.
    std::vector<I3CInterfaceConfig> interfaces;
    std::chrono::seconds pollingInterval{DEFAULT_I3C_POLL_INTERVAL};
    int maxRetries = 0;
};

struct PCIeInterfaceConfig
{
    std::string name;               // PCIe interface name (from MCTPPCIeConfiguration.Name)
    std::string physicalLinkName;   // Optional override of mctppciN link name
    std::string role = MCTP_PCIE_ROLE; // "bus-owner" or "endpoint"
    std::uint16_t pcieNet = DEFAULT_PCIE_NET; // MCTP network number
    std::uint8_t ownEID = DEFAULT_LOCAL_EID;  // Local EID
    std::uint32_t mtu = DEFAULT_MTU;          // MTU in bytes
    bool enabled = true;            // false = interface disabled, skip MCTP setup
    uint8_t hostIndex = 0;          // Host index (0=Host0, 1=Host1, 2=Host2) for multi-host mode
};

struct PCIeDiscoveryConfig
{
    std::vector<PCIeInterfaceConfig> interfaces;
    std::chrono::seconds pollingInterval{DEFAULT_PCIE_POLL_INTERVAL};
};

struct USBTargetConfig
{
    std::string name;
    std::string interface;
    std::string usbPath;
    std::string ownEid;
    std::string staticEndpointId;
    std::string bridgePoolStartEid;
    std::string bridgePoolEndEid;
    std::string pollingInterval;
    std::string ignoreEids;
};

struct USBDiscoveryConfig
{
    bool enabled = MCTP_USB_ENABLED;
    bool hotplugEnabled = MCTP_USB_HOTPLUG_ENABLED;
    std::uint16_t usbNet = DEFAULT_USB_NET;                       // Network number for USB MCTP devices
    std::uint8_t ownEID = DEFAULT_LOCAL_EID;                      // Default local EID for USB interfaces
    std::uint32_t mtu = DEFAULT_MTU;                              // MCTP minimum baseline MTU
    std::chrono::seconds pollingInterval{DEFAULT_USB_POLL_INTERVAL}; // Discovery poll interval
    std::vector<USBTargetConfig> targets;
};

struct RoutingTableConfig
{
    std::chrono::seconds pollingInterval{DEFAULT_ROUTING_TABLE_POLL_INTERVAL};
};

// The config structs above are now fully defined, so it is safe to pull in the
// discovery headers (each of which includes this file back for those structs).
#include "MCTPI2CDiscovery.hpp"
#include "MCTPI3CDiscovery.hpp"
#include "MCTPPCIeDiscovery.hpp"
#include "MCTPRoutingTableDiscovery.hpp"
#include "MCTPUSBDiscovery.hpp"
#include "PeriodicTask.hpp"

class MCTPI2CDiscovery;
class MCTPI3CDiscovery;
class MCTPPCIeDiscovery;
class MCTPUSBDiscovery;
class MCTPRoutingTableDiscovery;

struct MCTPReactorConfig
{
    static constexpr std::chrono::seconds reactorTickPeriod{5};
    I2CDiscoveryConfig i2c;
    I3CDiscoveryConfig i3c;
    PCIeDiscoveryConfig pcie;
    USBDiscoveryConfig usb;
    RoutingTableConfig routingTable;

    /**
     * @brief Load configuration from Entity Manager JSON file
     * @param jsonPath Path to mctp-ext.json file
     */
    static MCTPReactorConfig fromJsonFile(
        const std::string& jsonPath = MCTPD_JSON_FILE_DEFAULT);

    /**
     * @brief Load configuration from Entity Manager D-Bus service
     *        using the legacy MCTPI2C/I3C/PCIe/USB Configuration & Target
     *        interfaces (GetManagedObjects under EntityManager).
     * @param connection D-Bus connection
     */
    static std::pair<MCTPReactorConfig, std::size_t> fromEntityManager(
        const std::shared_ptr<sdbusplus::asio::connection>& connection);

    /**
     * @brief Load configuration from Entity Manager D-Bus using the
     *        per-device xyz.openbmc_project.Configuration.MctpConfig schema
     *        via mctp::config::Reader. Returns the populated config and the
     *        number of MCTP device entries successfully loaded (0 if none).
     */
    static std::pair<MCTPReactorConfig, std::size_t> fromEntityManagerViaReader(
        const std::shared_ptr<sdbusplus::asio::connection>& connection);
};

/**
 * @brief Holds the discovery module instances and their periodic tasks
 *        that MCTPReactor::buildDiscovery() / resetDiscovery() build and
 *        tear down from an MCTPReactorConfig.
 */
struct MCTPDiscoveryState
{
    std::shared_ptr<MCTPI2CDiscovery> i2cDiscovery;
    std::shared_ptr<MCTPPCIeDiscovery> pcieDiscovery;
    std::shared_ptr<MCTPUSBDiscovery> usbDiscovery;
    std::shared_ptr<MCTPI3CDiscovery> i3cDiscovery;
    std::shared_ptr<MCTPRoutingTableDiscovery> routingTableDiscovery;

    std::optional<PeriodicTask> reactorTick;
};
