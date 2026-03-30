#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sdbusplus::asio
{
class connection;
}

/**
 * @brief Abstract interface for periodic MCTP discovery modules
 *
 * Each discovery module (I2C hotplug, I3C rescan, PCIe setup,
 * routing table query) implements this interface so they can be
 * managed uniformly by the reactor's periodic task system.
 *
 * Lifecycle / event dispatch
 * --------------------------
 * Every concrete subclass auto-registers itself in a process-wide
 * registry when constructed and unregisters when destroyed. The static
 * dispatchHost{On,Off}() / dispatchPlatformReset() helpers fan an event
 * out to every registered instance by invoking the corresponding
 * virtual hook. Subclasses opt in to events by overriding the relevant
 * hook(s); the default implementations are no-ops.
 */
class MCTPDiscovery
{
  public:
    virtual ~MCTPDiscovery();

    /**
     * @brief Execute one round of discovery/scanning
     */
    virtual void run() = 0;

    /**
     * @brief Return a human-readable name for logging
     */
    virtual std::string_view name() const = 0;

    /**
     * @brief Host power-on event hook (default: no-op).
     *        Discovery modules override this to reset state and re-run
     *        discovery when the host comes up.
     */
    virtual void onHostOn() {}

    /**
     * @brief Host power-off event hook (default: no-op).
     *        Discovery modules override this to release per-host resources.
     */
    virtual void onHostOff() {}

    /**
     * @brief Platform (eSPI) reset event hook (default: no-op).
     *        Discovery modules override this to handle in-band resets that
     *        don't transition host power state.
     */
    virtual void onPlatformReset() {}

    /**
     * @brief Fan-out helpers: invoke the matching hook on every registered
     *        MCTPDiscovery instance. Safe to call from the reactor thread.
     */
    static void dispatchHostOn();
    static void dispatchHostOff();
    static void dispatchPlatformReset();

    /**
     * @brief Remove all endpoints by enumerating D-Bus endpoint objects and calling Remove
     * @return true if all Remove calls succeeded, false otherwise
     */
    bool removeAllEndpoint();

  protected:
    MCTPDiscovery(const std::shared_ptr<sdbusplus::asio::connection>& connection);

    /**
     * @brief Get MCTP link interfaces using netlink API
     * @param prefix Interface name prefix to filter (e.g., "mctpi2c")
     * @return Vector of MCTP interface names matching the prefix
     */
    std::vector<std::string> getLinksViaNetlink(const std::string& prefix);

    /**
     * @brief Set up the local EID for a given interface using netlink
     * @brief Check if a network interface is up and has the expected local EID.
     *        If not ready, bring it up and configure the local EID.
     * @param interfaceName Network interface name (e.g., "mctpusb1")
     * @param eid Expected local EID
     * @param net MCTP network number
     * @return true if the interface is up with the correct local EID
     */
    bool ensureInterfaceReady(const std::string& interfaceName, uint8_t eid,
                              int net);

    /**
     * @brief Response from AssignEndpoint D-Bus method call
     */
    struct AssignEndpointResponse
    {
        std::uint8_t eid;           // Assigned endpoint ID
        std::int32_t networkId;     // Network number
        std::string interface;      // Interface name
        bool probed;                // Probed flag
    };

    /**
     * @brief Assign endpoint via D-Bus to interface path
     * D-Bus path: /au/com/codeconstruct/mctp1/interfaces/{interface_name}
     * @param interfaceName Interface name (e.g., "usb-1-2-3")
     * @param address Physical address vector
     * @return AssignEndpointResponse with assigned EID and network info
     */
    AssignEndpointResponse assignEndpoint(const std::string& interfaceName,
                                          const std::vector<std::uint8_t>& address);

    /**
     * @brief Remove endpoint via D-Bus using network ID and endpoint ID
     * D-Bus path format: /au/com/codeconstruct/mctp/networks/<network-id>/endpoints/<endpoint-id>
     */
    bool removeEndpoint(std::uint16_t networkId, std::uint8_t eid);

    /**
     * @brief Assign a static endpoint via D-Bus
     * Uses AssignEndpointStatic method which takes an EID and address
     * @param device Device/interface name (e.g., "mctpi2c1")
     * @param eid Endpoint ID to assign
     * @param hexAddr Physical address as hex string (e.g., "50")
     * @return true if successful
     */
    bool assignEndpointStatic(const std::string& device, std::uint8_t eid,
                              const std::string& hexAddr);

    std::shared_ptr<sdbusplus::asio::connection> bus;
};
