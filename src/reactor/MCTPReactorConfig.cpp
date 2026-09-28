#include "MCTPReactorConfig.hpp"

#include "MCTPDiscovery.hpp"
#include "MCTPReactor.hpp"
#include "Utils.hpp"
#include "VariantVisitors.hpp"
#include "config/reader.hpp"
#include "config/wrapper_impl.hpp"

#include <json-c/json.h>

#include <boost/asio/spawn.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/message.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

PHOSPHOR_LOG2_USING;

static bool isEnabled(struct json_object* obj, const char* key)
{
    struct json_object* val = nullptr;
    if (json_object_object_get_ex(obj, key, &val))
    {
        const char* s = json_object_get_string(val);
        return s && std::string(s) == "enabled";
    }
    return false;
}

// Parse the trailing decimal digits of a link name (e.g. "mctpi3c4" -> 4,
// "mctppci1" -> 1). Returns -1 when the name has no trailing digits.
static int parseTrailingLinkNumber(const std::string& linkName)
{
    std::size_t pos = linkName.find_last_not_of("0123456789");
    if (pos == std::string::npos || pos + 1 >= linkName.size())
    {
        return -1;
    }
    try
    {
        return std::stoi(linkName.substr(pos + 1));
    }
    catch (const std::exception&)
    {
        return -1;
    }
}

static int parseI3CBusFromLinkName(const std::string& linkName)
{
    return parseTrailingLinkNumber(linkName);
}

#ifdef MULTI_HOST_MODE_SUPPORT
// Derive a PCIe interface's multi-host index from the trailing digit of its
// PhysicalLinkName (e.g. "mctppci1" -> host 1); falls back to the
// alternating Host1/Host2 assignment when the name has no parsable digit.
static std::uint8_t resolvePcieHostIndex(const std::string& linkName,
                                         std::uint8_t& nextPcieHostIndex)
{
    int parsed = parseTrailingLinkNumber(linkName);
    if (parsed > 0)
    {
        return static_cast<std::uint8_t>(parsed);
    }
    std::uint8_t index = nextPcieHostIndex;
    nextPcieHostIndex = static_cast<std::uint8_t>((nextPcieHostIndex % 2) + 1);
    return index;
}
#endif

// Associate each I3C target device with the MCTPI3CConfiguration interface
// serving its bus (matched by the bus number parsed from the interface
// PhysicalLinkName). This lets multiple physical I3C links, each with its own
// PhysicalLinkName and Net, coexist: every device inherits its interface's
// link name and network unless the device explicitly overrides them.
static void associateI3CDevicesWithInterfaces(I3CDiscoveryConfig& i3c)
{
    for (auto& dev : i3c.devices)
    {
        for (const auto& iface : i3c.interfaces)
        {
            if (iface.busNumber < 0 || iface.busNumber != dev.busNum)
            {
                continue;
            }
            if (dev.physicalLinkName.empty())
            {
                dev.physicalLinkName = iface.physicalLinkName;
            }
            if (dev.netOverride == 0)
            {
                dev.netOverride = iface.i3cNet;
            }
            if (dev.ownEID == 0)
            {
                dev.ownEID = iface.ownEID;
            }
            break;
        }
    }
}

