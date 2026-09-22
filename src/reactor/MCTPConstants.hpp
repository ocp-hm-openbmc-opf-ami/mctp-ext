#pragma once

#include <string_view>

namespace mctp::dbus
{

// CodeConstruct MCTP D-Bus service
constexpr std::string_view service = "au.com.codeconstruct.MCTP1";
constexpr std::string_view basePath = "/au/com/codeconstruct/mctp1";
constexpr std::string_view interfacesPath =
    "/au/com/codeconstruct/mctp1/interfaces/";
constexpr std::string_view networksPath =
    "/au/com/codeconstruct/mctp1/networks";

// CodeConstruct MCTP D-Bus interfaces
constexpr std::string_view busOwnerInterface =
    "au.com.codeconstruct.MCTP.BusOwner1";
constexpr std::string_view endpointInterface =
    "au.com.codeconstruct.MCTP.Endpoint1";
constexpr std::string_view bridgeInterface =
    "au.com.codeconstruct.MCTP.Bridge1";
constexpr std::string_view interfaceIface =
    "au.com.codeconstruct.MCTP.Interface1";

// Entity Manager
constexpr std::string_view entityManagerService =
    "xyz.openbmc_project.EntityManager";
constexpr std::string_view inventoryBasePath =
    "/xyz/openbmc_project/inventory";

// Reactor D-Bus name
constexpr std::string_view reactorService = "xyz.openbmc_project.MCTPReactor";

// OpenBMC decorator interfaces
constexpr std::string_view i2cDecoratorInterface =
    "xyz.openbmc_project.Inventory.Decorator.I2CDevice";

} // namespace mctp::dbus
