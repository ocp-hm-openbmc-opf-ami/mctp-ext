/*
// Copyright (c) 2025 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
*/
#include "wrapper_impl.hpp"

#include "constants.hpp"

#include <boost/asio/spawn.hpp>
#include <phosphor-logging/lg2.hpp>

#include <array>
#include <format>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace mctp::dbus
{
namespace
{
constexpr std::string_view mctpMultiValConfigIntf =
    "xyz.openbmc_project.Configuration.MctpMultiValConfig";
constexpr std::string_view i3cConfigIntf =
    "xyz.openbmc_project.Configuration.MctpI3CConfig";
constexpr std::string_view smbusConfigIntf =
    "xyz.openbmc_project.Configuration.MctpSMBusConfig";
constexpr std::string_view endpointConfigIntf =
    "xyz.openbmc_project.Configuration.MctpEndPointConfig";
constexpr std::string_view busOwnerConfigIntf =
    "xyz.openbmc_project.Configuration.MctpBusOwnerConfig";
} // namespace

void ConfigWrapperImpl::getMCTPConfigurations(
    boost::asio::yield_context yield, MCTPConfigCallback callback)
{
    ConfigWrapper::getMCTPConfigurations(yield, std::move(callback));

    namespace rules = sdbusplus::bus::match::rules;
    std::string matchRule =
        rules::interfacesAdded() +
        rules::path_namespace("/xyz/openbmc_project/inventory") +
        rules::sender("xyz.openbmc_project.EntityManager");

    mctpConfigMatch = std::make_unique<sdbusplus::bus::match::match>(
        *connection, matchRule,
        std::bind_front(&ConfigWrapperImpl::onNewMCTPConfigurationSignal,
                        this));
    lg2::info("Listening for new MCTP configurations on D-Bus");

    constexpr std::string_view objectMapperService =
        "xyz.openbmc_project.ObjectMapper";
    constexpr std::string_view objectMapperPath =
        "/xyz/openbmc_project/object_mapper";
    constexpr std::string_view objectMapperIface =
        "xyz.openbmc_project.ObjectMapper";
    constexpr std::string_view inventoryRoot =
        "/xyz/openbmc_project/inventory/system/board";
    constexpr int depth = 2;
    const std::array<std::string, 1> interfaces = {mctpConfigInterfaceName};

    boost::system::error_code ec;
    auto paths = connection->yield_method_call<std::vector<std::string>>(
        yield, ec, objectMapperService.data(), objectMapperPath.data(),
        objectMapperIface.data(), "GetSubTreePaths",
        std::string(inventoryRoot), depth, interfaces);

    if (ec)
    {
        lg2::error("GetSubTreePaths failed: {ERROR}", "ERROR", ec.message());
        return;
    }

    for (const auto& path : paths)
    {
        if (onNewMCTPConfig)
        {
            onNewMCTPConfig(yield, path);
        }
    }
}

void ConfigWrapperImpl::onNewMCTPConfigurationSignal(sdbusplus::message_t& msg)
{
    try
    {
        sdbusplus::message::object_path unitPath;
        std::unordered_map<std::string, ConfigurationMap> interfacesAdded;

        msg.read(unitPath, interfacesAdded);
        if (!interfacesAdded.contains(mctpConfigInterfaceName))
        {
            return;
        }
        lg2::info("New MCTP configuration at {PATH}", "PATH", unitPath.str);
        if (!onNewMCTPConfig)
        {
            return;
        }
        auto pathStr = unitPath.str;
        boost::asio::spawn(
            connection->get_io_context(),
            [this, pathStr](boost::asio::yield_context yield) {
                onNewMCTPConfig(yield, pathStr);
            },
            [pathStr](std::exception_ptr ep) {
                if (ep)
                {
                    try
                    {
                        std::rethrow_exception(ep);
                    }
                    catch (const std::exception& e)
                    {
                        lg2::error(
                            "Async config processing failed for {PATH}: {ERROR}",
                            "PATH", pathStr, "ERROR", e.what());
                    }
                }
            });
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to process MCTP configuration signal: {ERROR}",
                   "ERROR", e.what());
    }
}

ConfigWrapperImpl::ConfigurationField
    ConfigWrapperImpl::getConfigurationField(
        boost::asio::yield_context yield, const std::string& configPath,
        const std::string& property, bool throwOnErr)
{
    constexpr std::string_view entityManagerService =
        "xyz.openbmc_project.EntityManager";
    constexpr std::string_view propertiesIface =
        "org.freedesktop.DBus.Properties";

    auto interfaceName = getInterfaceName(property);

    boost::system::error_code ec;
    auto val = connection->yield_method_call<ConfigurationField>(
        yield, ec, entityManagerService.data(), configPath,
        propertiesIface.data(), "Get", interfaceName, property);

    if (ec)
    {
        if (throwOnErr)
        {
            throw std::runtime_error(std::format(
                "Failed to read property '{}' from {} ({}): {}", property,
                configPath, interfaceName, ec.message()));
        }
        return ConfigurationField{};
    }
    return val;
}

std::string ConfigWrapperImpl::getInterfaceName(
    const std::string& property) const
{
    using namespace mctp::config::property;
    static const std::unordered_map<std::string, std::string> map = {
        {std::string(name), mctpConfigInterfaceName},
        {std::string(physicalLinkType), mctpConfigInterfaceName},
        {std::string(physicalLinkName), mctpConfigInterfaceName},
        {std::string(priority), mctpConfigInterfaceName},
        {std::string(networkID), mctpConfigInterfaceName},
        {std::string(role), mctpConfigInterfaceName},
        {std::string(retryCount), mctpConfigInterfaceName},
        {std::string(timeOut), mctpConfigInterfaceName},
        {std::string(resetEvents), mctpConfigInterfaceName},
        {std::string(mtu), mctpConfigInterfaceName},
        {std::string(eidPool), std::string(busOwnerConfigIntf)},
        {std::string(ownEID), std::string(busOwnerConfigIntf)},
        {std::string(requestEIDPool), std::string(endpointConfigIntf)},
        {std::string(busNumber), std::string(i3cConfigIntf)},
        {std::string(pidMask), std::string(i3cConfigIntf)},
        {std::string(defaultAddress), std::string(smbusConfigIntf)},
        {std::string(useSharedEID), std::string(mctpMultiValConfigIntf)},
        {std::string(configIndex), std::string(mctpMultiValConfigIntf)},
        {std::string(sharedRequestEIDPool),
         std::string(mctpMultiValConfigIntf)},
    };
    auto it = map.find(property);
    if (it == map.end())
    {
        throw std::invalid_argument(
            std::format("Unknown MCTP config property '{}'", property));
    }
    return it->second;
}
} // namespace mctp::dbus
