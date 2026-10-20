#pragma once

#include "MCTPConstants.hpp"
#include "MCTPDiscovery.hpp"
#include "MCTPReactorConfig.hpp"

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// I3C provisioned ID is 6 bytes (48 bits) per MIPI I3C spec.
inline constexpr std::size_t kI3cPidLen = 6;

/**
 * @brief MCTP device descriptor used by the daemon discovery class.
 *
 * Mirrors the MctpDevice from mctp-i3c-daemon for use in the reactor
 * framework. Contains device identification, addressing, and state fields.
 */
struct I3CMctpDevice
{
    int busNumber = 0;
    std::string name;
    std::string pidMask;
    std::string role;
    std::string physicalLinkName;
    // BMC I3C physical mode; used only for scanning and interface naming.
    bool isTarget = false;
    bool isMctpBridge = false;
    bool deviceRegistered = false;
    bool detected = false;
    uint8_t eid = 0;
    uint8_t hwAddr[kI3cPidLen] = {};
    uint8_t dynamicAddr = 0;
    uint8_t localEid = 0;
    uint8_t configuredOwnEID = 0;
    std::vector<uint8_t> eidPool; // {startEID, poolSize} for bus-owner role
    bool isSecondaryBusOwner = false;
    // Endpoint-role shared-EID / pool-request (mirrors oks
    // MctpMultiValConfig.UseSharedEID / RequestEIDPool).
    bool useSharedEID = false;
    uint8_t requestEIDPool = 0;
    // Optional per-device MCTP network ID; 0 = use I3C transport net.
    uint16_t netOverride = 0;
    // When true, skip AssignEndpoint; mctpd assigns EID after pool-ID from BO.
    bool waitForPoolId = false;
};

/**
 * @brief I3C Daemon-compatible discovery with power reset handling.
 *
 * Ports the daemon's pwrResetHandler and all invoked functions into the
 * reactor framework as member functions. Manages device registry cleanup,
 * endpoint deletion, bus re-discovery, and endpoint registration when a
 * platform power reset event is detected.
 */
class MCTPI3CDiscovery : public MCTPDiscovery
{
  public:
    MCTPI3CDiscovery(const std::shared_ptr<sdbusplus::asio::connection>& bus,
                     const I3CDiscoveryConfig& config);
    ~MCTPI3CDiscovery() override = default;

    void run() override;
    std::string_view name() const override
    {
        return "I3CDaemon";
    }

    /// Trigger pwrResetHandler() once per power cycle. Called by both the
    /// HostOn and PlatformReset hooks; the second call within the same
    /// power cycle is suppressed by the internal latch.
    void onHostOn(uint8_t /*hostIndex*/) override
    {
        triggerPwrReset("HostOn");
    }
    void onPlatformReset() override
    {
        triggerPwrReset("PlatformReset");
    }
    /// Clear the once-per-cycle latch so the next power-on re-arms.
    void onHostOff(uint8_t /*hostIndex*/) override
    {
        resetTriggered = false;
    }

    /// Main handler for platform reset events
    void pwrResetHandler();

  private:
    /// Once-per-power-cycle latch; cleared on HostOff.
    bool resetTriggered = false;
    void triggerPwrReset(const char* source);

    // Device registry
    std::vector<I3CMctpDevice> devices;

    // State flags (ported from daemon globals)
    bool pwrResetInProgress = false;
    int mctpI3cNet = DEFAULT_I3C_NET;
    uint32_t mtu = DEFAULT_MTU;
    bool waitForEidEnabled = false;

    // --- Helper functions ported from daemon ---

    /// Validate EID is within valid range (0x08??xFE)
    bool isEidValid(uint8_t eid) const;

    /// Check device status via sysfs status file (retries up to 3 times)
    bool checkDeviceStatus(const std::filesystem::path& devicePath) const;

    /// Scan I3C buses and update detection status (ASPEED_SOC path)
    void scanI3CBuses();

