#include "MCTPI3CDiscovery.hpp"

// clang-format off
// <net/if.h> must precede <linux/mctp.h> to avoid IFF_* and struct
// redefinition conflicts with the transitively included <linux/if.h>.
#include <net/if.h>
// clang-format on
#include <linux/if_addr.h>
#include <linux/if_link.h>
#include <linux/mctp.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>

PHOSPHOR_LOG2_USING;

namespace fs = std::filesystem;

// EID range constants per DSP0236.
static constexpr uint8_t kMctpEidRangeStart = 0x08;
static constexpr uint8_t kMctpEidBroadcast = 0xFF;

// ----------------------------------------------------------------
// Construction
// ----------------------------------------------------------------

MCTPI3CDiscovery::MCTPI3CDiscovery(
    const std::shared_ptr<sdbusplus::asio::connection>& bus,
    const I3CDiscoveryConfig& config) :
    MCTPDiscovery(bus), mctpI3cNet(config.i3cNet),
    busownerLocalEid(config.busOwnerEid), endpointLocalEid(config.endpointEid),
    platform(config.platformSoc)
{
    // Populate device registry from config
    for (const auto& dev : config.devices)
    {
        I3CMctpDevice device;
        device.busNumber = dev.busNum;
        device.name = dev.name;
        device.pidMask = static_cast<uint16_t>(dev.pidMask);
        device.role = dev.role;
        device.isTarget = dev.isTarget;
        device.isSecondaryBusOwner = dev.isSecondaryBusOwner;

        // Parse devicePid hex string into hwAddr[kI3cPidLen]
        if (!dev.devicePid.empty())
        {
            std::string pid = dev.devicePid;
            // Pad to kI3cPidLen*2 hex chars (kI3cPidLen bytes)
            while (pid.size() < kI3cPidLen * 2)
            {
                pid = "0" + pid;
            }
            for (size_t i = 0; i < kI3cPidLen; i++)
            {
                device.hwAddr[i] = static_cast<uint8_t>(
                    std::stoul(pid.substr(i * 2, 2), nullptr, 16));
            }
        }

        devices.push_back(device);
    }
}

// ----------------------------------------------------------------
// MCTPDiscovery interface
// ----------------------------------------------------------------

void MCTPI3CDiscovery::run()
{
    pwrResetHandler();
}

// ----------------------------------------------------------------
// triggerPwrReset - latched dispatcher used by onHostOn / onPlatformReset
// ----------------------------------------------------------------

void MCTPI3CDiscovery::triggerPwrReset(const char* source)
{
    if (resetTriggered)
    {
        info("i3c pwrResetHandler already handled this power cycle "
             "(source={SOURCE} suppressed)",
             "SOURCE", source);
        return;
    }
    resetTriggered = true;
    info("Triggering i3c pwrResetHandler (source={SOURCE})", "SOURCE", source);
    pwrResetHandler();
}

// ----------------------------------------------------------------
// pwrResetHandler
// ----------------------------------------------------------------

