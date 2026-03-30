#include "MCTPPCIeDiscovery.hpp"

#include <phosphor-logging/lg2.hpp>

#include <vector>

PHOSPHOR_LOG2_USING;

MCTPPCIeDiscovery::MCTPPCIeDiscovery(
    const std::shared_ptr<sdbusplus::asio::connection>& bus,
    const PCIeDiscoveryConfig& config) :
    MCTPDiscovery(bus),
    config(config)
{
}

void MCTPPCIeDiscovery::run()
{
    auto now = std::chrono::steady_clock::now();
    if (lastResetTime.time_since_epoch().count() != 0 &&
        now - lastResetTime < std::chrono::seconds(PCIE_DISCOVERY_DELAY_TIME))
    {
        debug("PCIe discovery skipped, within {DELAY} sec cooldown after reset",
              "DELAY", PCIE_DISCOVERY_DELAY_TIME);
        return;
    }

    try
    {
        std::string ifacePath = std::string(mctp::dbus::basePath) + "/interfaces/mctppci0";

        auto method = bus->new_method_call(
            mctp::dbus::service.data(), ifacePath.c_str(),
            mctp::dbus::busOwnerInterface.data(), "SetupEndpoint");

        // Parameters: [0x3, 0x00, 0x00]
        std::vector<std::uint8_t> endpoint = {0x3, 0x00, 0x00};
        method.append(endpoint);

        bus->call(method);
        debug("PCIe SetupEndpoint called successfully");
    }
    catch (const std::exception& e)
    {
        warning("Failed to call PCIe SetupEndpoint: {EXCEPTION}",
                "EXCEPTION", e);
    }
}

void MCTPPCIeDiscovery::resetDiscoveryState()
{
    lastResetTime = std::chrono::steady_clock::now();
    ensureInterfaceReady("mctppci0", config.localEid, config.pcieNet);
}