static bool loadFromJsonFile(const std::string& jsonPath,
                             MCTPReactorConfig& config)
{
    struct json_object* root = json_object_from_file(jsonPath.c_str());
    if (!root)
    {
        info("JSON config file not found at {PATH}, using defaults", "PATH",
             jsonPath);
        return false;
    }

    if (!json_object_is_type(root, json_type_array) ||
        json_object_array_length(root) == 0)
    {
        warning("Invalid JSON config format in {PATH}", "PATH", jsonPath);
        json_object_put(root);
        return false;
    }

    struct json_object* board = json_object_array_get_idx(root, 0);
    struct json_object* exposes = nullptr;
    if (!json_object_object_get_ex(board, "Exposes", &exposes))
    {
        warning("No Exposes array in JSON config {PATH}", "PATH", jsonPath);
        json_object_put(root);
        return false;
    }

    int len = json_object_array_length(exposes);
#ifdef MULTI_HOST_MODE_SUPPORT
    // Assign each PCIe interface read to Host1, Host2, Host1, Host2...
    std::uint8_t nextPcieHostIndex = 1;
#endif
    for (int i = 0; i < len; i++)
    {
        struct json_object* entry = json_object_array_get_idx(exposes, i);
        struct json_object* typeObj = nullptr;
        if (!json_object_object_get_ex(entry, "Type", &typeObj))
        {
            continue;
        }
        std::string type = json_object_get_string(typeObj);

        if (type == "MCTPGeneralSetting")
        {
            struct json_object* val = nullptr;
            if (json_object_object_get_ex(entry, "RoutingTablePollingInterval",
                                          &val))
            {
                config.routingTable.pollingInterval =
                    std::chrono::seconds(json_object_get_int(val));
            }
        }
        else if (type == "MCTPI2CConfiguration")
        {
            config.i2c.enabled = isEnabled(entry, "Enabled");
            config.i2c.arpEnabled = isEnabled(entry, "ArpEnabled");

            struct json_object* val = nullptr;
            if (json_object_object_get_ex(entry, "Net", &val))
            {
                config.i2c.i2cNet =
                    static_cast<std::uint16_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "PollingInterval", &val))
            {
                config.i2c.pollingInterval =
                    std::chrono::seconds(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "MTU", &val))
            {
                config.i2c.mtu =
                    static_cast<std::uint32_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "OwnEID", &val))
            {
                config.i2c.ownEID =
                    static_cast<std::uint8_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "ArpIgnoreList", &val))
            {
                config.i2c.arpIgnoreList.clear();
                int ilLen = json_object_array_length(val);
                for (int j = 0; j < ilLen; j++)
                {
                    config.i2c.arpIgnoreList.insert(json_object_get_string(
                        json_object_array_get_idx(val, j)));
                }
            }
        }
        else if (type == "MCTPI2CTarget")
        {
            I2CDeviceConfig device;

            struct json_object* val = nullptr;
            if (json_object_object_get_ex(entry, "Name", &val))
            {
                device.name = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "Bus", &val))
            {
                device.bus = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "Address", &val))
            {
                device.address = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "StaticEndpointID", &val))
            {
                device.staticEndpointId = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "PollingInterval", &val))
            {
                device.pollingInterval = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "IgnoreMessageTypes", &val))
            {
                device.ignoreMessageTypes = json_object_get_string(val);
            }

            config.i2c.devices.push_back(device);
        }
        else if (type == "MCTPI3CConfiguration")
        {
            I3CInterfaceConfig iface;
            iface.enabled = isEnabled(entry, "Enabled");

            struct json_object* val = nullptr;
            if (json_object_object_get_ex(entry, "Name", &val))
            {
                iface.name = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "PhysicalLinkName", &val))
            {
                iface.physicalLinkName = json_object_get_string(val);
                iface.busNumber =
                    parseI3CBusFromLinkName(iface.physicalLinkName);
            }
            if (json_object_object_get_ex(entry, "Net", &val))
            {
                iface.i3cNet =
                    static_cast<std::uint16_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "MTU", &val))
            {
                iface.mtu =
                    static_cast<std::uint32_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "OwnEID", &val))
            {
                iface.ownEID =
                    static_cast<std::uint8_t>(json_object_get_int(val));
            }

            // The first interface populates the discovery-wide scalar
            // defaults (used by the discovery globals and single-link
            // legacy configs). Any enabled interface keeps I3C enabled.
            if (config.i3c.interfaces.empty())
            {
                config.i3c.enabled = iface.enabled;
                config.i3c.i3cNet = iface.i3cNet;
                config.i3c.mtu = iface.mtu;
            }
            else if (iface.enabled)
            {
                config.i3c.enabled = true;
            }

            // PollingInterval is discovery-wide, not per-interface.
            if (json_object_object_get_ex(entry, "PollingInterval", &val))
            {
                config.i3c.pollingInterval =
                    std::chrono::seconds(json_object_get_int(val));
            }

            config.i3c.interfaces.push_back(std::move(iface));
        }
        else if (type == "MCTPI3CTarget")
        {
            I3CDeviceConfig device;

            struct json_object* val = nullptr;
            if (json_object_object_get_ex(entry, "Name", &val))
            {
                device.name = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "Bus", &val))
            {
                device.busNum =
                    static_cast<std::uint8_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "Role", &val))
            {
                device.role = json_object_get_string(val);
            }
            else
            {
                device.role = "endpoint";
            }

            if (json_object_object_get_ex(entry, "PhysicalLinkName", &val))
            {
                device.physicalLinkName = json_object_get_string(val);
            }

            if (json_object_object_get_ex(entry, "I3CTarget", &val))
            {
                device.isTarget = json_object_get_boolean(val);
            }
            if (json_object_object_get_ex(entry, "PidMask", &val))
            {
                device.pidMask = json_object_get_string(val);
            }

            if (json_object_object_get_ex(entry, "SecondaryBusOwner", &val))
            {
                device.isSecondaryBusOwner = json_object_get_boolean(val);
            }

            if (json_object_object_get_ex(entry, "Address", &val))
            {
                int addrLen = json_object_array_length(val);
                std::string pid;
                for (int j = 0; j < addrLen; j++)
                {
                    char hex[3];
                    snprintf(
                        hex, sizeof(hex), "%02x",
                        json_object_get_int(json_object_array_get_idx(val, j)));
                    pid += hex;
                }
                device.devicePid = pid;
            }

            if (json_object_object_get_ex(entry, "StaticEndpointID", &val))
            {
                device.staticEndpointId = json_object_get_string(val);
            }

            if (json_object_object_get_ex(entry, "OwnEID", &val))
            {
                device.ownEID =
                    static_cast<std::uint8_t>(json_object_get_int(val));
            }

            if (json_object_object_get_ex(entry, "EIDPool", &val) &&
                json_object_is_type(val, json_type_array))
            {
                int poolLen = json_object_array_length(val);
                device.eidPool.clear();
                device.eidPool.reserve(poolLen);
                for (int j = 0; j < poolLen; j++)
                {
                    device.eidPool.push_back(
                        static_cast<std::uint8_t>(json_object_get_int(
                            json_object_array_get_idx(val, j))));
                }
            }

            if (json_object_object_get_ex(entry, "Net", &val))
            {
                device.netOverride =
                    static_cast<std::uint16_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "UseSharedEID", &val))
            {
                device.useSharedEID = json_object_get_boolean(val);
            }
            if (json_object_object_get_ex(entry, "RequestEIDPool", &val))
            {
                device.requestEIDPool =
                    static_cast<std::uint8_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "WaitForPoolId", &val))
            {
                device.waitForPoolId = json_object_get_boolean(val);
            }

            config.i3c.devices.push_back(device);
        }
        else if (type == "MCTPPCIeConfiguration")
        {
            PCIeInterfaceConfig iface;
            iface.enabled = isEnabled(entry, "Enabled");
            struct json_object* val = nullptr;

            if (json_object_object_get_ex(entry, "Name", &val))
            {
                iface.name = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "Net", &val))
            {
                iface.pcieNet =
                    static_cast<std::uint16_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "Role", &val))
            {
                iface.role = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "MTU", &val))
            {
                iface.mtu =
                    static_cast<std::uint32_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "PhysicalLinkName", &val))
            {
                iface.physicalLinkName = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "OwnEID", &val))
            {
                iface.ownEID =
                    static_cast<std::uint8_t>(json_object_get_int(val));
            }
            // PollingInterval is discovery-wide, not per-interface.
            if (json_object_object_get_ex(entry, "PollingInterval", &val))
            {
                config.pcie.pollingInterval =
                    std::chrono::seconds(json_object_get_int(val));
            }