    /// Trigger I3C kernel device discovery on buses
    void discoverI3CDevices();

    /// Discover MCTP endpoints behind I3C hubs by writing 0xff to each
    /// hub's sysfs `detect` file. Hubs are identified by `dcr == 0xc2`.
    /// Performs an initial bounded retry loop to wait for hubs to appear,
    /// then re-issues the broadcast a few additional times with a delay
    /// to give late-arriving downstream endpoints a chance to respond.
    /// Ported from oks I3CTopmostBusOwner::discoverPhysicalEndpoints
    /// (sysfs-only; no D-Bus I3CDeviceManager dependency).
    void discoverPhysicalEndpoints();

    /// Convert 6-byte hardware address to hex string
    std::string convertHwAddrToPidString(
        const uint8_t hwAddr[kI3cPidLen]) const;

    /// Normalize PID string by removing leading zeros
    static std::string normalizePID(const std::string& pid);

    /// Read dynamic address from sysfs file
    static std::string readDynamicAddress(const std::filesystem::path& path);

    /// Find device dynamic address by PID in sysfs, mark device detected
    std::string findDynamicAddressByPID(const std::string& pid,
                                        I3CMctpDevice& device);

    /// Setup MCTP local interfaces and addresses for all devices
    int setupLocalInterfaces();

    /// Delete all kernel PID-to-EID mappings
    int deleteAllPidMappings();

    /// Add a kernel PID-to-EID mapping
    int addPidMapping(uint8_t eid, const uint8_t* pid);

    /// Run device discovery workflow (endpoint discovery + notify)
    int deviceDiscoveryWorkflow();

    /// Send DiscoveryNotify to codeconstruct mctpd via D-Bus
    bool sendDiscoveryNotify(const I3CMctpDevice& device);

    /// Bus-owner role: call mctpd AssignEndpoint(hwAddr) over D-Bus.
    /// Mirrors oks TopmostBusOwner::discoveryTask endpoint-assign step.
    bool assignEndpoint(const I3CMctpDevice& device);

    /// Resolve effective MCTP network ID for a device (per-device override
    /// when non-zero, otherwise the I3C transport-wide net).
    uint16_t deviceNet(const I3CMctpDevice& device) const
    {
        return device.netOverride != 0 ? device.netOverride
                                       : static_cast<uint16_t>(mctpI3cNet);
    }

    /// Push EIDPool {startEID, poolSize} to mctpd Network1 property.
    /// Mirrors oks MCTPDWrapperImpl::setEIDPool. Pass {0, 0} to clear.
    bool setEIDPoolDbus(uint16_t network, uint8_t startEID,
                       uint8_t poolSize);

    /// Push RequestPoolSize to mctpd Interface1 property.
    /// Mirrors oks MCTPDWrapperImpl::setRequiredPoolSize. Endpoint role
    /// only; tells the upstream bus owner how many EIDs we need.
    bool setRequestPoolSizeDbus(const std::string& physicalLink,
                                uint8_t poolSize);

    /// Endpoint role: poll mctpd until a non-null local EID appears on
    /// the device's network (assigned by the bus owner via SetEID), then
    /// install it on this interface. Mirrors oks waitForSharedEID +
    /// "copy network local EID to endpoint interface" logic.
    /// Returns the assigned EID, or 0 on timeout.
    uint8_t waitForAssignedEID(const I3CMctpDevice& device,
                               std::chrono::seconds timeout);

    /// Resolve kernel/D-Bus interface name for a device
    /// (physicalLinkName override -> mctpi3cN -> findI3CTargetInterface()).
    std::string resolveInterfaceName(const I3CMctpDevice& device) const;

    /// Find actual kernel interface name matching 'mctpi3c-target*' or
    /// 'mctpi3c_target*' by scanning /sys/class/net/ with retry
    std::string findI3CTargetInterface() const;

    // PID size for neighbour entries
    static constexpr int pidSize = 6;
};
