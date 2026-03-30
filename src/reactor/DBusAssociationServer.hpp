#pragma once

#include "MCTPConstants.hpp"
#include "MCTPReactor.hpp"

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <format>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

/**
 * @brief Concrete D-Bus implementation of AssociationServer
 *
 * Manages OpenBMC association objects under the MCTP object path.
 * Previously defined inline in MCTPReactorMain.cpp.
 */
class DBusAssociationServer : public AssociationServer
{
  public:
    DBusAssociationServer() = delete;
    DBusAssociationServer(const DBusAssociationServer&) = delete;
    DBusAssociationServer(DBusAssociationServer&&) = delete;
    explicit DBusAssociationServer(
        const std::shared_ptr<sdbusplus::asio::connection>& connection) :
        server(connection)
    {
        server.add_manager(mctp::dbus::basePath.data());
    }
    ~DBusAssociationServer() override = default;
    DBusAssociationServer& operator=(const DBusAssociationServer&) = delete;
    DBusAssociationServer& operator=(DBusAssociationServer&&) = delete;

    void associate(const std::string& path,
                   const std::vector<Association>& associations) override
    {
        auto [entry, _] = objects.emplace(
            path, server.add_interface(path, association::interface));
        std::shared_ptr<sdbusplus::asio::dbus_interface> iface = entry->second;
        iface->register_property("Associations", associations);
        iface->initialize();
    }

    void disassociate(const std::string& path) override
    {
        const auto entry = objects.find(path);
        if (entry == objects.end())
        {
            throw std::logic_error(std::format(
                "Attempted to untrack path that was not tracked: {}", path));
        }
        server.remove_interface(entry->second);
        objects.erase(entry);
    }

  private:
    sdbusplus::asio::object_server server;
    std::map<std::string, std::shared_ptr<sdbusplus::asio::dbus_interface>>
        objects;
};