#ifdef MULTI_HOST_MODE_SUPPORT
            iface.hostIndex =
                resolvePcieHostIndex(iface.physicalLinkName, nextPcieHostIndex);
#endif
            config.pcie.interfaces.push_back(iface);
        }
        else if (type == "MCTPUSBConfiguration")
        {
            config.usb.enabled = isEnabled(entry, "Enabled");
            config.usb.hotplugEnabled = isEnabled(entry, "Hotplug");

            struct json_object* val = nullptr;
            if (json_object_object_get_ex(entry, "Net", &val))
            {
                config.usb.usbNet =
                    static_cast<std::uint16_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "PollingInterval", &val))
            {
                config.usb.pollingInterval =
                    std::chrono::seconds(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "MTU", &val))
            {
                config.usb.mtu =
                    static_cast<std::uint32_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "OwnEID", &val))
            {
                config.usb.ownEID =
                    static_cast<std::uint8_t>(json_object_get_int(val));
            }
        }
        else if (type == "MCTPUSBTarget")
        {
            USBTargetConfig target;

            struct json_object* val = nullptr;
            if (json_object_object_get_ex(entry, "Name", &val))
            {
                target.name = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "Interface", &val))
            {
                target.interface = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "USBPath", &val))
            {
                target.usbPath = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "OwnEID", &val))
            {
                target.ownEid = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "StaticEndpointID", &val))
            {
                target.staticEndpointId = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "BridgePoolStartEID", &val))
            {
                target.bridgePoolStartEid = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "BridgePoolEndEID", &val))
            {
                target.bridgePoolEndEid = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "PollingInterval", &val))
            {
                target.pollingInterval = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "IgnoreEIDs", &val))
            {
                target.ignoreEids = json_object_get_string(val);
            }

            config.usb.targets.push_back(target);
        }
    }

    json_object_put(root);

    // Map each I3C target device onto its physical interface (by bus) so
    // multiple MCTPI3CConfiguration links are honoured per device.
    associateI3CDevicesWithInterfaces(config.i3c);

    info("MCTPReactorConfig loaded from JSON file {PATH}:", "PATH", jsonPath);
    info("  I2C: enabled={EN}, net={NET}",
         "EN", config.i2c.enabled, "NET", config.i2c.i2cNet);
    info("  I3C: enabled={EN}, net={NET}, devices={DEV_COUNT}",
         "EN", config.i3c.enabled, "NET", config.i3c.i3cNet,
         "DEV_COUNT", config.i3c.devices.size());
    info("  PCIe: interfaces={IFACE_COUNT}",
         "IFACE_COUNT", config.pcie.interfaces.size());
    info("  USB: enabled={EN}, net={NET}, targets={TGT_COUNT}",
         "EN", config.usb.enabled, "NET", config.usb.usbNet,
         "TGT_COUNT", config.usb.targets.size());

    return true;
}

MCTPReactorConfig MCTPReactorConfig::fromJsonFile(const std::string& jsonPath)
{
    MCTPReactorConfig config;

    loadFromJsonFile(jsonPath, config);

    return config;
}

static std::string getStringProp(const SensorBaseConfigMap& props,
                                 const std::string& key,
                                 const std::string& defaultVal = "")
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return defaultVal;
    }
    return std::visit(VariantToStringVisitor(), it->second);
}

static int64_t getIntProp(const SensorBaseConfigMap& props,
                          const std::string& key, int64_t defaultVal = 0)
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return defaultVal;
    }
    return std::visit(VariantToIntVisitor(), it->second);
}

static bool getBoolProp(const SensorBaseConfigMap& props,
                        const std::string& key, bool defaultVal = false)
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return defaultVal;
    }
    return std::visit(VariantToUnsignedIntVisitor(), it->second) != 0;
}