void MCTPI3CDiscovery::pwrResetHandler()
{
    if (pwrResetInProgress)
    {
        info("PowerReset handler already running, skipping");
        return;
    }
    pwrResetInProgress = true;

    info("PowerReset handler triggered");

    info("Platform reset Detected, clearing all registered EIDs");

    for (auto& device : devices)
    {
        // Clear BusOwner and Endpoint EIDs in device registry
        device.eid = 0;

        // Reset controller device detection status after power reset
        if (!device.isTarget)
        {
            device.detected = false;
            device.dynamicAddr = 0;
            info("Reset detection status for controller device: {NAME}", "NAME",
                 device.name);
        }
    }

    /// @brief BHS Changes
    if (platform == "aspeed-2600" || platform == "aspeed-2700")
    {
        /// Scan I3C bus and update hardware and dynamic address
        scanI3CBuses();

        // Log detection status for all controller devices
        for (const auto& device : devices)
        {
            if (!device.isTarget)
            {
                info("Controller device {NAME} detection status: {STATUS}",
                     "NAME", device.name, "STATUS",
                     device.detected ? "DETECTED" : "NOT DETECTED");
            }
        }
    }
    else
    {
        if (platform == "nuvoton")
        {
            discoverI3CDevices();
        }
        // For non-ASPEED SOC platforms, re-trigger device detection
        // using PID-based dynamic address lookup
        info("Re-triggering device detection for non-ASPEED platform");
        for (auto& device : devices)
        {
            if (!device.isTarget)
            {
                // Convert hwAddr (PID) to string format for lookup
                std::string pidStr = convertHwAddrToPidString(device.hwAddr);

                if (!pidStr.empty() && pidStr != "000000000000")
                {
                    try
                    {
                        std::string dynamicAddr =
                            findDynamicAddressByPID(pidStr, device);
                        if (!dynamicAddr.empty())
                        {
                            info("Re-detected controller device PID: {PID}, "
                                 "Dynamic Address: {ADDR}",
                                 "PID", pidStr, "ADDR", dynamicAddr);
                        }
                        else
                        {
                            info("Controller device PID: {PID} not detected "
                                 "after power reset",
                                 "PID", pidStr);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        error("Error re-detecting device PID {PID}: {ERR}",
                              "PID", pidStr, "ERR", e.what());
                    }
                }
            }
        }
    }

    // Reconfigure local interfaces after power reset
    setupLocalInterfaces();

    // Initiate device discovery for Endpoint and send discovery notify
    deviceDiscoveryWorkflow();

    pwrResetInProgress = false;
}

// ----------------------------------------------------------------
// isEidValid
// ----------------------------------------------------------------

bool MCTPI3CDiscovery::isEidValid(uint8_t eid) const
{
    if (eid < kMctpEidRangeStart || eid == kMctpEidBroadcast)
        return false;
    return true;
}

// ----------------------------------------------------------------
// scanI3CBuses
// ----------------------------------------------------------------

void MCTPI3CDiscovery::scanI3CBuses()
{
    constexpr int maxRetries = 10;
    constexpr int retryDelaySecs = 1;

    // Group devices per bus
    std::unordered_map<int, std::vector<I3CMctpDevice*>> controllerBuses;

    for (auto& dev : devices)
    {
        if (!dev.isTarget)
        {
            controllerBuses[dev.busNumber].push_back(&dev);
        }
    }

    // Scan each bus — trigger sysfs rescan and match PIDs with retries
    for (auto& [busNum, devList] : controllerBuses)
    {
        info("Scanning bus {BUS} for {COUNT} devices", "BUS", busNum, "COUNT",
             devList.size());

        // Map bus number to platform device path (matches mctp-i3c-rescan.sh)
        std::string busPath;
        if (platform == "aspeed-2700")
        {
            // AST2700 address mapping
            static const std::unordered_map<int, std::string> ast2700Map = {
                {0, "14c20000.i3c0"}, {1, "14c21000.i3c1"},
                {2, "14c22000.i3c2"}, {3, "14c23000.i3c3"},
                {4, "14c24000.i3c4"}, {5, "14c25000.i3c5"},
                {6, "14c26000.i3c6"},
            };
            auto it = ast2700Map.find(busNum);
            if (it != ast2700Map.end())
                busPath = it->second;
        }
        else
        {
            // AST2600 address mapping
            static const std::unordered_map<int, std::string> ast2600Map = {
                {0, "1e7a2000.i3c0"}, {1, "1e7a3000.i3c1"},
                {2, "1e7a4000.i3c2"}, {3, "1e7a5000.i3c3"},
                {4, "1e7a6000.i3c4"}, {5, "1e7a7000.i3c5"},
            };
            auto it = ast2600Map.find(busNum);
            if (it != ast2600Map.end())
                busPath = it->second;
        }

        if (busPath.empty())
        {
            error("Invalid bus_num {BUS} for platform {PLAT}", "BUS", busNum,
                  "PLAT", platform);
            continue;
        }

        std::string basePath = "/sys/bus/platform/devices";
        std::string platformDevPath = basePath + "/" + busPath;

        if (!fs::is_directory(platformDevPath))
        {
            error("I3C bus path not found: {PATH}", "PATH", platformDevPath);
            continue;
        }

        info("I3C bus path verified: {PATH}", "PATH", platformDevPath);

        // Find the i3c-N subdirectory and rescan file
        std::string rescanFilePath;
        std::string i3cBusPath;

        for (const auto& busEntry : fs::directory_iterator(
                 platformDevPath,
                 fs::directory_options::skip_permission_denied))
        {
            if (!busEntry.is_directory())
                continue;

            std::string busDirName = busEntry.path().filename().string();
            if (busDirName.find("i3c") == std::string::npos)
                continue;

            std::string candidate =
                busEntry.path().generic_string() + "/rescan";
            if (fs::exists(candidate))
            {
                rescanFilePath = candidate;
                i3cBusPath = busEntry.path().generic_string();
                break;
            }
        }

        if (rescanFilePath.empty())
        {
            error("No rescan file found under {PATH}", "PATH", platformDevPath);
            continue;
        }

        info("Bus {BUS}: rescan file={RESCAN}, i3c bus path={PATH}", "BUS",
             busNum, "RESCAN", rescanFilePath, "PATH", i3cBusPath);

        // Retry loop: rescan and check devices until all detected or max
        // retries
        bool allDetected = false;
        for (int attempt = 0; attempt < maxRetries && !allDetected; ++attempt)
        {
            debug("Rescan attempt {N} for bus {BUS}", "N", attempt + 1, "BUS",
                  busNum);

            // Trigger rescan
            int fd = open(rescanFilePath.c_str(), O_WRONLY);
            if (fd < 0)
            {
                error("Failed to open rescan file for bus {BUS}", "BUS",
                      busNum);
                break;
            }
            const char* one = "1";
            if (write(fd, one, 1) != 1)
            {
                error("Failed to write to rescan file for bus {BUS}", "BUS",
                      busNum);
                close(fd);
                break;
            }
            close(fd);
            sleep(retryDelaySecs);

            // Scan for matching PIDs inside the i3c-N directory
            for (const auto& devEntry : fs::directory_iterator(i3cBusPath))
            {
                if (!fs::is_directory(devEntry))
                    continue;

                std::string pidPath = devEntry.path().generic_string() + "/pid";
                if (!fs::exists(pidPath))
                    continue;

                debug("Checking device entry: {PATH}", "PATH",
                      devEntry.path().generic_string());

                std::ifstream pidFile(pidPath);
                if (!pidFile)
                    continue;

                std::string pidStr;
                std::getline(pidFile, pidStr);
                pidFile.close();

                uint64_t devicePid = 0;
                try
                {
                    devicePid = std::stoull(pidStr, nullptr, 16);
                }
                catch (...)
                {
                    continue;
                }

                for (auto* dev : devList)
                {
                    if (dev->detected)
                        continue;

                    bool matched = false;

                    // PID mask check
                    if (dev->pidMask != 0)
                    {
                        uint16_t instIDRsvdVal =
                            static_cast<uint16_t>((devicePid & 0xFF0F));
                        if (instIDRsvdVal == dev->pidMask)
                        {
                            matched = true;
                        }
                    }

                    // Direct PID check
                    if (!matched)
                    {
                        uint64_t devPid = 0;
                        for (size_t i = 0; i < kI3cPidLen; i++)
                        {
                            devPid = (devPid << 8) | dev->hwAddr[i];
                        }
                        if (devicePid == devPid)
                        {
                            matched = true;
                        }
                    }

                    if (matched)
                    {
                        // Verify device is active via status file
                        if (!checkDeviceStatus(devEntry.path()))
                        {
                            warning("Device {NAME} PID matched but status "
                                    "check failed, will retry",
                                    "NAME", dev->name);
                            continue;
                        }

                        // Read dynamic address
                        std::string dynAddrPath =
                            devEntry.path().generic_string() +
                            "/dynamic_address";
                        if (fs::exists(dynAddrPath))
                        {
                            std::ifstream dynAddrFile(dynAddrPath);
                            if (dynAddrFile)
                            {
                                std::string dynAddr;
                                std::getline(dynAddrFile, dynAddr);
                                try
                                {
                                    dev->dynamicAddr = static_cast<uint8_t>(
                                        std::stoull(dynAddr, nullptr, 16));
                                }
                                catch (...)
                                {}
                            }
                        }

                        dev->detected = true;
                        info("Detected device: name={NAME} on attempt {N}",
                             "NAME", dev->name, "N", attempt + 1);

                        // Update hwAddr from actual PID if using mask
                        if (dev->pidMask != 0)
                        {
                            for (size_t i = 0; i < kI3cPidLen; i++)
                            {
                                dev->hwAddr[i] =
                                    (devicePid >>
                                     (8 * ((kI3cPidLen - 1) - i))) &
                                    0xFF;
                            }
                        }
                    }
                }
            }

            // Check if all devices on this bus are detected
            allDetected = true;
            for (const auto* dev : devList)
            {
                if (!dev->detected)
                {
                    allDetected = false;
                    info("Attempt {N}: device {NAME} not yet detected", "N",
                         attempt + 1, "NAME", dev->name);
                    break;
                }
            }
        }

        if (allDetected)
        {
            info("All devices detected on bus {BUS}", "BUS", busNum);
        }
        else
        {
            warning("Not all devices detected on bus {BUS} after {N} attempts",
                    "BUS", busNum, "N", maxRetries);
        }
    }
}

// ----------------------------------------------------------------
// checkDeviceStatus — verify device is active via sysfs status file
// ----------------------------------------------------------------

bool MCTPI3CDiscovery::checkDeviceStatus(const fs::path& devicePath) const
{
    fs::path statusFile = devicePath / "status";
    constexpr int maxAttempts = 3;

    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        if (fs::exists(statusFile))
        {
            std::ifstream file(statusFile);
            if (file.good())
            {
                return true;
            }
        }

        if (attempt < maxAttempts - 1)
        {
            debug("checkDeviceStatus: status check failed for {PATH}, "
                  "retrying...",
                  "PATH", devicePath.string());
        }
    }

    return false;
}

