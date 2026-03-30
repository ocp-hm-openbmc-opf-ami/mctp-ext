#include "MCTPReactorConfig.hpp"

#include "Utils.hpp"
#include "VariantVisitors.hpp"

#include <json-c/json.h>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/message.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

// System include for toml parsing (from lib/tomlc99)
extern "C"
{
#include "toml.h"
}

PHOSPHOR_LOG2_USING;

/**
 * @brief Load configuration from mctpd.conf TOML file
 * @param configPath Path to mctpd.conf file
 * @param config Config object to populate
 * @return true if file was parsed successfully, false if file not found or error
 */
static bool loadFromTomlFile(const std::string& configPath,
                             MCTPReactorConfig& config)
{
    std::ifstream file(configPath);
    if (!file.is_open())
    {
        info("Config file not found at {PATH}, using defaults", "PATH",
             configPath);
        return false;
    }

    // Read entire file into string
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string fileContent = buffer.str();

    // Parse TOML - toml_parse requires a mutable char*
    char errbuf[200];
    // Create mutable copy for parsing
    std::string mutableContent = fileContent;
    toml_table_t* conf = toml_parse(const_cast<char*>(mutableContent.c_str()), errbuf, sizeof(errbuf));

    if (!conf)
    {
        warning("Failed to parse TOML file {PATH}: {ERROR}", "PATH",
                configPath, "ERROR", errbuf);
        return false;
    }

    // --- General MCTP Configuration ---
    toml_table_t* mctp_table = toml_table_in(conf, "mctp");
    if (mctp_table)
    {
        toml_datum_t local_eid = toml_int_in(mctp_table, "local_eid");
        if (local_eid.ok)
        {
            config.localEid = static_cast<uint8_t>(local_eid.u.i);
        }
    }

    // --- I2C Configuration ---
    toml_table_t* i2c_table = toml_table_in(conf, "mctp_i2c");
    if (i2c_table)
    {
        // Read network number
        toml_datum_t network_number = toml_int_in(i2c_table, "mctp_i2c_net");
        if (network_number.ok)
        {
            config.i2c.i2cNet = static_cast<std::uint16_t>(network_number.u.i);
        }

        // Read whitelist (space-separated string)
        toml_datum_t whitelist = toml_string_in(i2c_table, "mctp_i2c_whitelist");
        if (whitelist.ok)
        {
            config.i2c.whitelist.clear();
            std::istringstream stream(whitelist.u.s);
            std::string token;
            while (stream >> token)
            {
                config.i2c.whitelist.insert(token);
            }
        }

        // Read poll_interval_secs
        toml_datum_t poll_interval = toml_int_in(i2c_table, "mctp_i2c_poll_interval_secs");
        if (poll_interval.ok)
        {
            config.i2c.pollingInterval = std::chrono::seconds(poll_interval.u.i);
        }
    }

    // --- I3C Configuration ---
    toml_table_t* i3c_table = toml_table_in(conf, "mctp_i3c");
    if (i3c_table)
    {
        // Read net
        toml_datum_t i3c_net = toml_int_in(i3c_table, "mctp_i3c_net");
        if (i3c_net.ok)
        {
            config.i3c.i3cNet = static_cast<std::uint16_t>(i3c_net.u.i);
        }

        // Read platform
        toml_datum_t platform = toml_string_in(i3c_table, "mctp_i3c_platform");
        if (platform.ok)
        {
            config.i3c.platformSoc = platform.u.s;
            free(platform.u.s);
        }

        // Read poll_interval_secs
        toml_datum_t poll_interval = toml_int_in(i3c_table, "mctp_i3c_poll_interval_secs");
        if (poll_interval.ok)
        {
            config.i3c.pollingInterval = std::chrono::seconds(poll_interval.u.i);
        }

        // Read I3C devices array
        int device_count = 0;
        toml_array_t* devices = toml_array_in(i3c_table, "devices");
        if (devices)
        {
            for (int i = 0; i < toml_array_nelem(devices); i++)
            {
                toml_table_t* device_table = toml_table_at(devices, i);
                if (!device_table)
                    continue;

                I3CDeviceConfig device;

                // Read name
                toml_datum_t name = toml_string_in(device_table, "name");
                if (name.ok)
                {
                    device.name = name.u.s;
                }

                // Read bus_num
                toml_datum_t bus_num = toml_int_in(device_table, "bus_num");
                if (bus_num.ok)
                {
                    device.busNum = static_cast<std::uint8_t>(bus_num.u.i);
                }

                // Read pid_mask
                toml_datum_t pid_mask = toml_int_in(device_table, "pid_mask");
                if (pid_mask.ok)
                {
                    device.pidMask = static_cast<std::uint32_t>(pid_mask.u.i);
                }

                // Read device_pid
                toml_datum_t device_pid = toml_string_in(device_table, "device_pid");
                if (device_pid.ok)
                {
                    device.devicePid = device_pid.u.s;
                }

                // Read role
                toml_datum_t role = toml_string_in(device_table, "role");
                if (role.ok)
                {
                    device.role = role.u.s;
                }

                // Read is_i3c_target
                toml_datum_t is_tgt = toml_bool_in(device_table, "is_i3c_target");
                if (is_tgt.ok)
                {
                    device.isTarget = is_tgt.u.b;
                }
                else
                {
                    // Default based on role
                    device.isTarget = (device.role != "bus-owner");
                }

                // Read is_secondary_bus_owner
                toml_datum_t is_secondary = toml_bool_in(device_table, "is_secondary_bus_owner");
                if (is_secondary.ok)
                {
                    device.isSecondaryBusOwner = is_secondary.u.b;
                }

                config.i3c.devices.push_back(device);
                device_count++;
            }
        }
    }

    // --- PCIe Configuration ---
    toml_table_t* pcie_table = toml_table_in(conf, "mctp_pcie");
    if (pcie_table)
    {
        // Read network number
        toml_datum_t pcie_net = toml_int_in(pcie_table, "mctp_pcie_net");
        if (pcie_net.ok)
        {
            config.pcie.pcieNet = static_cast<uint16_t>(pcie_net.u.i);
        }

        // Read PCIe role
        toml_datum_t pcie_role = toml_string_in(pcie_table, "mctp_pcie_role");
        if (pcie_role.ok)
        {
            config.pcie.pcieRole = pcie_role.u.s;
            free(pcie_role.u.s);
        }

        // Read poll_interval_secs
        toml_datum_t poll_interval = toml_int_in(pcie_table, "mctp_pcie_poll_interval_secs");
        if (poll_interval.ok)
        {
            config.pcie.pollingInterval = std::chrono::seconds(poll_interval.u.i);
        }
    }

    // --- USB Configuration ---
    toml_table_t* usb_table = toml_table_in(conf, "mctp_usb");
    if (usb_table)
    {
        toml_datum_t usb_net = toml_int_in(usb_table, "mctp_usb_net");
        if (usb_net.ok)
        {
            config.usb.usbNet = static_cast<std::uint16_t>(usb_net.u.i);
        }

        toml_datum_t poll_interval = toml_int_in(usb_table, "mctp_usb_poll_interval_secs");
        if (poll_interval.ok)
        {
            config.usb.pollingInterval = std::chrono::seconds(poll_interval.u.i);
        }
    }

    toml_free(conf);

    info("MCTPReactorConfig loaded from TOML file {PATH}:", "PATH", configPath);
    info("  I2C: net={NET}",
         "NET", config.i2c.i2cNet);
    info("  I3C: platform={PLAT}, devices={DEV_COUNT}",
         "PLAT", config.i3c.platformSoc,
         "DEV_COUNT", config.i3c.devices.size());
    info("  PCIe: net={NET}",
         "NET", config.pcie.pcieNet);
    info("  USB: net={NET}, targets={TGT_COUNT}",
         "NET", config.usb.usbNet,
         "TGT_COUNT", config.usb.targets.size());

    return true;
}