static bool isEnabledProp(const SensorBaseConfigMap& props,
                          const std::string& key)
{
    return getStringProp(props, key) == "enabled";
}

static std::size_t loadFromEntityManager(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    MCTPReactorConfig& config)
{
    ManagedObjectType managedObj;
    sdbusplus::message_t getManagedObjects = connection->new_method_call(
        entityManagerName, inventoryPath, "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects");

    sdbusplus::message_t reply = connection->call(getManagedObjects);
    reply.read(managedObj);

    std::size_t mctpInterfaceCount = 0;

    // Configuration interface types to look for
    const std::string mctpGeneralIntf =
        configInterfaceName("MCTPGeneralSetting");
    const std::string i2cConfigIntf =
        configInterfaceName("MCTPI2CConfiguration");
    const std::string i2cTargetIntf = configInterfaceName("MCTPI2CTarget");
    const std::string i3cConfigIntf =
        configInterfaceName("MCTPI3CConfiguration");
    const std::string i3cTargetIntf = configInterfaceName("MCTPI3CTarget");
    const std::string pcieConfigIntf =
        configInterfaceName("MCTPPCIeConfiguration");
    const std::string usbConfigIntf =
        configInterfaceName("MCTPUSBConfiguration");
    const std::string usbTargetIntf = configInterfaceName("MCTPUSBTarget");

#ifdef MULTI_HOST_MODE_SUPPORT
    // Assign each PCIe interface read to Host1, Host2, Host1, Host2...
    std::uint8_t nextPcieHostIndex = 1;
#endif
    for (const auto& [path, interfaces] : managedObj)
    {
        for (const auto& [intf, props] : interfaces)
        {
            if (intf == mctpGeneralIntf)
            {
                auto rtPoll =
                    getIntProp(props, "RoutingTablePollingInterval", 0);
                if (rtPoll > 0)
                {
                    config.routingTable.pollingInterval =
                        std::chrono::seconds(rtPoll);
                }
            }
            else if (intf == i2cConfigIntf)
            {
                ++mctpInterfaceCount;
                config.i2c.enabled = isEnabledProp(props, "Enabled");
                config.i2c.arpEnabled = isEnabledProp(props, "ArpEnabled");

                auto net = getIntProp(props, "Net", 0);
                if (net > 0)
                {
                    config.i2c.i2cNet = static_cast<std::uint16_t>(net);
                }
                auto poll = getIntProp(props, "PollingInterval", 0);
                if (poll > 0)
                {
                    config.i2c.pollingInterval = std::chrono::seconds(poll);
                }
                auto mtu = getIntProp(props, "MTU", 0);
                if (mtu > 0)
                {
                    config.i2c.mtu = static_cast<std::uint32_t>(mtu);
                }
                {
                    auto own = getIntProp(props, "OwnEID", 0);
                    if (own > 0)
                    {
                        config.i2c.ownEID = static_cast<std::uint8_t>(own);
                    }
                }

                // ArpIgnoreList comes as vector<string> from D-Bus
                auto ilIt = props.find("ArpIgnoreList");
                if (ilIt != props.end())
                {
                    auto* il =
                        std::get_if<std::vector<std::string>>(&ilIt->second);
                    if (il)
                    {
                        config.i2c.arpIgnoreList.clear();
                        config.i2c.arpIgnoreList.insert(il->begin(), il->end());
                    }
                }
            }
            else if (intf == i2cTargetIntf)
            {
                ++mctpInterfaceCount;
                I2CDeviceConfig device;
                device.name = getStringProp(props, "Name");
                device.bus = getStringProp(props, "Bus");
                device.address = getStringProp(props, "Address");
                device.staticEndpointId =
                    getStringProp(props, "StaticEndpointID");
                device.pollingInterval =
                    getStringProp(props, "PollingInterval");
                device.ignoreMessageTypes =
                    getStringProp(props, "IgnoreMessageTypes");
                config.i2c.devices.push_back(device);
            }
            else if (intf == i3cConfigIntf)
            {
                ++mctpInterfaceCount;
                I3CInterfaceConfig iface;
                iface.enabled = isEnabledProp(props, "Enabled");
                iface.name = getStringProp(props, "Name");

                auto net = getIntProp(props, "Net", 0);
                if (net > 0)
                {
                    iface.i3cNet = static_cast<std::uint16_t>(net);
                }
                auto mtu = getIntProp(props, "MTU", 0);
                if (mtu > 0)
                {
                    iface.mtu = static_cast<std::uint32_t>(mtu);
                }
                {
                    auto own = getIntProp(props, "OwnEID", 0);
                    if (own > 0)
                    {
                        iface.ownEID = static_cast<std::uint8_t>(own);
                    }
                }

                {
                    auto pln = getStringProp(props, "PhysicalLinkName");
                    if (!pln.empty())
                    {
                        iface.physicalLinkName = pln;
                        iface.busNumber =
                            parseI3CBusFromLinkName(iface.physicalLinkName);
                    }
                }

                // The first interface seeds the discovery-wide scalars;
                // any enabled interface keeps I3C enabled.
                if (config.i3c.interfaces.empty())
                {
                    config.i3c.enabled = iface.enabled;
                    config.i3c.i3cNet = iface.i3cNet;
                    config.i3c.mtu = iface.mtu;
                }
                else if (iface.enabled)
                {
                    config.i3c.enabled = true;
                }

                auto poll = getIntProp(props, "PollingInterval", 0);
                if (poll > 0)
                {
                    config.i3c.pollingInterval = std::chrono::seconds(poll);
                }

                config.i3c.interfaces.push_back(std::move(iface));
            }
            else if (intf == i3cTargetIntf)
            {
                ++mctpInterfaceCount;
                I3CDeviceConfig device;
                device.name = getStringProp(props, "Name");
                device.busNum =
                    static_cast<std::uint8_t>(getIntProp(props, "Bus"));
                device.role = getStringProp(props, "Role", "endpoint");

                device.isTarget = getBoolProp(props, "I3CTarget", false);

                std::string pidMaskStr = getStringProp(props, "PidMask");
                if (!pidMaskStr.empty())
                {
                    device.pidMask = std::move(pidMaskStr);
                }
                {
                    auto pln = getStringProp(props, "PhysicalLinkName");
                    if (!pln.empty())
                    {
                        device.physicalLinkName = pln;
                    }
                    auto own = getIntProp(props, "OwnEID", 0);
                    if (own > 0)
                    {
                        device.ownEID = static_cast<std::uint8_t>(own);
                    }
                    auto net = getIntProp(props, "Net", 0);
                    if (net > 0)
                    {
                        device.netOverride = static_cast<std::uint16_t>(net);
                    }
                    device.useSharedEID = getBoolProp(props, "UseSharedEID");
                    auto reqPool = getIntProp(props, "RequestEIDPool", 0);
                    if (reqPool > 0)
                    {
                        device.requestEIDPool =
                            static_cast<std::uint8_t>(reqPool);
                    }
                    device.waitForPoolId = getBoolProp(props, "WaitForPoolId");
                }
                device.isSecondaryBusOwner =
                    getBoolProp(props, "SecondaryBusOwner");

                // Address arrives as 'at' (uint64_t) from Entity Manager
                auto addrIt = props.find("Address");
                if (addrIt != props.end())
                {
                    std::string pid;
                    if (auto* v64 =
                            std::get_if<std::vector<uint64_t>>(&addrIt->second))
                    {
                        for (auto byte : *v64)
                        {
                            char hex[3];
                            snprintf(hex, sizeof(hex), "%02x",
                                     static_cast<uint8_t>(byte));
                            pid += hex;
                        }
                    }
                    else if (auto* v8 = std::get_if<std::vector<uint8_t>>(
                                 &addrIt->second))
                    {
                        for (auto byte : *v8)
                        {
                            char hex[3];
                            snprintf(hex, sizeof(hex), "%02x", byte);
                            pid += hex;
                        }
                    }
                    if (!pid.empty())
                    {
                        device.devicePid = pid;
                    }
                }

                device.staticEndpointId =
                    getStringProp(props, "StaticEndpointID");

                // EIDPool comes as vector<uint64_t> from D-Bus
                auto poolIt = props.find("EIDPool");
                if (poolIt != props.end())
                {
                    if (auto* p64 =
                            std::get_if<std::vector<uint64_t>>(&poolIt->second))
                    {
                        device.eidPool.assign(p64->begin(), p64->end());
                    }
                    else if (auto* p8 = std::get_if<std::vector<uint8_t>>(
                                 &poolIt->second))
                    {
                        device.eidPool = *p8;
                    }
                }

                config.i3c.devices.push_back(device);
            }
            else if (intf == pcieConfigIntf)
            {
                ++mctpInterfaceCount;
                PCIeInterfaceConfig iface;
                iface.enabled = isEnabledProp(props, "Enabled");
                iface.name = getStringProp(props, "Name");

                auto net = getIntProp(props, "Net", 0);
                if (net > 0)
                {
                    iface.pcieNet = static_cast<std::uint16_t>(net);
                }
                {
                    auto r = getStringProp(props, "Role");
                    if (!r.empty())
                        iface.role = r;
                }
                auto mtu = getIntProp(props, "MTU", 0);
                if (mtu > 0)
                {
                    iface.mtu = static_cast<std::uint32_t>(mtu);
                }
                {
                    auto pln = getStringProp(props, "PhysicalLinkName");
                    if (!pln.empty())
                    {
                        iface.physicalLinkName = pln;
                    }
                    auto own = getIntProp(props, "OwnEID", 0);
                    if (own > 0)
                    {
                        iface.ownEID = static_cast<std::uint8_t>(own);
                    }
                }
                // PollingInterval is discovery-wide, not per-interface.
                auto poll = getIntProp(props, "PollingInterval", 0);
                if (poll > 0)
                {
                    config.pcie.pollingInterval = std::chrono::seconds(poll);
                }
#ifdef MULTI_HOST_MODE_SUPPORT
                iface.hostIndex = resolvePcieHostIndex(iface.physicalLinkName,
                                                       nextPcieHostIndex);
#endif
                config.pcie.interfaces.push_back(iface);
            }
            else if (intf == usbConfigIntf)
            {
                ++mctpInterfaceCount;
                config.usb.enabled = isEnabledProp(props, "Enabled");
                config.usb.hotplugEnabled = isEnabledProp(props, "Hotplug");

                auto net = getIntProp(props, "Net", 0);
                if (net > 0)
                {
                    config.usb.usbNet = static_cast<std::uint16_t>(net);
                }
                auto poll = getIntProp(props, "PollingInterval", 0);
                if (poll > 0)
                {
                    config.usb.pollingInterval = std::chrono::seconds(poll);
                }
                auto mtu = getIntProp(props, "MTU", 0);
                if (mtu > 0)
                {
                    config.usb.mtu = static_cast<std::uint32_t>(mtu);
                }
                {
                    auto own = getIntProp(props, "OwnEID", 0);
                    if (own > 0)
                    {
                        config.usb.ownEID = static_cast<std::uint8_t>(own);
                    }
                }
            }
            else if (intf == usbTargetIntf)
            {
                ++mctpInterfaceCount;
                USBTargetConfig target;
                target.name = getStringProp(props, "Name");
                target.interface = getStringProp(props, "Interface");
                target.usbPath = getStringProp(props, "USBPath");
                target.ownEid = getStringProp(props, "OwnEID");
                target.staticEndpointId =
                    getStringProp(props, "StaticEndpointID");
                target.bridgePoolStartEid =
                    getStringProp(props, "BridgePoolStartEID");
                target.bridgePoolEndEid =
                    getStringProp(props, "BridgePoolEndEID");
                target.pollingInterval =
                    getStringProp(props, "PollingInterval");
                target.ignoreEids = getStringProp(props, "IgnoreEIDs");
                config.usb.targets.push_back(target);
            }
        }
    }

    // Map each I3C target device onto its physical interface (by bus) so
    // multiple MCTPI3CConfiguration links are honoured per device.
    associateI3CDevicesWithInterfaces(config.i3c);

    info("MCTPReactorConfig loaded from Entity Manager D-Bus:");
    info("  I2C: enabled={EN}, net={NET}",
         "EN", config.i2c.enabled, "NET", config.i2c.i2cNet);
    info("  I3C: enabled={EN}, net={NET}, devices={DEV_COUNT}",
         "EN", config.i3c.enabled, "NET", config.i3c.i3cNet,
         "DEV_COUNT", config.i3c.devices.size());
    info("  PCIe: interfaces={IFACE_COUNT}",
         "IFACE_COUNT", config.pcie.interfaces.size());
    info("  USB: enabled={EN}, net={NET}",
         "EN", config.usb.enabled, "NET", config.usb.usbNet);
    return mctpInterfaceCount;
}