void MCTPI3CDiscovery::discoverI3CDevices()
{
    // Group devices per bus
    std::unordered_map<int, std::vector<I3CMctpDevice*>> controllerBuses;

    for (auto& dev : devices)
    {
        if (!dev.isTarget)
        {
            controllerBuses[dev.busNumber].push_back(&dev);
        }
    }

    for (auto& [busNum, devList] : controllerBuses)
    {
        debug("Scanning bus {BUS} for {COUNT} devices", "BUS", busNum, "COUNT",
              devList.size());

        // Construct the discover file path for this bus
        std::string discoverFilePath =
            "/sys/bus/i3c/devices/i3c-" + std::to_string(busNum) + "/discover";
        if (!fs::exists(discoverFilePath))
        {
            error("Discover file does not exist for bus {BUS}: {PATH}", "BUS",
                  busNum, "PATH", discoverFilePath);
            continue;
        }
        int fd = open(discoverFilePath.c_str(), O_WRONLY);
        if (fd < 0)
        {
            error("Failed to open discover file for bus {BUS}", "BUS", busNum);
            continue;
        }
        const char* writeData = "1";
        if (write(fd, writeData, 1) != 1)
        {
            error("Failed to write to discover file for bus {BUS}", "BUS",
                  busNum);
            close(fd);
            continue;
        }
        close(fd);
        sleep(1); // Wait for discovery to complete
        info("Triggered I3C device discovery on bus {BUS}", "BUS", busNum);
    }
}

