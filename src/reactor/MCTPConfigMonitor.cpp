#include "MCTPConfigMonitor.hpp"

#include "MCTPConstants.hpp"

#include <phosphor-logging/lg2.hpp>

#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

PHOSPHOR_LOG2_USING;

namespace
{
const std::set<std::string>& mctpConfigInterfaces()
{
    static const std::set<std::string> set = {
        // Legacy MCTP* configuration interfaces
        "xyz.openbmc_project.Configuration.MCTPGeneralSetting",
        "xyz.openbmc_project.Configuration.MCTPI2CConfiguration",
        "xyz.openbmc_project.Configuration.MCTPI2CTarget",
        "xyz.openbmc_project.Configuration.MCTPI3CConfiguration",
        "xyz.openbmc_project.Configuration.MCTPI3CTarget",
        "xyz.openbmc_project.Configuration.MCTPPCIeConfiguration",
        "xyz.openbmc_project.Configuration.MCTPUSBConfiguration",
        // oks-style configuration interfaces
        "xyz.openbmc_project.Configuration.MctpConfig",
        "xyz.openbmc_project.Configuration.MctpI3CConfig",
        "xyz.openbmc_project.Configuration.MctpSMBusConfig",
        "xyz.openbmc_project.Configuration.MctpEndPointConfig",
        "xyz.openbmc_project.Configuration.MctpBusOwnerConfig",
        "xyz.openbmc_project.Configuration.MctpMultiValConfig",
    };
    return set;
}
} // namespace

MCTPConfigMonitor::MCTPConfigMonitor(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    boost::asio::io_context& io, ReloadCallback onReload,
    std::chrono::milliseconds debounce) :
    bus(connection),
    reloadCb(std::move(onReload)), debouncePeriod(debounce),
    debounceTimer(io)
{
    namespace rules = sdbusplus::bus::match::rules;

    const std::string emSender =
        rules::sender(mctp::dbus::entityManagerService.data());
    const std::string pathNs =
        rules::path_namespace(mctp::dbus::inventoryBasePath.data());

    // InterfacesAdded
    {
        std::string spec = rules::interfacesAdded() + emSender + pathNs;
        matches.emplace_back(std::make_unique<sdbusplus::bus::match_t>(
            *bus, spec,
            [this](sdbusplus::message_t& msg) {
                this->onInterfacesAdded(msg);
            }));
    }

    // InterfacesRemoved
    {
        std::string spec = rules::interfacesRemoved() + emSender + pathNs;
        matches.emplace_back(std::make_unique<sdbusplus::bus::match_t>(
            *bus, spec,
            [this](sdbusplus::message_t& msg) {
                this->onInterfacesRemoved(msg);
            }));
    }

    // PropertiesChanged on any inventory object owned by EM (filter by
    // interface name in the callback).
    {
        std::string spec = "type='signal',member='PropertiesChanged'," +
                           emSender + pathNs;
        matches.emplace_back(std::make_unique<sdbusplus::bus::match_t>(
            *bus, spec,
            [this](sdbusplus::message_t& msg) {
                this->onPropertiesChanged(msg);
            }));
    }

    info("MCTPConfigMonitor watching Entity Manager configuration "
         "(debounce {MS} ms)",
         "MS", static_cast<long>(debouncePeriod.count()));
}

bool MCTPConfigMonitor::isMctpConfigInterface(const std::string& iface) const
{
    return mctpConfigInterfaces().contains(iface);
}

void MCTPConfigMonitor::onInterfacesAdded(sdbusplus::message_t& msg)
{
    try
    {
        sdbusplus::message::object_path path;
        std::unordered_map<
            std::string,
            std::unordered_map<std::string,
                               std::variant<std::monostate, bool, uint8_t,
                                            uint16_t, uint32_t, uint64_t,
                                            int16_t, int32_t, int64_t, double,
                                            std::string,
                                            std::vector<std::string>,
                                            std::vector<uint8_t>,
                                            std::vector<uint64_t>>>>
            interfaces;
        msg.read(path, interfaces);
        for (const auto& [iface, _] : interfaces)
        {
            if (isMctpConfigInterface(iface))
            {
                scheduleReload("InterfacesAdded", path.str);
                return;
            }
        }
    }
    catch (const std::exception& e)
    {
        warning("MCTPConfigMonitor InterfacesAdded parse failed: {ERR}",
                "ERR", e.what());
    }
}

void MCTPConfigMonitor::onInterfacesRemoved(sdbusplus::message_t& msg)
{
    try
    {
        sdbusplus::message::object_path path;
        std::vector<std::string> interfaces;
        msg.read(path, interfaces);
        for (const auto& iface : interfaces)
        {
            if (isMctpConfigInterface(iface))
            {
                scheduleReload("InterfacesRemoved", path.str);
                return;
            }
        }
    }
    catch (const std::exception& e)
    {
        warning("MCTPConfigMonitor InterfacesRemoved parse failed: {ERR}",
                "ERR", e.what());
    }
}

void MCTPConfigMonitor::onPropertiesChanged(sdbusplus::message_t& msg)
{
    try
    {
        // The first argument of PropertiesChanged is the interface name;
        // we only need to peek at it.
        std::string iface;
        msg.read(iface);
        if (!isMctpConfigInterface(iface))
        {
            return;
        }
        scheduleReload("PropertiesChanged", msg.get_path());
    }
    catch (const std::exception& e)
    {
        warning("MCTPConfigMonitor PropertiesChanged parse failed: {ERR}",
                "ERR", e.what());
    }
}

void MCTPConfigMonitor::scheduleReload(const char* reason,
                                       const std::string& path)
{
    info("MCTPConfigMonitor: {REASON} on {PATH}, debouncing reload",
         "REASON", std::string(reason), "PATH", path);
    debounceTimer.expires_after(debouncePeriod);
    debounceTimer.async_wait([this](const boost::system::error_code& ec) {
        if (ec)
        {
            // Cancelled because another signal arrived; do nothing.
            return;
        }
        if (!reloadCb)
        {
            return;
        }
        info("MCTPConfigMonitor: invoking reload callback");
        try
        {
            reloadCb();
        }
        catch (const std::exception& e)
        {
            error("MCTPConfigMonitor reload callback threw: {ERR}", "ERR",
                  e.what());
        }
    });
}