std::pair<MCTPReactorConfig, std::size_t> MCTPReactorConfig::fromEntityManager(
    const std::shared_ptr<sdbusplus::asio::connection>& connection)
{
    MCTPReactorConfig config;

    auto count = loadFromEntityManager(connection, config);
    return {std::move(config), count};
}

namespace
{

// Map a single mctp::config::Configuration produced by the Reader into the
// reactor's MCTPReactorConfig structures.
void applyReaderConfig(MCTPReactorConfig& config,
                       const mctp::config::Configuration& cfg)
{
    using LinkType = mctp::types::PhysicalLinkType;
    using Role = mctp::types::Role;

    switch (cfg.physicalLinkType)
    {
        case LinkType::smbus:
        {
            const auto& i2c =
                static_cast<const mctp::config::I2CConfiguration&>(cfg);
            config.i2c.enabled = true;
            if (cfg.networkID > 0)
            {
                config.i2c.i2cNet = static_cast<std::uint16_t>(cfg.networkID);
            }
            if (cfg.mtu > 0)
            {
                config.i2c.mtu = cfg.mtu;
            }
            if (cfg.ownEID != 0)
            {
                config.i2c.ownEID = cfg.ownEID;
            }
            if (cfg.role == Role::endpoint || cfg.role == Role::bridge)
            {
                I2CDeviceConfig dev;
                dev.name = cfg.configName;
                dev.bus = std::to_string(i2c.busNumber);
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "0x%02x",
                                  i2c.defaultAddress);
                    dev.address = buf;
                }
                if (cfg.role == Role::endpoint && !cfg.eidPool.empty())
                {
                    dev.staticEndpointId = std::to_string(cfg.eidPool.front());
                }
                config.i2c.devices.push_back(std::move(dev));
            }
            break;
        }
        case LinkType::i3c:
        {
            const auto& i3c =
                static_cast<const mctp::config::I3CConfiguration&>(cfg);
            config.i3c.enabled = true;
            if (cfg.networkID > 0)
            {
                config.i3c.i3cNet = static_cast<std::uint16_t>(cfg.networkID);
            }
            if (cfg.mtu > 0)
            {
                config.i3c.mtu = cfg.mtu;
            }

            I3CDeviceConfig dev;
            dev.name = cfg.configName;
            dev.busNum = static_cast<std::uint8_t>(i3c.busNumber);
            dev.physicalLinkName = cfg.physicalLinkName;
            dev.pidMask = i3c.pidMask;
            switch (cfg.role)
            {
                case Role::endpoint:
                    dev.role = "endpoint";
                    break;
                case Role::bridge:
                    dev.role = "bridge";
                    break;
                case Role::topmostBusOwner:
                    dev.role = "bus-owner";
                    break;
            }
            if (cfg.role == Role::endpoint && !cfg.eidPool.empty())
            {
                dev.staticEndpointId = std::to_string(cfg.eidPool.front());
            }
            dev.ownEID = cfg.ownEID;
            if (cfg.networkID > 0)
            {
                dev.netOverride = static_cast<std::uint16_t>(cfg.networkID);
            }
            // For bus-owner roles the oks MctpBusOwnerConfig.EIDPool carries
            // the downstream pool {start, end} that mctpd advertises.
            if (!cfg.eidPool.empty())
            {
                dev.eidPool.assign(cfg.eidPool.begin(), cfg.eidPool.end());
            }

            // Record the physical interface so multiple I3C links (each
            // with its own PhysicalLinkName / Net) are tracked. The device
            // already carries its own netOverride and physicalLinkName, so
            // no post-pass association is required for the Reader path.
            {
                I3CInterfaceConfig iface;
                iface.name = cfg.configName;
                iface.physicalLinkName = cfg.physicalLinkName;
                iface.busNumber = static_cast<int>(i3c.busNumber);
                if (cfg.networkID > 0)
                {
                    iface.i3cNet =
                        static_cast<std::uint16_t>(cfg.networkID);
                }
                if (cfg.mtu > 0)
                {
                    iface.mtu = cfg.mtu;
                }
                config.i3c.interfaces.push_back(std::move(iface));
            }

            config.i3c.devices.push_back(std::move(dev));
            break;
        }
        case LinkType::pcie:
        {
            PCIeInterfaceConfig pcie_iface;
            pcie_iface.enabled = true;
            if (cfg.networkID > 0)
            {
                pcie_iface.pcieNet =
                    static_cast<std::uint16_t>(cfg.networkID);
            }
            if (cfg.mtu > 0)
            {
                pcie_iface.mtu = cfg.mtu;
            }
            if (!cfg.physicalLinkName.empty())
            {
                pcie_iface.physicalLinkName = cfg.physicalLinkName;
            }
            if (cfg.ownEID != 0)
            {
                pcie_iface.ownEID = cfg.ownEID;
            }
            if (cfg.role == Role::topmostBusOwner)
            {
                pcie_iface.role = "bus-owner";
                if (cfg.ownEID != 0)
                {
                    pcie_iface.ownEID = cfg.ownEID;
                }
            }
            else
            {
                pcie_iface.role = "endpoint";
            }
            config.pcie.interfaces.push_back(std::move(pcie_iface));
            break;
        }
        case LinkType::usb:
        {
            config.usb.enabled = true;
            if (cfg.networkID > 0)
            {
                config.usb.usbNet = static_cast<std::uint16_t>(cfg.networkID);
            }
            if (cfg.mtu > 0)
            {
                config.usb.mtu = cfg.mtu;
            }
            USBTargetConfig tgt;
            tgt.name = cfg.configName;
            tgt.interface = cfg.physicalLinkName;
            if (cfg.ownEID != 0)
            {
                tgt.ownEid = std::to_string(cfg.ownEID);
            }
            if (!cfg.eidPool.empty())
            {
                tgt.staticEndpointId = std::to_string(cfg.eidPool.front());
                if (cfg.eidPool.size() > 1)
                {
                    tgt.bridgePoolStartEid =
                        std::to_string(cfg.eidPool.front());
                    tgt.bridgePoolEndEid = std::to_string(cfg.eidPool.back());
                }
            }
            config.usb.targets.push_back(std::move(tgt));
            break;
        }
    }
}
} // namespace