// ----------------------------------------------------------------
// convertHwAddrToPidString
// ----------------------------------------------------------------

std::string MCTPI3CDiscovery::convertHwAddrToPidString(
    const uint8_t hwAddr[kI3cPidLen]) const
{
    std::string pidStr;
    for (size_t i = 0; i < kI3cPidLen; i++)
    {
        char byte[3];
        snprintf(byte, sizeof(byte), "%02x", hwAddr[i]);
        pidStr += byte;
    }
    return pidStr;
}

// ----------------------------------------------------------------
// normalizePID
// ----------------------------------------------------------------

std::string MCTPI3CDiscovery::normalizePID(const std::string& pid)
{
    size_t firstNonZero = pid.find_first_not_of('0');
    if (firstNonZero == std::string::npos)
    {
        return "0";
    }
    return pid.substr(firstNonZero);
}

// ----------------------------------------------------------------
// readDynamicAddress
// ----------------------------------------------------------------

std::string MCTPI3CDiscovery::readDynamicAddress(const fs::path& path)
{
    std::ifstream file(path);
    if (!file.is_open())
        return "";
    std::string address;
    file >> address;
    return address;
}

// ----------------------------------------------------------------
// findDynamicAddressByPID
// ----------------------------------------------------------------

std::string MCTPI3CDiscovery::findDynamicAddressByPID(const std::string& pid,
                                                      I3CMctpDevice& device)
{
    try
    {
        std::string normalizedPID = normalizePID(pid);
        debug("Searching for PID: {PID} (normalized: {NPID})", "PID", pid,
              "NPID", normalizedPID);

        const fs::path basePath("/sys/bus/i3c/devices");
        if (!fs::exists(basePath) || !fs::is_directory(basePath))
        {
            debug("I3C devices base path not found");
            return "";
        }

        for (const auto& entry : fs::recursive_directory_iterator(basePath))
        {
            if (!fs::is_directory(entry))
                continue;

            const std::string dirName = entry.path().filename().string();

            if (dirName.find(normalizedPID) == std::string::npos)
                continue;

            // PID is usually after the dash, e.g., 1-20a012900ef
            auto dashPos = dirName.find('-');
            if (dashPos != std::string::npos)
            {
                std::string entryPID = dirName.substr(dashPos + 1);
                if (entryPID == normalizedPID)
                {
                    // Check if pid file exists and matches
                    fs::path pidFilePath = entry.path() / "pid";
                    if (fs::exists(pidFilePath))
                    {
                        std::ifstream pidFile(pidFilePath);
                        if (pidFile)
                        {
                            std::string fileContent;
                            std::getline(pidFile, fileContent);
                            pidFile.close();

                            if (fileContent == normalizedPID)
                            {
                                fs::path dynAddrPath =
                                    entry.path() / "dynamic_address";
                                if (!fs::exists(dynAddrPath))
                                {
                                    debug("Dynamic address file not found "
                                          "for PID {PID}",
                                          "PID", pid);
                                    return "";
                                }

                                std::string dynAddr =
                                    readDynamicAddress(dynAddrPath);
                                if (!dynAddr.empty())
                                {
                                    device.detected = true;
                                    info("Device detected: {NAME} (PID: "
                                         "{PID}, DynAddr: 0x{ADDR})",
                                         "NAME", device.name, "PID", pid,
                                         "ADDR", dynAddr);
                                }
                                return dynAddr;
                            }
                        }
                    }
                }
            }
        }
    }
    catch (const fs::filesystem_error& e)
    {
        error("Filesystem error while searching for PID {PID}: {ERR}", "PID",
              pid, "ERR", e.what());
        return "";
    }
    catch (const std::exception& e)
    {
        error("Error while searching for PID {PID}: {ERR}", "PID", pid, "ERR",
              e.what());
        return "";
    }
    return "";
}

