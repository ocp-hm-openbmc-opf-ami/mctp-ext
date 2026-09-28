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

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

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
    MCTPDiscovery(bus), mctpI3cNet(config.i3cNet), mtu(config.mtu),
    waitForEidEnabled(config.waitForEid)
{
    // Populate device registry from config
    for (const auto& dev : config.devices)
    {
        I3CMctpDevice device;
        device.busNumber = dev.busNum;
        device.name = dev.name;
        device.pidMask = dev.pidMask;
        device.role = dev.role;
        device.physicalLinkName = dev.physicalLinkName;
        device.isTarget = dev.isTarget;
        device.isSecondaryBusOwner = dev.isSecondaryBusOwner;
        device.configuredOwnEID = dev.ownEID;
        device.eidPool = dev.eidPool;
        device.useSharedEID = dev.useSharedEID;
        device.requestEIDPool = dev.requestEIDPool;
        device.netOverride = dev.netOverride;
        device.waitForPoolId = dev.waitForPoolId;

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

        info("Registered I3C device: name={NAME}, bus={BUS}, "
             "isTarget={TGT}, role={ROLE}, pidMask={MASK}",
             "NAME", device.name, "BUS", device.busNumber, "TGT",
             device.isTarget, "ROLE", device.role, "MASK", device.pidMask);
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

        // Reset device detection status after power reset (all devices)
        device.detected = false;
        device.dynamicAddr = 0;
        info("Reset detection status for device: {NAME} (isTarget={TGT})",
             "NAME", device.name, "TGT", device.isTarget);
    }

    /// @brief BHS Changes
#ifdef NUVOTON_SDK
    // Select the rescan mechanism by probing what the kernel i3c driver
    // exposes, instead of keying on a SoC string:
    //   - ASPEED masters expose a per-bus "rescan" sysfs file -> scanI3CBuses()
    //   - nuvoton exposes "/sys/bus/i3c/devices/i3c-N/discover"
    //     -> discoverI3CDevices() + PID-based redetection.
    bool hasDiscoverFile = false;
    {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(
                 "/sys/bus/i3c/devices",
                 fs::directory_options::skip_permission_denied, ec))
        {
            if (fs::exists(entry.path() / "discover"))
            {
                hasDiscoverFile = true;
                break;
            }
        }
    }

    if (!hasDiscoverFile)
    {
#endif
        /// Scan I3C bus and update hardware and dynamic address
        scanI3CBuses();

        // Log detection status for all devices
        for (const auto& device : devices)
        {
            if (!device.isTarget)
            {
                info("Controller device {NAME} detection status: {STATUS}",
                     "NAME", device.name, "STATUS",
                     device.detected ? "DETECTED" : "NOT DETECTED");
            }
        }
#ifdef NUVOTON_SDK
    }
    else
    {
        discoverI3CDevices();
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
#endif

    // Reconfigure local interfaces after power reset
    setupLocalInterfaces();

    // Discover endpoints behind any I3C hubs (broadcast detect 0xff)
    discoverPhysicalEndpoints();

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

    // Detect isTarget=True devices (BMC is I3C target, remote device is bus
    // owner) by checking for a mctpi3c-target interface in /sys/class/net/.
    // The controller-mode platform path does not exist for target-mode buses.
    for (auto& dev : devices)
    {
        if (!dev.isTarget)
            continue;

        std::string targetIface = findI3CTargetInterface();
        if (!targetIface.empty())
        {
            dev.detected = true;
            info("Detected target-mode device: {NAME} via interface {IFACE}",
                 "NAME", dev.name, "IFACE", targetIface);
        }
        else
        {
            warning("Target-mode device {NAME} not detected: "
                    "no mctpi3c-target interface found",
                    "NAME", dev.name);
        }
    }

    // Group only controller-mode devices (isTarget=False) per bus for
    // PID-based sysfs scanning.
    std::unordered_map<int, std::vector<I3CMctpDevice*>> controllerBuses;

    for (auto& dev : devices)
    {
        if (!dev.isTarget)
            controllerBuses[dev.busNumber].push_back(&dev);
    }

    // Scan each bus and trigger sysfs rescan and match PIDs with retries
    for (auto& [busNum, devList] : controllerBuses)
    {
        info("Scanning bus {BUS} for {COUNT} devices", "BUS", busNum, "COUNT",
             devList.size());

        // Locate the platform device for this I3C bus by matching the
        // ".i3c<busNum>" suffix under /sys/bus/platform/devices. This is
        // SoC-agnostic: AST2600 (1e7aX000.i3cN), AST2700 (14cXX000.i3cN)
        // and any other SoC all expose the same ".i3cN" naming, so no
        // hardcoded address table or SoC string is needed here.
        const std::string basePath = "/sys/bus/platform/devices";
        const std::string suffix = ".i3c" + std::to_string(busNum);
        std::string platformDevPath;
        for (const auto& entry : fs::directory_iterator(
                 basePath, fs::directory_options::skip_permission_denied))
        {
            const std::string devName = entry.path().filename().string();
            if (devName.size() >= suffix.size() &&
                devName.compare(devName.size() - suffix.size(), suffix.size(),
                                suffix) == 0)
            {
                platformDevPath = entry.path().generic_string();
                break;
            }
        }

        if (platformDevPath.empty())
        {
            error("I3C platform device not found for bus {BUS} "
                  "(no *{SUFFIX} under {BASE})",
                  "BUS", busNum, "SUFFIX", suffix, "BASE", basePath);
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

            // Always capture the i3c bus directory path
            i3cBusPath = busEntry.path().generic_string();

            std::string candidate = i3cBusPath + "/rescan";
            if (fs::exists(candidate))
            {
                rescanFilePath = candidate;
            }
            break;
        }

        if (i3cBusPath.empty())
        {
            error("No i3c bus directory found under {PATH}", "PATH",
                  platformDevPath);
            continue;
        }

        if (rescanFilePath.empty())
        {
            // Target-mode buses have no rescan file — this is expected.
            // We still attempt PID matching below.
            info("Bus {BUS}: no rescan file (target-mode bus), "
                 "i3c bus path={PATH}",
                 "BUS", busNum, "PATH", i3cBusPath);
        }

        if (!rescanFilePath.empty())
        {
            info("Bus {BUS}: rescan file={RESCAN}, i3c bus path={PATH}", "BUS",
                 busNum, "RESCAN", rescanFilePath, "PATH", i3cBusPath);
        }

        // Retry loop: rescan and check devices until all detected or max
        // retries
        bool allDetected = false;
        for (int attempt = 0; attempt < maxRetries && !allDetected; ++attempt)
        {
            debug("Scan attempt {N} for bus {BUS}", "N", attempt + 1, "BUS",
                  busNum);

            // Trigger rescan (only if rescan file exists — controller buses)
            if (!rescanFilePath.empty())
            {
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
            }
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

                    // Regex PID match (matches mctp-setup
                    // findI3CDevicesFromSysfs)
                    if (!dev->pidMask.empty())
                    {
                        try
                        {
                            std::regex pidRegex(dev->pidMask);
                            if (std::regex_match(pidStr, pidRegex))
                            {
                                matched = true;
                            }
                        }
                        catch (const std::regex_error& e)
                        {
                            warning("Invalid PidMask regex '{MASK}' for "
                                    "device {NAME}: {ERR}",
                                    "MASK", dev->pidMask, "NAME", dev->name,
                                    "ERR", std::string(e.what()));
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

                        // Update hwAddr from actual PID when matched by mask
                        if (!dev->pidMask.empty())
                        {
                            for (size_t i = 0; i < kI3cPidLen; i++)
                            {
                                dev->hwAddr[i] =
                                    (devicePid >>
                                     (8 * ((kI3cPidLen - 1) - i))) &
                                    0xFF;
                            }
                        }

                        // Each sysfs PID maps to exactly one logical device.
                        // Stop here so a second device sharing the same
                        // PidMask on this bus is not also bound to this same
                        // PID; it will match the next sysfs entry instead.
                        // Without this break, two devices with an identical
                        // PidMask (e.g. cpu1_iod0/cpu1_iod1, mask ".*21.8")
                        // both receive the first-iterated PID, so both send
                        // the same physaddr to mctpd and one real endpoint is
                        // never assigned.
                        break;
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

#ifdef MCTP_I3C_MULTIPLE_BO_WITH_DIFF_EIDS
    deleteAllPidMappings();
#endif

    for (auto& device : devices)
    {
        // Skip undetected devices.
        if (!device.detected)
        {
            info("Skipping interface setup for undetected device: {NAME}",
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

        // Local EID for this link: use the per-device OwnEID when
        // configured (non-zero); otherwise fall back to the default local
        // EID. The remote bus owner (CPU) may still issue the final EID via
        // SetEID after Discovery Notify for endpoint-role links.
        const bool isMctpEndpoint = (device.role == "endpoint");
        const bool isMctpBusOwner = (device.role == "bus-owner");
        device.localEid = device.configuredOwnEID != 0 ? device.configuredOwnEID
                                                       : DEFAULT_LOCAL_EID;

        // Endpoint role with UseSharedEID: do not pre-install a local EID.
        // The bus owner on this network will assign one via SetEID; we
        // copy it onto our interface in deviceDiscoveryWorkflow.
        // When wait-for-EID is disabled, always bring up the local EID
        // immediately using the default local EID.
        const bool deferLocalEid =
            waitForEidEnabled && isMctpEndpoint && device.useSharedEID;

        std::string interfaceName;
        if (!device.physicalLinkName.empty())
        {
            interfaceName = device.physicalLinkName;
        }
        else if (!device.isTarget)
        {
            // Bus owner interface: mctpi3cN (netdev name declared via
            // PhysicalLinkName; this bus-number fallback is used only when
            // the config omits it).
            interfaceName = "mctpi3c" + std::to_string(device.busNumber);
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

        const uint16_t devNet = deviceNet(device);

        if (deferLocalEid)
        {
            info("Deferring local EID install for shared-EID endpoint "
                 "{NAME} on {IFACE} net {NET}",
                 "NAME", device.name, "IFACE", interfaceName, "NET", devNet);
        }
        else if (!ensureInterfaceReady(interfaceName, device.localEid, devNet,
                                       mtu))
        {
            error("Failed to configure local EID {EID} for {NAME}", "EID",
                  lg2::hex, device.localEid, "NAME", device.name);
        }

        // Endpoint-role pool request: tell mctpd to clear our network's
        // EIDPool and tell the upstream BO how many EIDs we need. Mirrors
        // oks Endpoint::discoveryTask: setEIDPool(net,0,0) +
        // setRequiredPoolSize(intf, requiredEIDPoolSize).
        if (isMctpEndpoint && device.requestEIDPool > 0)
        {
            setEIDPoolDbus(devNet, 0, 0);
            setRequestPoolSizeDbus(interfaceName, device.requestEIDPool);
        }

        // Bus-owner role with a configured downstream EIDPool: publish it
        // to mctpd Network1.EIDPool. Mirrors oks TopmostBusOwner::
        // discoveryTask -> setEIDPool(net, eidPool.front(), eidPool.back()).
        if (isMctpBusOwner && device.eidPool.size() >= 2)
        {
            setEIDPoolDbus(devNet, device.eidPool.front(),
                           device.eidPool.back());
        }

        // Add PID mapping for endpoint devices (remote is bus owner).
        // Only meaningful when BMC is the MCTP bus owner on this link;
        // for role=="endpoint" the remote bus owner's EID is not yet
        // known (it is learned via SetEID after Discovery Notify), so
        // skip the mapping here and let the kernel/mctpd populate it
        // once the handshake completes.
#ifdef MCTP_I3C_MULTIPLE_BO_WITH_DIFF_EIDS
        if (isMctpEndpoint && device.detected)
        {
            if (addPidMapping(device.localEid, device.hwAddr) < 0)
            {
                warning("Failed to add PID mapping for {NAME} EID {EID}",
                        "NAME", device.name, "EID", lg2::hex, device.localEid);
            }
        }
#endif
    }
    return 0;
}

// ----------------------------------------------------------------
// discoverPhysicalEndpoints (ported from oks I3CTopmostBusOwner)
// ----------------------------------------------------------------

void MCTPI3CDiscovery::discoverPhysicalEndpoints()
{
    constexpr std::size_t initialRetries = 20;
    constexpr std::size_t additionalHubProbes = 10;
    constexpr auto retryDelay = std::chrono::seconds(1);
    constexpr auto hubProbeDelay = std::chrono::seconds(3);
    static constexpr std::uint8_t kI3CHubDcr = 0xc2;
    const fs::path sysfsRoot = "/sys/bus/i3c/devices";

    // Identify the buses we own (controller-side configured devices).
    std::set<int> ownedBuses;
    for (const auto& dev : devices)
    {
        if (!dev.isTarget)
        {
            ownedBuses.insert(dev.busNumber);
        }
    }
    if (ownedBuses.empty())
    {
        return;
    }

    auto readByteFile = [](const fs::path& p, std::uint64_t& out) -> bool {
        std::ifstream f(p);
        if (!f.is_open())
        {
            return false;
        }
        std::string s;
        f >> s;
        if (s.empty())
        {
            return false;
        }
        try
        {
            out = std::stoull(s, nullptr, 0);
        }
        catch (const std::exception&)
        {
            return false;
        }
        return true;
    };

    auto broadcastHubDetect = [](const fs::path& hubPath) {
        std::ofstream detectFile(hubPath / "detect");
        if (!detectFile.is_open())
        {
            warning("Failed to open detect file for hub at {PATH}", "PATH",
                    hubPath.string());
            return;
        }
        detectFile << "0xff\n";
    };

    auto scanForHubs = [&](std::vector<fs::path>& hubsOut) {
        if (!fs::exists(sysfsRoot))
        {
            return;
        }
        for (const auto& entry : fs::directory_iterator(sysfsRoot))
        {
            if (!entry.is_directory())
            {
                continue;
            }
            const auto& name = entry.path().filename().string();
            // I3C device dirs are named "<bus>-<pid>".
            auto dash = name.find('-');
            if (dash == std::string::npos)
            {
                continue;
            }
            int bus = 0;
            try
            {
                bus = std::stoi(name.substr(0, dash));
            }
            catch (const std::exception&)
            {
                continue;
            }
            if (!ownedBuses.contains(bus))
            {
                continue;
            }
            std::uint64_t dcr = 0;
            if (!readByteFile(entry.path() / "dcr", dcr))
            {
                continue;
            }
            if ((dcr & 0xff) == kI3CHubDcr)
            {
                hubsOut.push_back(entry.path());
            }
        }
    };

    std::vector<fs::path> knownHubs;
    for (std::size_t r = 0; r < initialRetries; ++r)
    {
        scanForHubs(knownHubs);
        if (!knownHubs.empty())
        {
            for (const auto& hub : knownHubs)
            {
                info("Filling devices behind I3C hub at {PATH}", "PATH",
                     hub.string());
                broadcastHubDetect(hub);
            }
            break;
        }
        sleep(static_cast<unsigned>(retryDelay.count()));
    }

    if (knownHubs.empty())
    {
        debug("No I3C hubs found on owned buses; skipping hub re-probe");
        return;
    }

    // Re-issue the hub `detect 0xff` broadcast to give late endpoints a
    // chance to respond with a Discovery Notify.
    for (std::size_t i = 0; i < additionalHubProbes; ++i)
    {
        sleep(static_cast<unsigned>(hubProbeDelay.count()));
        for (const auto& hub : knownHubs)
        {
            debug("Re-probing I3C hub at {PATH} (iteration {I})", "PATH",
                  hub.string(), "I", i);
            broadcastHubDetect(hub);
        }
    }
}

// ----------------------------------------------------------------
// deviceDiscoveryWorkflow
// ----------------------------------------------------------------

int MCTPI3CDiscovery::deviceDiscoveryWorkflow()
{
    info("Initiating device discovery workflow");

    // Dispatch per-device based on the BMC's role on this link:
    //   role == "bus-owner" -> BMC is the I3C bus owner. Mirror the oks
    //     TopmostBusOwner control flow by actively assigning an EID to the
    //     detected downstream endpoint via mctpd AssignEndpoint.
    //   role == "endpoint" (or anything else) -> BMC is endpoint/bridge on
    //     this link, so notify the remote bus owner with DiscoveryNotify.
    auto dispatch = [&](I3CMctpDevice& device) {
        if (device.role == "bus-owner")
        {
            if (!device.waitForPoolId)
            {
                if (!assignEndpoint(device))
                {
                    error("AssignEndpoint failed for {NAME}", "NAME",
                          device.name);
                }
            }
            // else: waitForPoolId=true — mctpd assigns EID after pool-ID from
            // BO
        }
        else
        {
            if (!sendDiscoveryNotify(device))
            {
                error("sendDiscoveryNotify failed for {NAME}", "NAME",
                      device.name);
            }
            // After Discovery Notify, wait for the upstream bus owner to
            // assign us an EID via SetEID. For UseSharedEID endpoints
            // this is required to bring the interface online; for the
            // standalone case it is best-effort confirmation.
            if (waitForEidEnabled)
            {
                constexpr auto setEidTimeout = std::chrono::seconds(60);
                uint8_t assigned = waitForAssignedEID(device, setEidTimeout);
                if (assigned != 0)
                {
                    info("Endpoint {NAME} assigned EID {EID} by upstream "
                         "bus owner",
                         "NAME", device.name, "EID", lg2::hex, assigned);
                }
                else if (device.useSharedEID || device.requestEIDPool > 0)
                {
                    warning("Endpoint {NAME} did not receive an EID from "
                            "the upstream bus owner within {SEC}s",
                            "NAME", device.name, "SEC",
                            static_cast<int>(setEidTimeout.count()));
                }
            }
        }
    };

    for (auto& device : devices)
    {
        // Skip ANY device not detected during bus scanning
        if (!device.detected)
        {
            info("Skipping device discovery for undetected "
                 "device: {NAME} (isTarget={TGT}, PidMask '{MASK}')",
                 "NAME", device.name, "TGT", device.isTarget, "MASK",
                 device.pidMask);
            continue;
        }

        // Secondary bus owners are processed after a delay below so the
        // primary bus owner has time to publish the network first.
        if (device.isSecondaryBusOwner)
        {
            continue;
        }

        dispatch(device);
    }

    bool hasSecondary = false;
    for (const auto& device : devices)
    {
        if (device.isSecondaryBusOwner && device.detected)
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
            if (!device.isSecondaryBusOwner)
            {
                continue;
            }
            if (!device.detected)
            {
                continue;
            }
            dispatch(device);
        }
    }

    return 0;
}

// ----------------------------------------------------------------
// resolveInterfaceName — pick kernel/D-Bus interface name per device
// ----------------------------------------------------------------

std::string MCTPI3CDiscovery::resolveInterfaceName(
    const I3CMctpDevice& device) const
{
    if (!device.physicalLinkName.empty())
    {
        return device.physicalLinkName;
    }
    if (!device.isTarget)
    {
        return "mctpi3c" + std::to_string(device.busNumber);
    }
    return findI3CTargetInterface();
}

// ----------------------------------------------------------------
// sendDiscoveryNotify — D-Bus call to codeconstruct mctpd
// ----------------------------------------------------------------

bool MCTPI3CDiscovery::sendDiscoveryNotify(const I3CMctpDevice& device)
{
    // Build the 6-byte PID vector from hwAddr
    std::vector<std::uint8_t> formattedPid(device.hwAddr,
                                           device.hwAddr + pidSize);

    // I3CTarget controls only physical interface-name resolution.
    std::string interfaceName = resolveInterfaceName(device);

    if (interfaceName.empty())
    {
        error("sendDiscoveryNotify: no I3C interface found for device {NAME}",
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

    // Retry loop for DiscoveryNotify D-Bus call
    const int maxAttempts = 3;
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
// assignEndpoint — bus-owner role: mctpd AssignEndpoint(hwAddr) D-Bus
// Mirrors oks MCTPDWrapperImpl::assignEndpoint, called per-endpoint from
// TopmostBusOwner::discoveryTask after physical-endpoint discovery.
// ----------------------------------------------------------------

bool MCTPI3CDiscovery::assignEndpoint(const I3CMctpDevice& device)
{
    std::vector<std::uint8_t> hwAddr(device.hwAddr, device.hwAddr + pidSize);

    std::string interfaceName = resolveInterfaceName(device);
    if (interfaceName.empty())
    {
        error("AssignEndpoint: no interface found for device {NAME}", "NAME",
              device.name);
        return false;
    }

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
                mctp::dbus::busOwnerInterface.data(), "AssignEndpoint");
            method.append(hwAddr);
            auto reply = bus->call(method);

            std::uint8_t eid = 0;
            std::int32_t net = 0;
            std::string objPath;
            bool added = false;
            reply.read(eid, net, objPath, added);

            info("AssignEndpoint succeeded for {NAME} (interface: {IFACE}) "
                 "EID={EID} net={NET} path={PATH} added={ADDED}",
                 "NAME", device.name, "IFACE", interfaceName, "EID", lg2::hex,
                 eid, "NET", net, "PATH", objPath, "ADDED",
                 added ? "true" : "false");
            return true;
        }
        catch (const std::exception& e)
        {
            if (attempt < maxAttempts - 1)
            {
                debug("AssignEndpoint attempt {N} failed for {NAME}, "
                      "retrying...",
                      "N", attempt + 1, "NAME", device.name);
                sleep(1);
            }
            else
            {
                error("AssignEndpoint failed for {NAME} after {N} "
                      "attempts: {ERR}",
                      "NAME", device.name, "N", maxAttempts, "ERR", e.what());
            }
        }
    }
    return false;
}

// ----------------------------------------------------------------
// setEIDPoolDbus — push Network1.EIDPool property to mctpd.
// Mirrors oks MCTPDWrapperImpl::setEIDPool. Pass {0,0} to clear (used
// by endpoint-role devices that are about to request a pool).
// ----------------------------------------------------------------

bool MCTPI3CDiscovery::setEIDPoolDbus(uint16_t network, uint8_t startEID,
                                      uint8_t poolSize)
{
    std::string objPath =
        std::string(mctp::dbus::networksPath) + "/" + std::to_string(network);
    std::vector<uint8_t> pool = {startEID, poolSize};

    constexpr int maxAttempts = 3;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        try
        {
            auto method = bus->new_method_call(
                mctp::dbus::service.data(), objPath.c_str(),
                "org.freedesktop.DBus.Properties", "Set");
            method.append(std::string("au.com.codeconstruct.MCTP.Network1"),
                          std::string("EIDPool"),
                          std::variant<std::vector<uint8_t>>(pool));
            bus->call(method);
            info("setEIDPool net={NET} start={START} size={SIZE}", "NET",
                 network, "START", lg2::hex, startEID, "SIZE", lg2::hex,
                 poolSize);
            return true;
        }
        catch (const std::exception& e)
        {
            if (attempt < maxAttempts - 1)
            {
                debug("setEIDPool attempt {N} failed (net={NET}), retrying",
                      "N", attempt + 1, "NET", network);
                sleep(1);
            }
            else
            {
                error("setEIDPool failed on net {NET}: {ERR}", "NET", network,
                      "ERR", e.what());
            }
        }
    }
    return false;
}

// ----------------------------------------------------------------
// setRequestPoolSizeDbus — push Interface1.RequestPoolSize to mctpd.
// Mirrors oks MCTPDWrapperImpl::setRequiredPoolSize. Endpoint-role only;
// tells the upstream bus owner how many EIDs we need allocated.
// ----------------------------------------------------------------

bool MCTPI3CDiscovery::setRequestPoolSizeDbus(const std::string& physicalLink,
                                              uint8_t poolSize)
{
    std::string dbusIface = physicalLink;
    for (auto& c : dbusIface)
    {
        if (c == '-')
            c = '_';
    }
    std::string objPath =
        std::string(mctp::dbus::basePath) + "/interfaces/" + dbusIface;

    constexpr int maxAttempts = 3;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        try
        {
            auto method = bus->new_method_call(
                mctp::dbus::service.data(), objPath.c_str(),
                "org.freedesktop.DBus.Properties", "Set");
            method.append(std::string(mctp::dbus::interfaceIface.data()),
                          std::string("RequestPoolSize"),
                          std::variant<uint8_t>(poolSize));
            bus->call(method);
            info("setRequestPoolSize iface={IFACE} size={SIZE}", "IFACE",
                 physicalLink, "SIZE", lg2::hex, poolSize);
            return true;
        }
        catch (const std::exception& e)
        {
            if (attempt < maxAttempts - 1)
            {
                debug("setRequestPoolSize attempt {N} failed ({IFACE}), "
                      "retrying",
                      "N", attempt + 1, "IFACE", physicalLink);
                sleep(1);
            }
            else
            {
                error("setRequestPoolSize failed for {IFACE}: {ERR}", "IFACE",
                      physicalLink, "ERR", e.what());
            }
        }
    }
    return false;
}

// ----------------------------------------------------------------
// waitForAssignedEID — endpoint-role poll for SetEID from upstream BO.
// Reads mctpd Network1.LocalEIDs (filters out the null EID), and on
// the first valid entry installs it on this device's interface via
// ensureInterfaceReady. Mirrors oks Endpoint::waitForSetEIDFromBusOwner
// + the "copy network local EID to endpoint interface" step.
// ----------------------------------------------------------------

uint8_t MCTPI3CDiscovery::waitForAssignedEID(const I3CMctpDevice& device,
                                             std::chrono::seconds timeout)
{
    const uint16_t network = deviceNet(device);
    std::string netPath =
        std::string(mctp::dbus::networksPath) + "/" + std::to_string(network);
    const std::string interfaceName = resolveInterfaceName(device);

    // Snapshot the current LocalEIDs before polling so we can distinguish
    // pre-existing EIDs (e.g. from bus-owner interfaces on the same
    // network) from genuinely NEW EIDs assigned by the upstream bus owner
    // via SetEID.
    std::set<uint8_t> baselineEids;
    try
    {
        auto snapMethod =
            bus->new_method_call(mctp::dbus::service.data(), netPath.c_str(),
                                 "org.freedesktop.DBus.Properties", "Get");
        snapMethod.append(std::string("au.com.codeconstruct.MCTP.Network1"),
                          std::string("LocalEIDs"));
        auto snapReply = bus->call(snapMethod);

        std::variant<std::vector<uint8_t>> sv;
        snapReply.read(sv);
        for (uint8_t e : std::get<std::vector<uint8_t>>(sv))
        {
            if (e != 0 && e != kMctpEidBroadcast)
            {
                baselineEids.insert(e);
            }
        }
        debug("waitForAssignedEID: baseline EIDs on net {NET}: {CNT} entries",
              "NET", network, "CNT", baselineEids.size());
    }
    catch (const std::exception& e)
    {
        debug("waitForAssignedEID: failed to read baseline EIDs: {ERR}", "ERR",
              e.what());
    }

    auto start = std::chrono::steady_clock::now();
    while ((std::chrono::steady_clock::now() - start) < timeout)
    {
        try
        {
            auto method = bus->new_method_call(
                mctp::dbus::service.data(), netPath.c_str(),
                "org.freedesktop.DBus.Properties", "Get");
            method.append(std::string("au.com.codeconstruct.MCTP.Network1"),
                          std::string("LocalEIDs"));
            auto reply = bus->call(method);

            std::variant<std::vector<uint8_t>> v;
            reply.read(v);
            const auto& eids = std::get<std::vector<uint8_t>>(v);

            for (uint8_t eid : eids)
            {
                if (eid == 0 || eid == kMctpEidBroadcast)
                {
                    continue;
                }
                // Skip EIDs that already existed before discovery —
                // they belong to other interfaces (e.g. bus-owner on
                // the same network) and are not assigned by the
                // upstream bus owner.
                if (baselineEids.count(eid))
                {
                    continue;
                }
                // Install on our interface (idempotent).
                if (!interfaceName.empty())
                {
                    if (!ensureInterfaceReady(interfaceName, eid, network, mtu))
                    {
                        warning("waitForAssignedEID: failed to install "
                                "EID {EID} on {IFACE}",
                                "EID", lg2::hex, eid, "IFACE", interfaceName);
                    }
                }
                return eid;
            }
        }
        catch (const std::exception& e)
        {
            debug("waitForAssignedEID poll error on net {NET}: {ERR}", "NET",
                  network, "ERR", e.what());
        }
        sleep(1);
    }
    return 0;
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