std::pair<MCTPReactorConfig, std::size_t>
    MCTPReactorConfig::fromEntityManagerViaReader(
        const std::shared_ptr<sdbusplus::asio::connection>& connection)
{
    MCTPReactorConfig config;
    std::size_t deviceCount = 0;

    auto wrapper = std::make_shared<mctp::dbus::ConfigWrapperImpl>(connection);

    auto onConfig = [&config, &deviceCount
#ifdef MULTI_HOST_MODE_SUPPORT
                     ,
                     nextPcieHostIndex = std::uint8_t{1}
#endif
    ](std::shared_ptr<mctp::config::Configuration> cfg) mutable {
        if (!cfg)
        {
            return;
        }
        try
        {
            applyReaderConfig(config, *cfg);
            ++deviceCount;
#ifdef MULTI_HOST_MODE_SUPPORT
            if (cfg->physicalLinkType == mctp::types::PhysicalLinkType::pcie)
            {
                config.pcie.interfaces.back().hostIndex = resolvePcieHostIndex(
                    cfg->physicalLinkName, nextPcieHostIndex);
            }
#endif
        }
        catch (const std::exception& e)
        {
            error("Failed to apply Reader config '{NAME}': {ERROR}", "NAME",
                  cfg->configName, "ERROR", std::string(e.what()));
        }
    };

    auto reader =
        std::make_shared<mctp::config::Reader>(wrapper, std::move(onConfig));

    auto& io = connection->get_io_context();
    std::promise<void> done;
    auto fut = done.get_future();

    boost::asio::spawn(
        io,
        [reader](boost::asio::yield_context yield) {
            reader->readMCTPConfigurations(yield);
        },
        [&done, reader](std::exception_ptr ep) {
            if (ep)
            {
                try
                {
                    std::rethrow_exception(ep);
                }
                catch (const std::exception& e)
                {
                    error("Reader spawn failed: {ERROR}", "ERROR",
                          std::string(e.what()));
                }
            }
            done.set_value();
        });

    // Drive the io_context until the spawned coroutine completes. This is
    // safe at startup before main run() begins.
    while (fut.wait_for(std::chrono::milliseconds(0)) !=
           std::future_status::ready)
    {
        if (io.run_one() == 0)
        {
            break;
        }
    }
    io.restart();

    info(
        "MCTPReactorConfig::fromEntityManagerViaReader loaded {COUNT} MCTP device entries",
        "COUNT", deviceCount);

    return {std::move(config), deviceCount};
}

