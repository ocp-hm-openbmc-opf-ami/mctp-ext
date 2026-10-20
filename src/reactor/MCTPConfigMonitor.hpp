#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

/**
 * @brief Watches Entity Manager for MCTP configuration changes and invokes a
 *        debounced callback so the reactor can reload its in-process config.
 *
 * Monitors both:
 *  - Legacy MCTP{General,I2C,I3C,PCIe,USB}Configuration / MCTP{I2C,I3C}Target
 *  - oks-style Mctp{Config,I3CConfig,SMBusConfig,EndPointConfig,
 *    BusOwnerConfig,MultiValConfig}
 *
 * Signals watched:
 *  - InterfacesAdded
 *  - InterfacesRemoved
 *  - PropertiesChanged
 *
 * Multiple signals arriving in a short window collapse to a single callback
 * invocation via a debounce timer (default 1s).
 */
class MCTPConfigMonitor
{
  public:
    using ReloadCallback = std::function<void()>;

    MCTPConfigMonitor(
        const std::shared_ptr<sdbusplus::asio::connection>& connection,
        boost::asio::io_context& io, ReloadCallback onReload,
        std::chrono::milliseconds debounce = std::chrono::seconds(1));

    ~MCTPConfigMonitor() = default;
    MCTPConfigMonitor(const MCTPConfigMonitor&) = delete;
    MCTPConfigMonitor(MCTPConfigMonitor&&) = delete;
    MCTPConfigMonitor& operator=(const MCTPConfigMonitor&) = delete;
    MCTPConfigMonitor& operator=(MCTPConfigMonitor&&) = delete;

  private:
    bool isMctpConfigInterface(const std::string& iface) const;
    void onInterfacesAdded(sdbusplus::message_t& msg);
    void onInterfacesRemoved(sdbusplus::message_t& msg);
    void onPropertiesChanged(sdbusplus::message_t& msg);
    void scheduleReload(const char* reason, const std::string& path);

    std::shared_ptr<sdbusplus::asio::connection> bus;
    ReloadCallback reloadCb;
    std::chrono::milliseconds debouncePeriod;
    boost::asio::steady_timer debounceTimer;
    std::vector<std::unique_ptr<sdbusplus::bus::match_t>> matches;
};
