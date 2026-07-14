#pragma once

#include "MCTPConstants.hpp"
#include "MCTPDiscovery.hpp"
#include "MCTPReactorConfig.hpp"

#include <sdbusplus/asio/connection.hpp>

#include <chrono>
#include <memory>
#include <string_view>

/**
 * @brief PCIe MCTP endpoint discovery and setup
 */
class MCTPPCIeDiscovery : public MCTPDiscovery
{
  public:
    MCTPPCIeDiscovery(const std::shared_ptr<sdbusplus::asio::connection>& bus,
                      const PCIeDiscoveryConfig& config);
    ~MCTPPCIeDiscovery() override = default;

    void run() override;
    std::string_view name() const override
    {
        return "PCIe";
    }

    /// Bring mctppci0 up and re-configure local EID on host power-on.
    void onHostOn() override
    {
        resetDiscoveryState();
    }

    void resetDiscoveryState();

  private:
    const PCIeDiscoveryConfig& config;
    std::chrono::steady_clock::time_point lastResetTime{};
};
