#pragma once

#include "MCTPConstants.hpp"
#include "MCTPDiscovery.hpp"
#include "Utils.hpp"

#include <sdbusplus/asio/connection.hpp>

#include <chrono>
#include <memory>
#include <string>

/**
 * @brief Queries MCTP bridge routing tables via D-Bus
 *
 * This replaces the getRoutingTable() free function that was
 * previously defined in MCTPReactorMain.cpp, which itself was
 * a C++ port of mctp-get-routing-table.sh.
 */
class MCTPRoutingTableDiscovery : public MCTPDiscovery
{
  public:
    explicit MCTPRoutingTableDiscovery(
        const std::shared_ptr<sdbusplus::asio::connection>& bus) :
        MCTPDiscovery(bus)
    {}
    ~MCTPRoutingTableDiscovery() override = default;

    void run() override;
    std::string_view name() const override
    {
        return "RoutingTable";
    }

    /// Re-trigger discovery and refresh routing table when host comes up.
    void onHostOn() override { notifyHostOn(); }
    /// Drop all known endpoints when host goes away.
    void onHostOff() override { removeAllEndpoint(); }

    void notifyHostOn();

  private:
    std::chrono::steady_clock::time_point hostOnTime{};
};