// ----------------------------------------------------------------
// setupLocalInterfaces
// ----------------------------------------------------------------

int MCTPI3CDiscovery::setupLocalInterfaces()
{
    info("Initialising mctp local interface(s) and addr(s)");

    deleteAllPidMappings();

    for (auto& device : devices)
    {
        // Skip controller devices that were not detected during bus scanning
        if (!device.isTarget && !device.detected)
        {
            info("Skipping interface setup for undetected controller "
                 "device: {NAME}",
                 "NAME", device.name);
            continue;
        }

        // Skip secondary bus owners ??handled later via DiscoveryNotify
        if (device.isSecondaryBusOwner)
        {
            info("Skipping interface setup for secondary bus owner: {NAME}",
                 "NAME", device.name);
            continue;
        }

        if (!device.isTarget)
        {
            device.localEid = busownerLocalEid;
        }
        else
        {
            device.localEid = endpointLocalEid;
        }

        std::string interfaceName;
        if (!device.isTarget)
        {
            // Bus owner interface: mctpi3cN
            int busIdx = (platform == "aspeed-2600" && device.busNumber >= 2)
                             ? (device.busNumber - 2)
                             : device.busNumber;
            interfaceName = "mctpi3c" + std::to_string(busIdx);
        }
        else
        {
            // Target interface: look up from /sys/class/net/
            interfaceName = findI3CTargetInterface();
            if (interfaceName.empty())
            {
                error("No mctpi3c-target interface found for {NAME}", "NAME",
                      device.name);
                continue;
            }
        }

        if (!ensureInterfaceReady(interfaceName, device.localEid, mctpI3cNet))
        {
            error("Failed to configure local EID {EID} for {NAME}", "EID",
                  lg2::hex, device.localEid, "NAME", device.name);
        }

        // Add PID mapping for endpoint devices (remote is bus owner)
        if (!device.isTarget && device.detected)
        {
            if (addPidMapping(device.localEid, device.hwAddr) < 0)
            {
                warning("Failed to add PID mapping for {NAME} EID {EID}",
                        "NAME", device.name, "EID", lg2::hex, device.localEid);
            }
        }
    }
    return 0;
}

