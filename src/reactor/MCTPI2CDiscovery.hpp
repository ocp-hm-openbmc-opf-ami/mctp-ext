#pragma once

#include "config.h"

#include "MCTPConstants.hpp"
#include "MCTPDiscovery.hpp"
#include "MCTPReactorConfig.hpp"

#include <sdbusplus/asio/connection.hpp>

#include <format>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class MCTPReactor;

/**
 * @brief Enumeration for I2C Mux idle modes
 */

/**
 * @brief Structure to hold MCTP neighbor information
 */
struct MCTPNeighbor
{
    std::uint8_t eid = 0;      // Endpoint ID
    std::string device;        // Device name (e.g., "mctpi2c1")
    std::uint8_t physAddr = 0; // Physical address (I2C address)
};

class MCTPI2CDiscovery : public MCTPDiscovery
{
  public:
    MCTPI2CDiscovery(const std::shared_ptr<sdbusplus::asio::connection>& bus,
                     const I2CDiscoveryConfig& config);
    ~MCTPI2CDiscovery() override = default;

    void run() override;
    std::string_view name() const override
    {
        return "I2C";
    }

    /// Reset ARP state for fresh discovery on host power-on.
    void onHostOn() override
    {
        resetARPState();
    }

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
    void setReactor(const std::shared_ptr<MCTPReactor>& r)
    {
        reactor = r;
    }
#endif

    /**
     * @brief Reset ARP state when host state changes
     * Clears next ARP address and processed buses for fresh discovery on boot
     */
    void resetARPState()
    {
        nextArpAddress = arpStartAddress;
        processedBuses.clear();
        staleArpAddresses.clear();
    }

  private:
    const I2CDiscoveryConfig& config;
    std::uint16_t i2cNet = 1;

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
    std::shared_ptr<MCTPReactor> reactor;
    void manageDeviceViaReactor(std::uint8_t busNum, std::uint8_t addr,
                                const std::string& ifaceName);
#endif

    // Forbidden I2C addresses that should never be assigned
    static const std::set<std::string> forbiddenAddresses;

    // Default whitelist: "12 13 1d 32"
    std::set<std::string> whitelist = {"12", "13", "1d", "32"};

    static constexpr std::uint8_t minBusNum = 16;
    static constexpr std::uint32_t i2cDefaultMtu = 254;

    /**
     * @brief Busy-file path used to serialise long-running I2C discovery.
     *
     * PROTOCOL — any module that drives the I2C bus for an extended period
     * (firmware update, raw scan, test tool, etc.) MUST follow this contract
     * to prevent concurrent access races with MCTPI2CDiscovery::run().
     *
     * ACQUIRING the lock (before starting the long task):
     * -------------------------------------------------------
     *   int fd = open(busyFilePath.data(),
     *                 O_CREAT | O_EXCL | O_WRONLY, 0644);
     *   if (fd < 0) {
     *       if (errno == EEXIST)
     *           // Another task owns the bus — abort or retry later.
     *       else
     *           // Unexpected error — log and abort.
     *       return;
     *   }
     *   close(fd);   // File creation is the lock; fd is not kept open.
     *
     * Rules:
     *   1. Use O_CREAT | O_EXCL together — this is an atomic
     *      create-only-if-absent operation; never use exists() + create()
     *      as separate steps (TOCTOU race).
     *   2. Do NOT hold the fd open. The presence of the file is the mutex;
     *      the fd can be closed immediately after creation.
     *   3. Always remove the file when the task finishes — both on the
     *      success and every error path:
     *          std::filesystem::remove(busyFilePath);
     *      Use RAII or a try/catch to guarantee cleanup on exceptions.
     *   4. The lock is advisory: all cooperating callers must check it.
     *      It does NOT prevent kernel-level I2C access.
     *   5. Never nest locks.  If your code already owns the file, do not
     *      attempt to re-create it.
     *
     * RELEASING the lock (after the long task completes or fails):
     * -------------------------------------------------------
     *   std::filesystem::remove(busyFilePath);  // idempotent on missing file
     */
    static constexpr std::string_view busyFilePath =
        "/var/run/mctp_i2c_bus_busy";
    std::set<std::uint8_t>
        processedBuses;  // Buses that have had Prepare ARP sent
    std::uint8_t nextArpAddress =
        arpStartAddress; // Current next ARP address (in-memory)
    static constexpr std::uint8_t arpDefaultAddress = 0x61;
    static constexpr std::uint8_t arpStartAddress = 0x13; // 19 in decimal