MCTPReactorConfig MCTPReactorConfig::fromTomlFile()
{
    MCTPReactorConfig config;

    loadFromTomlFile(MCTPD_CONF_FILE_DEFAULT, config);

    return config;
}

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
            if (json_object_object_get_ex(entry, "DefaultLocalEid", &val))
            {
                config.localEid =
                    static_cast<std::uint8_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(
                    entry, "RoutingTablePollingInterval", &val))
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
            if (json_object_object_get_ex(entry, "Whitelist", &val))
            {
                config.i2c.whitelist.clear();
                int wlLen = json_object_array_length(val);
                for (int j = 0; j < wlLen; j++)
                {
                    config.i2c.whitelist.insert(
                        json_object_get_string(
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
            config.i3c.enabled = isEnabled(entry, "Enabled");

            struct json_object* val = nullptr;
            if (json_object_object_get_ex(entry, "Net", &val))
            {
                config.i3c.i3cNet =
                    static_cast<std::uint16_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "PollingInterval", &val))
            {
                config.i3c.pollingInterval =
                    std::chrono::seconds(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "SoC", &val))
            {
                config.i3c.platformSoc = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "BusOwnerEID", &val))
            {
                config.i3c.busOwnerEid =
                    static_cast<std::uint8_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "EndpointEID", &val))
            {
                config.i3c.endpointEid =
                    static_cast<std::uint8_t>(json_object_get_int(val));
            }
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

            if (json_object_object_get_ex(entry, "I3CTarget", &val))
            {
                device.isTarget = json_object_get_boolean(val);
            }
            if (json_object_object_get_ex(entry, "PidMask", &val))
            {
                device.pidMask = static_cast<std::uint32_t>(
                    std::stoul(json_object_get_string(val), nullptr, 0));
            }

            if (json_object_object_get_ex(
                    entry, "SecondaryBusOwner", &val))
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
                    snprintf(hex, sizeof(hex), "%02x",
                                json_object_get_int(
                                    json_object_array_get_idx(val, j)));
                    pid += hex;
                }
                device.devicePid = pid;
            }

            if (json_object_object_get_ex(entry, "StaticEndpointID", &val))
            {
                device.staticEndpointId = json_object_get_string(val);
            }

            config.i3c.devices.push_back(device);
        }
        else if (type == "MCTPPCIeConfiguration")
        {
            config.pcie.enabled = isEnabled(entry, "Enabled");

            struct json_object* val = nullptr;
            if (json_object_object_get_ex(entry, "Net", &val))
            {
                config.pcie.pcieNet =
                    static_cast<std::uint16_t>(json_object_get_int(val));
            }
            if (json_object_object_get_ex(entry, "Role", &val))
            {
                config.pcie.pcieRole = json_object_get_string(val);
            }
            if (json_object_object_get_ex(entry, "PollingInterval", &val))
            {
                config.pcie.pollingInterval =
                    std::chrono::seconds(json_object_get_int(val));
            }
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

    info("MCTPReactorConfig loaded from JSON file {PATH}:", "PATH", jsonPath);
    info("  I2C: enabled={EN}, net={NET}",
         "EN", config.i2c.enabled, "NET", config.i2c.i2cNet);
    info("  I3C: enabled={EN}, net={NET}, devices={DEV_COUNT}",
         "EN", config.i3c.enabled, "NET", config.i3c.i3cNet,
         "DEV_COUNT", config.i3c.devices.size());
    info("  PCIe: enabled={EN}, net={NET}",
         "EN", config.pcie.enabled, "NET", config.pcie.pcieNet);
    info("  USB: enabled={EN}, net={NET}, targets={TGT_COUNT}",
         "EN", config.usb.enabled, "NET", config.usb.usbNet,
         "TGT_COUNT", config.usb.targets.size());

    return true;
}