// ----------------------------------------------------------------
// deviceDiscoveryWorkflow
// ----------------------------------------------------------------

int MCTPI3CDiscovery::deviceDiscoveryWorkflow()
{
    info("Initiating device discovery workflow");

    for (auto& device : devices)
    {
        // Skip controller devices that were not detected during bus scanning
        if (!device.isTarget && !device.detected)
        {
            info("Skipping device discovery for undetected controller "
                 "device: {NAME} (PID 0x{PID})",
                 "NAME", device.name, "PID", lg2::hex, device.pidMask);
            continue;
        }

        if (!device.isTarget && device.detected && !device.isSecondaryBusOwner)
        {
            if (!sendDiscoveryNotify(device))
            {
                error("sendDiscoveryNotify failed for {NAME}", "NAME",
                      device.name);
            }
        }
    }

    // Send DiscoveryNotify for secondary bus owners after 20s delay
    bool hasSecondary = false;
    for (const auto& device : devices)
    {
        if (device.isSecondaryBusOwner && !device.isTarget &&
            (device.isTarget || device.detected))
        {
            hasSecondary = true;
            break;
        }
    }

    if (hasSecondary)
    {
        info("Waiting 20 seconds before notifying secondary bus owners");
        sleep(20);

        for (auto& device : devices)
        {
            if (!device.isTarget && !device.detected)
            {
                continue;
            }

            if (!device.isTarget && device.isSecondaryBusOwner)
            {
                if (!sendDiscoveryNotify(device))
                {
                    error("sendDiscoveryNotify failed for secondary "
                          "bus owner {NAME}",
                          "NAME", device.name);
                }
            }
        }
    }

    return 0;
}

// ----------------------------------------------------------------
// sendDiscoveryNotify — D-Bus call to codeconstruct mctpd
// ----------------------------------------------------------------