    // Addresses that ARP has moved devices AWAY from (stale in this cycle)
    std::set<std::string> staleArpAddresses;

    // Addresses that ARP already handled this cycle (avoid Phase 3
    // re-assignment)
    std::set<std::string> arpAssignedThisCycle;

    // Mux idle mode tracking
    void ensureMuxIdleMode();

    /**
     * @brief Enhanced I2C address probing with EEPROM support
     * Uses different probe methods based on address type (like SMBusBinding)
     * @param fd File descriptor for the I2C port
     * @param addr I2C address to probe
     * @return true if address responds
     */
    bool probeI2CAddress(int fd, std::uint8_t addr);

    /**
     * @brief Check if address is an EEPROM range (requires special probing)
     * @param addr I2C address
     * @return true if address is in EEPROM range
     */
    bool isEEPROMAddress(std::uint8_t addr) const;

    /**
     * @brief Get list of responding I2C addresses in a range via ioctl
     * @param busNum I2C bus number
     * @param startAddr Start address (e.g., 0x08)
     * @param endAddr End address (e.g., 0x77)
     * @return Vector of responding addresses
     */
    std::vector<std::uint8_t> scanI2CRange(
        std::uint8_t busNum, std::uint8_t startAddr, std::uint8_t endAddr);

    /**
     * @brief Get MCTP neighbors using netlink API
     */
    std::vector<MCTPNeighbor> getNeighborsViaNetlink();

    /**
     * @brief Check if a neighbor exists using netlink
     */
    bool neighborExistsViaNetlink(const std::string& hexAddr,
                                  const std::string& ifname);

    /**
     * @brief Phase 3: Dynamic ARP-based device assignment
     */
    void performARPAssignment();

    /**
     * @brief Check if bus has been processed by ARP
     */
    bool isBusARPProcessed(std::uint8_t busNum);

    /**
     * @brief Mark bus as ARP processed
     */
    void markBusARPProcessed(std::uint8_t busNum);

    /**
     * @brief Check if address is in forbidden list
     */
    bool isForbidden(const std::string& hexAddr) const;

    /**
     * @brief Check if address is in the whitelist
     */
    bool isInWhitelist(const std::string& hexAddr) const;

    /**
     * @brief Check if device with bus and address exists in D-Bus
     */
    bool isDeviceExistsInDBus(std::uint8_t busNum, const std::string& hexAddr);

    /**
     * @brief Phase 1: Validate existing routes
     */
    void validateExistingRoutes();

    /**
     * @brief Phase 2: Scan for new devices
     */
    void scanForNewDevices();

    /**
     * @brief Scan configured device list and assign endpoints for undiscovered
     * devices
     */
    void scanConfiguredDevices();

    /**
     * @brief Perform ARP assignment on a single bus
     */
    bool performARPOnBus(std::uint8_t busNum, const std::string& device);

    /**
     * @brief Get next available I2C address for ARP assignment
     */
    std::uint8_t getNextARPAddress();

    /**
     * @brief Set next available I2C address in persistent storage
     */
    void setNextARPAddress(std::uint8_t addr);

    /**
     * @brief Write data to I2C device via ioctl
     */
    bool i2cWrite(std::uint8_t busNum, std::uint8_t addr,
                  const std::vector<std::uint8_t>& buffer);

    /**
     * @brief Write then read from I2C device (combined transaction)
     */
    bool i2cWriteRead(std::uint8_t busNum, std::uint8_t addr,
                      const std::vector<std::uint8_t>& writeBuffer,
                      std::vector<std::uint8_t>& readBuffer);

    /**
     * @brief Write then read using an already-open fd (for ARP sequence)
     */
    bool i2cWriteReadFd(int fd, std::uint8_t addr,
                        const std::vector<std::uint8_t>& writeBuffer,
                        std::vector<std::uint8_t>& readBuffer);

    /**
     * @brief Check if a bus/address is configured with a static EID
     * @param busNum I2C bus number
     * @param hexAddr Hex string of the I2C address
     * @return true if the device is in config.devices with a non-empty
     * staticEndpointId
     */
    bool isConfiguredStaticDevice(std::uint8_t busNum,
                                  const std::string& hexAddr) const;

    /**
     * @brief Remove MCTP neighbor at a given address on an interface
     * @return true if a neighbor was found and removed
     */
    bool removeNeighborByAddress(const std::string& ifname,
                                 const std::string& hexAddr);
};