MCTPReactorConfig MCTPReactorConfig::fromJsonFile(
    const std::string& jsonPath)
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

static void loadFromEntityManager(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    MCTPReactorConfig& config)
{
    ManagedObjectType managedObj;
    sdbusplus::message_t getManagedObjects = connection->new_method_call(
        entityManagerName, inventoryPath,
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");

    sdbusplus::message_t reply = connection->call(getManagedObjects);
    reply.read(managedObj);

    // Configuration interface types to look for
    const std::string mctpGeneralIntf =
        configInterfaceName("MCTPGeneralSetting");
    const std::string i2cConfigIntf =
        configInterfaceName("MCTPI2CConfiguration");
    const std::string i2cTargetIntf =
        configInterfaceName("MCTPI2CTarget");
    const std::string i3cConfigIntf =
        configInterfaceName("MCTPI3CConfiguration");
    const std::string i3cTargetIntf =
        configInterfaceName("MCTPI3CTarget");
    const std::string pcieConfigIntf =
        configInterfaceName("MCTPPCIeConfiguration");
    const std::string usbConfigIntf =
        configInterfaceName("MCTPUSBConfiguration");
    const std::string usbTargetIntf =
        configInterfaceName("MCTPUSBTarget");

    for (const auto& [path, interfaces] : managedObj)
    {
        for (const auto& [intf, props] : interfaces)
        {
            if (intf == mctpGeneralIntf)
            {
                config.localEid = static_cast<std::uint8_t>(
                    getIntProp(props, "DefaultLocalEid", config.localEid));
                auto rtPoll = getIntProp(
                    props, "RoutingTablePollingInterval", 0);
                if (rtPoll > 0)
                {
                    config.routingTable.pollingInterval =
                        std::chrono::seconds(rtPoll);
                }
            }
            else if (intf == i2cConfigIntf)
            {
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

                // Whitelist comes as vector<string> from D-Bus
                auto wlIt = props.find("Whitelist");
                if (wlIt != props.end())
                {
                    auto* wl = std::get_if<std::vector<std::string>>(
                        &wlIt->second);
                    if (wl)
                    {
                        config.i2c.whitelist.clear();
                        config.i2c.whitelist.insert(wl->begin(), wl->end());
                    }
                }
            }
            else if (intf == i2cTargetIntf)
            {
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
                config.i3c.enabled = isEnabledProp(props, "Enabled");

                auto net = getIntProp(props, "Net", 0);
                if (net > 0)
                {
                    config.i3c.i3cNet = static_cast<std::uint16_t>(net);
                }
                auto poll = getIntProp(props, "PollingInterval", 0);
                if (poll > 0)
                {
                    config.i3c.pollingInterval = std::chrono::seconds(poll);
                }

                std::string soc = getStringProp(props, "SoC");
                if (!soc.empty())
                {
                    config.i3c.platformSoc = soc;
                }
            }
            else if (intf == i3cTargetIntf)
            {
                I3CDeviceConfig device;
                device.name = getStringProp(props, "Name");
                device.busNum = static_cast<std::uint8_t>(
                    getIntProp(props, "Bus"));
                device.role = getStringProp(props, "Role", "endpoint");

                device.isTarget =
                    getBoolProp(props, "I3CTarget",
                                device.role != "bus-owner");

                std::string pidMaskStr = getStringProp(props, "PidMask");
                if (!pidMaskStr.empty())
                {
                    device.pidMask = static_cast<std::uint32_t>(
                        std::stoul(pidMaskStr, nullptr, 0));
                }
                device.isSecondaryBusOwner =
                    getBoolProp(props, "SecondaryBusOwner");

                // Address comes as vector<uint8_t> PID bytes from D-Bus
                auto addrIt = props.find("Address");
                if (addrIt != props.end())
                {
                    auto* addrVec =
                        std::get_if<std::vector<uint8_t>>(&addrIt->second);
                    if (addrVec)
                    {
                        std::string pid;
                        for (auto byte : *addrVec)
                        {
                            char hex[3];
                            snprintf(hex, sizeof(hex), "%02x", byte);
                            pid += hex;
                        }
                        device.devicePid = pid;
                    }
                }

                device.staticEndpointId =
                    getStringProp(props, "StaticEndpointID");

                config.i3c.devices.push_back(device);
            }
            else if (intf == pcieConfigIntf)
            {
                config.pcie.enabled = isEnabledProp(props, "Enabled");

                auto net = getIntProp(props, "Net", 0);
                if (net > 0)
                {
                    config.pcie.pcieNet = static_cast<std::uint16_t>(net);
                }
                config.pcie.pcieRole =
                    getStringProp(props, "Role", config.pcie.pcieRole);
                auto poll = getIntProp(props, "PollingInterval", 0);
                if (poll > 0)
                {
                    config.pcie.pollingInterval = std::chrono::seconds(poll);
                }
            }
            else if (intf == usbConfigIntf)
            {
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
            }
            else if (intf == usbTargetIntf)
            {
                USBTargetConfig target;
                target.name = getStringProp(props, "Name");
                target.interface = getStringProp(props, "Interface");
                target.usbPath = getStringProp(props, "USBPath");
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

    info("MCTPReactorConfig loaded from Entity Manager D-Bus:");
    info("  I2C: enabled={EN}, net={NET}",
         "EN", config.i2c.enabled, "NET", config.i2c.i2cNet);
    info("  I3C: enabled={EN}, net={NET}, devices={DEV_COUNT}",
         "EN", config.i3c.enabled, "NET", config.i3c.i3cNet,
         "DEV_COUNT", config.i3c.devices.size());
    info("  PCIe: enabled={EN}, net={NET}",
         "EN", config.pcie.enabled, "NET", config.pcie.pcieNet);
    info("  USB: enabled={EN}, net={NET}",
         "EN", config.usb.enabled, "NET", config.usb.usbNet);
}

MCTPReactorConfig MCTPReactorConfig::fromEntityManager(
    const std::shared_ptr<sdbusplus::asio::connection>& connection)
{
    MCTPReactorConfig config;

    loadFromEntityManager(connection, config);

    return config;
}