namespace
{
// Load config: try Entity Manager via the per-device MctpConfig schema
// (config::Reader) first; if no MCTP devices are exposed there, fall
// back to the legacy Entity Manager MCTP{I2C,I3C,PCIe,USB}Configuration
// interfaces, then to the JSON file. If none are available the config
// is left at compile-time defaults.
MCTPReactorConfig loadReactorConfig(
    const std::shared_ptr<sdbusplus::asio::connection>& systemBus)
{
    MCTPReactorConfig config;
    auto [emConfig, emDeviceCount] =
        MCTPReactorConfig::fromEntityManagerViaReader(systemBus);
    if (emDeviceCount > 0)
    {
        info("Loaded MCTP configuration from Entity Manager (Reader): "
             "{COUNT} devices",
             "COUNT", emDeviceCount);
        return emConfig;
    }
    try
    {
        auto [legacyConfig,
              legacyCount] = MCTPReactorConfig::fromEntityManager(systemBus);
        if (legacyCount > 0)
        {
            info("Loaded MCTP configuration from Entity Manager "
                 "(legacy MCTP*Configuration): {COUNT} interfaces",
                 "COUNT", legacyCount);
            return legacyConfig;
        }
        if (std::filesystem::exists(MCTPD_JSON_FILE_DEFAULT))
        {
            return MCTPReactorConfig::fromJsonFile();
        }
        info("No MCTP configuration source found; using defaults");
    }
    catch (const std::exception& e)
    {
        warning("Legacy Entity Manager load failed: {ERR}; falling back", "ERR",
                e.what());
        if (std::filesystem::exists(MCTPD_JSON_FILE_DEFAULT))
        {
            return MCTPReactorConfig::fromJsonFile();
        }
        info("No MCTP configuration source found; using defaults");
    }
    return config;
}
} // namespace

