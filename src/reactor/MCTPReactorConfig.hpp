#pragma once

#include "config.h"

#include <sdbusplus/asio/connection.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
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
    std::uint8_t localEid = DEFAULT_LOCAL_EID;
    std::set<std::string> whitelist = {DEFAULT_I2C_WHITELIST};
    std::chrono::seconds pollingInterval{DEFAULT_I2C_POLL_INTERVAL};
    std::vector<I2CDeviceConfig> devices;
};

/**
 * @brief Configuration for a single I3C device
 * Populated from [[i3c.devices]] sections in mctpd.conf
 */
struct I3CDeviceConfig
{
    std::string name;               // Device name
    std::uint8_t busNum = 0;        // I3C bus number
    std::uint32_t pidMask = 0;      // PID mask to match
    std::string devicePid;          // Device PID hex string
    std::string role;               // Device role (e.g., "bus-owner")
    bool isTarget = true;            // Is I3C target device
    bool isSecondaryBusOwner = false; // Is secondary bus owner
    std::string staticEndpointId;    // Static EID to assign
};

struct I3CDiscoveryConfig
{
    bool enabled = MCTP_I3C_ENABLED;    
    std::uint16_t i3cNet = DEFAULT_I3C_NET;
    std::uint8_t busOwnerEid = DEFAULT_I3C_BUSOWNER_EID;
    std::uint8_t endpointEid = DEFAULT_I3C_ENDPOINT_EID;
    std::string platformSoc = DEFAULT_PLATFORM_SOC;
    std::vector<I3CDeviceConfig> devices = DEFAULT_I3C_DEVICES;
    std::chrono::seconds pollingInterval{DEFAULT_I3C_POLL_INTERVAL};
    int maxRetries = 0;
};

struct PCIeDiscoveryConfig
{
    bool enabled = MCTP_PCIE_ENABLED;       
    std::uint16_t pcieNet = DEFAULT_PCIE_NET;
    std::uint8_t localEid = DEFAULT_LOCAL_EID;
    std::string pcieRole = MCTP_PCIE_ROLE;
    std::chrono::seconds pollingInterval{DEFAULT_PCIE_POLL_INTERVAL};
};

struct USBTargetConfig {
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

struct USBDiscoveryConfig {
    bool enabled = MCTP_USB_ENABLED;       
    bool hotplugEnabled = MCTP_USB_HOTPLUG_ENABLED;
    std::uint16_t usbNet = DEFAULT_USB_NET;                       // Network number for USB MCTP devices
    std::uint8_t localEid = DEFAULT_LOCAL_EID;                    // Default local EID for USB interfaces
    std::chrono::seconds pollingInterval{DEFAULT_USB_POLL_INTERVAL}; // Discovery poll interval
    std::vector<USBTargetConfig> targets;
};

struct RoutingTableConfig
{
    std::chrono::seconds pollingInterval{DEFAULT_ROUTING_TABLE_POLL_INTERVAL};
};

struct MCTPReactorConfig
{
    static constexpr std::chrono::seconds reactorTickPeriod{5};
    std::uint8_t localEid = DEFAULT_LOCAL_EID;
    I2CDiscoveryConfig i2c;
    I3CDiscoveryConfig i3c;
    PCIeDiscoveryConfig pcie;
    USBDiscoveryConfig usb;
    RoutingTableConfig routingTable;

    /**
     * @brief Load configuration from TOML file
     */
    static MCTPReactorConfig fromTomlFile();

    /**
     * @brief Load configuration from Entity Manager JSON file
     * @param jsonPath Path to mctp-ext.json file
     */
    static MCTPReactorConfig fromJsonFile(
        const std::string& jsonPath = MCTPD_JSON_FILE_DEFAULT);

    /**
     * @brief Load configuration from Entity Manager D-Bus service
     * @param connection D-Bus connection
     */
    static MCTPReactorConfig fromEntityManager(
        const std::shared_ptr<sdbusplus::asio::connection>& connection);
};