bool MCTPI3CDiscovery::sendDiscoveryNotify(const I3CMctpDevice& device)
{
    // Build the 6-byte PID vector from hwAddr
    std::vector<std::uint8_t> formattedPid(device.hwAddr,
                                           device.hwAddr + pidSize);

    // Determine interface name based on device type (matches
    // mctp-i3c-rescan.sh) MCTPI3CTarget (role=endpoint): remote is I3C target,
    // BMC is bus owner -> mctpi3cN MCTPI3CBusOwner (role=bus-owner): remote is
    // bus owner, BMC is target -> mctpi3c-targetN
    std::string interfaceName;
    if (!device.isTarget)
    {
        // Bus owner interface: mctpi3cN
        int busIdx = (platform == "aspeed-2600" && device.busNumber >= 2)
                         ? (device.busNumber - 2)
                         : device.busNumber;
        interfaceName = "mctpi3c" + std::to_string(busIdx);
    }
    else
    {
        // Target interface: look up from /sys/class/net/
        interfaceName = findI3CTargetInterface();
    }

    if (interfaceName.empty())
    {
        error("sendDiscoveryNotify: no mctpi3c target interface "
              "found for device {NAME}",
              "NAME", device.name);
        return false;
    }
    // mctpd converts dash to underscore in D-Bus paths
    // (see convert_dash_to_underscore() in mctpd.c)
    std::string dbusIfaceName = interfaceName;
    for (auto& c : dbusIfaceName)
    {
        if (c == '-')
            c = '_';
    }
    std::string dbusPath =
        std::string(mctp::dbus::basePath) + "/interfaces/" + dbusIfaceName;

    constexpr int maxAttempts = 3;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        try
        {
            auto method = bus->new_method_call(
                mctp::dbus::service.data(), dbusPath.c_str(),
                mctp::dbus::interfaceIface.data(), "DiscoveryNotify");
            method.append(formattedPid);
            bus->call(method);

            info("DiscoveryNotify sent for {NAME} (interface: {IFACE})", "NAME",
                 device.name, "IFACE", interfaceName);
            return true;
        }
        catch (const std::exception& e)
        {
            if (attempt < maxAttempts - 1)
            {
                debug(
                    "DiscoveryNotify attempt {N} failed for {NAME}, retrying...",
                    "N", attempt + 1, "NAME", device.name);
                sleep(1);
            }
            else
            {
                error(
                    "DiscoveryNotify failed for {NAME} after {N} attempts: {ERR}",
                    "NAME", device.name, "N", maxAttempts, "ERR", e.what());
            }
        }
    }
    return false;
}

// ----------------------------------------------------------------
// findI3CTargetInterface — look up actual kernel interface name
// ----------------------------------------------------------------

std::string MCTPI3CDiscovery::findI3CTargetInterface() const
{
    // The kernel creates network interfaces named mctpi3c-targetN (dash).
    // mctpd converts dash to underscore for D-Bus paths.
    // Scan /sys/class/net/ to find the first matching interface.
    // Retry up to 5 times with 2-second delay, as the kernel mctp-i3c
    // driver may take time to create the interface after rescan/IBI setup.
    const fs::path netPath("/sys/class/net");
    constexpr int maxRetries = 5;
    constexpr int retryDelayUs = 2000000; // 2 seconds

    for (int attempt = 0; attempt < maxRetries; ++attempt)
    {
        if (attempt > 0)
        {
            debug("findI3CTargetInterface: retry {N}/{MAX}", "N", attempt + 1,
                  "MAX", maxRetries);
            usleep(retryDelayUs);
        }

        try
        {
            if (!fs::exists(netPath))
            {
                warning("/sys/class/net does not exist");
                return "";
            }

            for (const auto& entry : fs::directory_iterator(netPath))
            {
                std::string ifName = entry.path().filename().string();
                // Kernel uses dash: mctpi3c-targetN
                // Also check underscore in case of variant drivers
                if (ifName.find("mctpi3c-target") == 0)
                {
                    debug("findI3CTargetInterface: found {IFACE}", "IFACE",
                          ifName);
                    return ifName;
                }
            }
        }
        catch (const std::exception& e)
        {
            warning("findI3CTargetInterface error: {ERR}", "ERR", e.what());
        }
    }

    warning("findI3CTargetInterface: no mctpi3c target interface found "
            "after {N} attempts",
            "N", maxRetries);
    return "";
}

