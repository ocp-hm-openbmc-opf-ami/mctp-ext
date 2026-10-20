#include "MCTPPCIeDiscovery.hpp"

#include "MCTPConstants.hpp"

#include <phosphor-logging/lg2.hpp>

#include <cstdint>
#include <string>
#include <vector>

PHOSPHOR_LOG2_USING;

MCTPPCIeDiscovery::MCTPPCIeDiscovery(
    const std::shared_ptr<sdbusplus::asio::connection>& bus,
    const PCIeDiscoveryConfig& config) :
    MCTPDiscovery(bus), config(config),
    lastResetTimes(config.interfaces.size())
{
    retryTimers.reserve(config.interfaces.size());
    for (size_t i = 0; i < config.interfaces.size(); ++i)
    {
        retryTimers.emplace_back(bus->get_io_context());
    }
}

void MCTPPCIeDiscovery::startTask(std::chrono::seconds interval)
{
    task.emplace(bus->get_io_context(), interval, [this]() { run(); });
}

void MCTPPCIeDiscovery::run()
{
    runBusOwnerMode();
}

void MCTPPCIeDiscovery::runBusOwnerMode()
{
    auto setupEndpoint = [&](const std::string& linkName, uint8_t hostIdx) {
        try
        {
            std::string ifacePath = std::string(mctp::dbus::basePath) +
                                    "/interfaces/" + linkName;

            auto method = bus->new_method_call(
                mctp::dbus::service.data(), ifacePath.c_str(),
                mctp::dbus::busOwnerInterface.data(), "SetupEndpoint");

            // Parameters: [0x3, 0x00, 0x00]
            std::vector<std::uint8_t> endpoint = {0x3, 0x00, 0x00};
            method.append(endpoint);

            bus->call(method);
            debug("PCIe SetupEndpoint called on {INTF} for Host{HOST}", "INTF",
                  linkName, "HOST", hostIdx);
        }
        catch (const std::exception& e)
        {
            warning(
                "Failed to call PCIe SetupEndpoint on {INTF}: {EXCEPTION}",
                "INTF", linkName, "EXCEPTION", e);
        }
    };

    for (size_t i = 0; i < config.interfaces.size(); ++i)
    {
        const auto& iface = config.interfaces[i];
        if (!iface.enabled || iface.role != "bus-owner")
        {
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        const auto& lastReset = lastResetTimes[i];
        if (lastReset.time_since_epoch().count() != 0 &&
            now - lastReset < std::chrono::seconds(PCIE_DISCOVERY_DELAY_TIME))
        {
            debug(
                "PCIe discovery skipped for {INTF}, within {DELAY} sec cooldown after reset",
                "INTF", iface.physicalLinkName, "DELAY",
                PCIE_DISCOVERY_DELAY_TIME);
            continue;
        }

        setupEndpoint(iface.physicalLinkName, iface.hostIndex);
    }
}

void MCTPPCIeDiscovery::resetDiscoveryState(uint8_t hostIndex)
{
    auto now = std::chrono::steady_clock::now();

    for (size_t i = 0; i < config.interfaces.size(); ++i)
    {
        const auto& iface = config.interfaces[i];
        if (!iface.enabled || iface.hostIndex != hostIndex)
        {
            continue;
        }
        lastResetTimes[i] = now;
        bringUpInterface(i);
    }
}

void MCTPPCIeDiscovery::bringUpInterface(size_t idx)
{
    const auto& iface = config.interfaces[idx];
    if (ensureInterfaceReady(iface.physicalLinkName, iface.ownEID,
                             iface.pcieNet, iface.mtu))
    {
        return;
    }

    warning(
        "PCIe discovery: failed to bring up {IFACE}, retrying in {DELAY}s",
        "IFACE", iface.physicalLinkName, "DELAY", PCIE_DISCOVERY_DELAY_TIME);

    retryTimers[idx].expires_after(
        std::chrono::seconds(PCIE_DISCOVERY_DELAY_TIME));
    retryTimers[idx].async_wait(
        [this, idx](const boost::system::error_code& ec) {
            if (ec)
            {
                // Timer cancelled (e.g. object destroyed); do not retry.
                return;
            }
            bringUpInterface(idx);
        });
}