void MCTPReactor::buildDiscovery(
    boost::asio::io_context& io,
    const std::shared_ptr<sdbusplus::asio::connection>& systemBus)
{
    // Discovery objects hold const references into `config`; resetDiscovery()
    // must be called before reassigning it below, or those references would
    // dangle. Enforce the ordering invariant here rather than only in comments.
    assert(!discovery.i2cDiscovery && !discovery.pcieDiscovery &&
           !discovery.usbDiscovery && !discovery.i3cDiscovery &&
           !discovery.routingTableDiscovery &&
           "buildDiscovery() requires resetDiscovery() to be called first");

    config = loadReactorConfig(systemBus);

    if (config.i2c.enabled)
    {
        discovery.i2cDiscovery =
            std::make_shared<MCTPI2CDiscovery>(systemBus, config.i2c);
#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
        discovery.i2cDiscovery->setReactor(shared_from_this());
#endif
    }
    if (!config.pcie.interfaces.empty())
    {
        discovery.pcieDiscovery =
            std::make_shared<MCTPPCIeDiscovery>(systemBus, config.pcie);
    }
    if (config.usb.enabled)
    {
        discovery.usbDiscovery =
            std::make_shared<MCTPUSBDiscovery>(systemBus, config.usb);
#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
        discovery.usbDiscovery->setReactor(shared_from_this());
#endif
    }
    if (config.i3c.enabled)
    {
        discovery.i3cDiscovery =
            std::make_shared<MCTPI3CDiscovery>(systemBus, config.i3c);
    }
    discovery.routingTableDiscovery =
        std::make_shared<MCTPRoutingTableDiscovery>(systemBus,
                                                     config.routingTable);

    discovery.reactorTick.emplace(io, config.reactorTickPeriod,
                                  [this]() { tick(); });

    MCTPDiscovery::initHostMonitoring(systemBus);
}

void MCTPReactor::resetDiscovery()
{
    // Tear down the host power monitor(s) first: their D-Bus signal
    // callbacks dispatch onHostOn()/onHostOff() to the registered
    // MCTPDiscovery instances below, so the monitors must not outlive them.
    MCTPDiscovery::resetHostMonitoring();

    discovery.reactorTick.reset();

    discovery.i2cDiscovery.reset();
    discovery.pcieDiscovery.reset();
    discovery.usbDiscovery.reset();
    discovery.i3cDiscovery.reset();
    discovery.routingTableDiscovery.reset();
}