// ----------------------------------------------------------------
// deleteAllPidMappings — query kernel for all PID mappings and delete each
// ----------------------------------------------------------------

// PID mapping ioctl structures (must match kernel definitions)
static constexpr int maxPidMapEntries = 64;
static constexpr int pidDataSize = 6;

struct PidMapEntry
{
    uint8_t eid;
    unsigned char pid[6];
};

struct PidMapBulk
{
    uint32_t count;
    PidMapEntry entries[64];
};

struct PidMapReq
{
    uint8_t eid;
    unsigned char pid[6];
};

#ifndef SIOCMCTPSETPIDMAP
#define SIOCMCTPSETPIDMAP _IOW('m', 1, struct PidMapReq)
#endif
#ifndef SIOCMCTPGETPIDMAP
#define SIOCMCTPGETPIDMAP _IOR('m', 3, struct PidMapBulk)
#endif
#ifndef SIOCMCTPDELPIDMAP
#define SIOCMCTPDELPIDMAP _IOW('m', 2, uint8_t)
#endif

int MCTPI3CDiscovery::deleteAllPidMappings()
{
    info("Deleting all kernel PID mappings");

    int sock = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        error("deleteAllPidMappings: socket creation failed");
        return -1;
    }

    // Step 1: Get all PID mappings from kernel
    PidMapBulk bulk = {};
    bulk.count = maxPidMapEntries;

    if (ioctl(sock, SIOCMCTPGETPIDMAP, &bulk) < 0)
    {
        error("deleteAllPidMappings: ioctl SIOCMCTPGETPIDMAP failed");
        close(sock);
        return -1;
    }

    if (bulk.count == 0)
    {
        debug("deleteAllPidMappings: no PID mappings found");
        close(sock);
        return 0;
    }

    info("deleteAllPidMappings: found {COUNT} PID mappings to delete", "COUNT",
         bulk.count);

    // Step 2: Delete each mapping
    int deleted = 0;
    for (uint32_t i = 0; i < bulk.count; ++i)
    {
        uint8_t eid = bulk.entries[i].eid;
        if (eid < kMctpEidRangeStart || eid == kMctpEidBroadcast)
        {
            continue;
        }

        if (ioctl(sock, SIOCMCTPDELPIDMAP, &eid) < 0)
        {
            warning("deleteAllPidMappings: failed to delete PID mapping "
                    "for EID {EID}",
                    "EID", lg2::hex, eid);
        }
        else
        {
            debug("deleteAllPidMappings: deleted PID mapping for EID {EID}",
                  "EID", lg2::hex, eid);
            deleted++;
        }
    }

    close(sock);
    info("deleteAllPidMappings: deleted {COUNT} PID mappings", "COUNT",
         deleted);
    return deleted;
}

// ----------------------------------------------------------------
// addPidMapping — add a single kernel PID-to-EID mapping
// ----------------------------------------------------------------

int MCTPI3CDiscovery::addPidMapping(uint8_t eid, const uint8_t* pid)
{
    int sock = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        error("addPidMapping: socket creation failed");
        return -1;
    }

    PidMapReq req = {};
    req.eid = eid;
    memcpy(req.pid, pid, pidDataSize);

    if (ioctl(sock, SIOCMCTPSETPIDMAP, &req) < 0)
    {
        close(sock);
        return -1;
    }

    info("addPidMapping: EID {EID} -> PID "
         "{P0}:{P1}:{P2}:{P3}:{P4}:{P5}",
         "EID", lg2::hex, eid, "P0", lg2::hex, req.pid[0], "P1", lg2::hex,
         req.pid[1], "P2", lg2::hex, req.pid[2], "P3", lg2::hex, req.pid[3],
         "P4", lg2::hex, req.pid[4], "P5", lg2::hex, req.pid[5]);

    close(sock);
    return 0;
}
