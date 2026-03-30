#include "MCTPI2CDiscovery.hpp"

#include "Utils.hpp"

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
#include "MCTPEndpoint.hpp"
#include "MCTPReactor.hpp"
#endif

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/message.hpp>

#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <unistd.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
extern "C" {
#include <i2c/smbus.h>
}
#include <net/if.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>

PHOSPHOR_LOG2_USING;

// Forbidden I2C addresses (reserved, reserved, reserved, etc.)
const std::set<std::string> MCTPI2CDiscovery::forbiddenAddresses = {
    "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",  "a",  "b",  "c",
    "d",  "e",  "f",  "00", "01", "02", "03", "04", "05", "06", "07", "30",
    "3c", "50", "61", "78", "79", "7a", "7b", "7c", "7d", "7e", "7f"};

MCTPI2CDiscovery::MCTPI2CDiscovery(
    const std::shared_ptr<sdbusplus::asio::connection>& bus,
    const I2CDiscoveryConfig& cfg) :
    MCTPDiscovery(bus),
    config(cfg),
    i2cNet(cfg.i2cNet)
{
    if (!cfg.whitelist.empty())
    {
        whitelist = cfg.whitelist;
    }

    setMuxIdleMode(MuxIdleModes::muxIdleModeConnect);
}

void MCTPI2CDiscovery::run()
{
    debug("=== Starting I2C Discovery ===");

    // Phase 1: If ARP mode enabled, perform ARP assignment
    if (config.arpEnabled)
    {
        debug("Performing I2C ARP discovery...");
        performARPAssignment();
    }

    // Phase 2: Scan configured device list first (static EID assignments)
    if (!config.devices.empty())
    {
        debug("Scanning configured I2C devices...");
        scanConfiguredDevices();
    }

    // Phase 3: Perform hotplug discovery for remaining devices
    debug("Performing I2C hotplug discovery...");
    scanForNewDevices();

    // Phase 4: Always validate existing routes
    validateExistingRoutes();

    debug("=== I2C Discovery Complete ===");
}

void MCTPI2CDiscovery::validateExistingRoutes()
{
    debug("=== Phase 1: Validating Existing Routes ===");

    // Get neighbors via netlink
    std::vector<MCTPNeighbor> neighbors = getNeighborsViaNetlink();
    if (neighbors.empty())
    {
        debug("No existing routes found");
        return;
    }

    for (const auto& neighbor : neighbors)
    {
        // Extract bus number from device name (mctpi2c1 -> 1)
        if (neighbor.device.find("mctpi2c") != 0)
        {
            continue;
        }

        std::string busStr = neighbor.device.substr(7);
        std::uint8_t busNum = static_cast<std::uint8_t>(std::stoi(busStr));

        // Check if address responds
        std::string probePath = "/dev/i2c-" + std::to_string(busNum);
        int probeFd = open(probePath.c_str(), O_RDWR);
        bool responds = false;
        if (probeFd >= 0)
        {
            responds = probeI2CAddress(probeFd, neighbor.physAddr);
            close(probeFd);
        }
        if (!responds)
        {
            debug("No response at 0x{HEX_ADDR} on bus {BUS_NUM} (stale "
                 "endpoint). Removing EID {EID}",
                 "HEX_ADDR", std::format("{:02x}", neighbor.physAddr),
                 "BUS_NUM", busNum, "EID", neighbor.eid);

            if (removeEndpoint(config.i2cNet, neighbor.eid))
            {
                debug("Successfully removed stale EID {EID}", "EID",
                     neighbor.eid);
            }
            else
            {
                warning("Failed to remove stale EID {EID}", "EID",
                        neighbor.eid);
            }
        }
        else
        {
            debug("Address 0x{HEX_ADDR} responds on bus {BUS_NUM} (active)",
                 "HEX_ADDR", std::format("{:02x}", neighbor.physAddr),
                 "BUS_NUM", busNum);

            // Check if endpoint needs syncing
            std::string epPath = std::string(mctp::dbus::basePath) + "/networks/" +
                                 std::to_string(i2cNet) + "/endpoints/" +
                                 std::to_string(neighbor.eid);
            try
            {
                auto method = bus->new_method_call(
                    mctp::dbus::service.data(), epPath.c_str(),
                    "org.freedesktop.DBus.Properties", "GetAll");
                bus->call(method);
            }
            catch (const std::exception&)
            {
                // Endpoint doesn't exist, sync it
                if (assignEndpointStatic(neighbor.device, neighbor.eid,
                                         std::format("{:02x}", neighbor.physAddr)))
                {
                    debug("Synced EID {EID} via AssignEndpointStatic", "EID",
                         neighbor.eid);
                }
            }
        }
    }
}

void MCTPI2CDiscovery::scanForNewDevices()
{
    debug("=== Phase 3: Scanning I2C Links (Buses >= {MIN_BUS})", "MIN_BUS",
         minBusNum);

    std::vector<std::string> devices = getLinksViaNetlink("mctpi2c");
    if (devices.empty())
    {
        debug("No MCTP links found");
        return;
    }

    for (const auto& device : devices)
    {
        // Extract bus number
        std::string busStr = device.substr(7);
        std::uint8_t busNum = static_cast<std::uint8_t>(std::stoi(busStr));

        if (busNum < minBusNum)
        {
            continue;
        }

        debug("Scanning bus {BUS_NUM} ({DEVICE})", "BUS_NUM", busNum, "DEVICE",
             device);

        // Ensure interface is up and local EID is configured
        if (!ensureInterfaceReady(device, config.localEid, i2cNet))
        {
            continue;
        }

        // Scan I2C addresses via ioctl
        std::vector<std::uint8_t> detectedAddresses =
            scanI2CRange(busNum, 0x08, 0x77);

        for (std::uint8_t addr : detectedAddresses)
        {
            std::string hexAddr = std::format("{:02x}", addr);

            // Skip addresses that are configured with a static EID
            if (isConfiguredStaticDevice(busNum, hexAddr))
            {
                debug("Device 0x{HEX_ADDR} on bus {BUS_NUM} is configured "
                      "with static EID, skipping dynamic assignment",
                      "HEX_ADDR", hexAddr, "BUS_NUM", busNum);
                continue;
            }

            if (neighborExistsViaNetlink(hexAddr, device))
            {
                debug("Device 0x{HEX_ADDR} already exists in MCTP neighbors",
                      "HEX_ADDR", hexAddr);
                continue;
            }

            if (isDeviceExistsInDBus(busNum, hexAddr))
            {
                debug(
                    "Device at 0x{HEX_ADDR} on bus {BUS_NUM} already exists in D-Bus",
                    "HEX_ADDR", hexAddr, "BUS_NUM", busNum);
                continue;
            }

            debug("New device at 0x{HEX_ADDR} on {DEVICE}. Assigning...",
                 "HEX_ADDR", hexAddr, "DEVICE", device);

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
            manageDeviceViaReactor(busNum, addr, device);
#else
            // Call parent class assignEndpoint with hex address as vector
            std::vector<std::uint8_t> addrVector;
            addrVector.push_back(std::stoi(hexAddr, nullptr, 16));
            auto response = assignEndpoint(device, addrVector);

            if (response.eid != 0)
            {
                debug("Successfully assigned endpoint for 0x{HEX_ADDR}: EID={EID}",
                     "HEX_ADDR", hexAddr, "EID", static_cast<int>(response.eid));
            }
            else
            {
                warning("Failed to assign endpoint for 0x{HEX_ADDR}",
                        "HEX_ADDR", hexAddr);
            }
#endif
        }
    }
}

void MCTPI2CDiscovery::scanConfiguredDevices()
{
    debug("=== Scanning {COUNT} configured I2C device(s) ===", "COUNT",
         config.devices.size());

    for (const auto& device : config.devices)
    {
        // Parse bus and address from string fields
        std::uint8_t busNum = 0;
        std::uint8_t addr = 0;
        try
        {
            busNum = static_cast<std::uint8_t>(
                std::stoul(device.bus, nullptr, 0));
            addr = static_cast<std::uint8_t>(
                std::stoul(device.address, nullptr, 0));
        }
        catch (const std::exception& e)
        {
            warning("Invalid bus/address for device {NAME}: {ERR}",
                    "NAME", device.name, "ERR", e);
            continue;
        }

        std::string hexAddr = std::format("{:02x}", addr);
        std::string ifaceName = (busNum == 0) ? "mctpmbox0" : "mctpi2c" + std::to_string(busNum);

        // Ensure interface is up and local EID is configured
        if (!ensureInterfaceReady(ifaceName, config.localEid, i2cNet))
        {
            continue;
        }

        // Check if already discovered via netlink
        if (neighborExistsViaNetlink(hexAddr, ifaceName))
        {
            debug("Configured device {NAME} at 0x{ADDR} already in MCTP "
                  "neighbors",
                  "NAME", device.name, "ADDR", hexAddr);
            continue;
        }

        // Check if already exists in D-Bus
        if (isDeviceExistsInDBus(busNum, hexAddr))
        {
            debug("Configured device {NAME} at 0x{ADDR} already in D-Bus",
                  "NAME", device.name, "ADDR", hexAddr);
            continue;
        }

        // Probe the I2C address to check if slave exists (only for I2C buses)
        if (busNum > 0)
        {
            std::string probePath = "/dev/i2c-" + std::to_string(busNum);
            int fd = open(probePath.c_str(), O_RDWR);
            if (fd < 0)
            {
                warning("Cannot open {PATH} for configured device {NAME}", "PATH",
                        probePath, "NAME", device.name);
                continue;
            }

            bool responds = probeI2CAddress(fd, addr);
            close(fd);

            if (!responds)
            {
                debug("Configured device {NAME} at 0x{ADDR} on bus {BUS} not "
                      "present",
                      "NAME", device.name, "ADDR", hexAddr, "BUS", busNum);
                continue;
            }
        }

        debug("Configured device {NAME} found at 0x{ADDR} on {IFACE}. "
             "Assigning...",
             "NAME", device.name, "ADDR", hexAddr, "IFACE", ifaceName);

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
        manageDeviceViaReactor(busNum, addr, ifaceName);
#else
        // Use static EID assignment if StaticEndpointID is configured
        if (!device.staticEndpointId.empty())
        {
            std::uint8_t staticEid = 0;
            try
            {
                staticEid = static_cast<std::uint8_t>(
                    std::stoul(device.staticEndpointId, nullptr, 0));
            }
            catch (const std::exception& e)
            {
                warning("Invalid StaticEndpointID for device {NAME}: {ERR}",
                        "NAME", device.name, "ERR", e);
                continue;
            }

            debug("Using static EID {EID} for configured device {NAME}",
                 "EID", staticEid, "NAME", device.name);

            if (assignEndpointStatic(ifaceName, staticEid, hexAddr))
            {
                debug("Assigned configured device {NAME} with static EID={EID}",
                     "NAME", device.name, "EID", staticEid);
            }
            else
            {
                warning("Failed to assign static EID {EID} for device {NAME} "
                        "at 0x{ADDR}",
                        "EID", staticEid, "NAME", device.name, "ADDR", hexAddr);
            }
        }
        else
        {
            // No static EID configured, use dynamic assignment
            std::vector<std::uint8_t> addrVector = {addr};
            auto response = assignEndpoint(ifaceName, addrVector);

            if (response.eid != 0)
            {
                debug("Assigned configured device {NAME}: EID={EID}", "NAME",
                     device.name, "EID", static_cast<int>(response.eid));
            }
            else
            {
                warning("Failed to assign configured device {NAME} at 0x{ADDR}",
                        "NAME", device.name, "ADDR", hexAddr);
            }
        }
#endif
    }
}

bool MCTPI2CDiscovery::isConfiguredStaticDevice(
    std::uint8_t busNum, const std::string& hexAddr) const
{
    for (const auto& device : config.devices)
    {
        if (device.staticEndpointId.empty())
        {
            continue;
        }

        try
        {
            std::uint8_t cfgBus = static_cast<std::uint8_t>(
                std::stoul(device.bus, nullptr, 0));
            std::uint8_t cfgAddr = static_cast<std::uint8_t>(
                std::stoul(device.address, nullptr, 0));
            std::string cfgHexAddr = std::format("{:02x}", cfgAddr);

            if (cfgBus == busNum && cfgHexAddr == hexAddr)
            {
                return true;
            }
        }
        catch (const std::exception&)
        {
            // Invalid config entry, skip
        }
    }
    return false;
}

bool MCTPI2CDiscovery::isForbidden(const std::string& hexAddr) const
{
    return forbiddenAddresses.find(hexAddr) != forbiddenAddresses.end();
}

bool MCTPI2CDiscovery::isInWhitelist(const std::string& hexAddr) const
{
    if (whitelist.empty())
    {
        return true; // Empty whitelist means allow all
    }
    return whitelist.find(hexAddr) != whitelist.end();
}

std::vector<std::uint8_t> MCTPI2CDiscovery::scanI2CRange(
    std::uint8_t busNum, std::uint8_t startAddr, std::uint8_t endAddr)
{
    std::vector<std::uint8_t> foundAddresses;

    try
    {
        std::string devPath = "/dev/i2c-" + std::to_string(busNum);
        int fd = open(devPath.c_str(), O_RDWR);

        if (fd < 0)
        {
            warning("Failed to open I2C device {DEVICE}: {ERROR}", "DEVICE",
                    devPath, "ERROR", strerror(errno));
            return foundAddresses;
        }

        // Scan range of addresses
        for (std::uint8_t addr = startAddr; addr <= endAddr; ++addr)
        {
            std::string hexAddr = std::format("{:02x}", addr);

            if (!isInWhitelist(hexAddr))
            {
                continue;
            }

            // Check if forbidden or already exists
            if (isForbidden(hexAddr))
            {
                debug("Device at 0x{HEX_ADDR} is forbidden", "HEX_ADDR",
                      hexAddr);
                continue;
            }

            // Use enhanced probe method
            if (probeI2CAddress(fd, addr))
            {
                foundAddresses.push_back(addr);
                debug("Found I2C device at 0x{ADDR} on bus {BUS}", "ADDR",
                      std::format("{:02x}", addr), "BUS", busNum);
            }
        }

        close(fd);
    }
    catch (const std::exception& e)
    {
        warning(
            "Exception scanning I2C range 0x{START}-0x{END} on bus {BUS}: {EXCEPTION}",
            "START", std::format("{:02x}", startAddr), "END",
            std::format("{:02x}", endAddr), "BUS", busNum, "EXCEPTION", e);
    }

    return foundAddresses;
}

std::vector<MCTPNeighbor> MCTPI2CDiscovery::getNeighborsViaNetlink()
{
    std::vector<MCTPNeighbor> neighbors;

    try
    {
        int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        if (sock < 0)
        {
            warning("Failed to create netlink socket: {ERROR}", "ERROR",
                    strerror(errno));
            return neighbors;
        }

        // Bind to netlink socket
        struct sockaddr_nl local = {};
        local.nl_family = AF_NETLINK;
        local.nl_groups = 0;

        if (bind(sock, (struct sockaddr*)&local, sizeof(local)) < 0)
        {
            warning("Failed to bind netlink socket: {ERROR}", "ERROR",
                    strerror(errno));
            close(sock);
            return neighbors;
        }

        // Prepare RTM_GETNEIGH request
        struct
        {
            struct nlmsghdr hdr;
            struct ndmsg msg;
        } req = {};

        req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));
        req.hdr.nlmsg_type = RTM_GETNEIGH;
        req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
        req.hdr.nlmsg_seq = 1;
        req.hdr.nlmsg_pid = getpid();

        req.msg.ndm_family = AF_UNSPEC; // Get all families
        req.msg.ndm_ifindex = 0;        // All interfaces

        // Send request
        struct sockaddr_nl peer = {};
        peer.nl_family = AF_NETLINK;

        if (sendto(sock, &req, req.hdr.nlmsg_len, 0, (struct sockaddr*)&peer,
                   sizeof(peer)) < 0)
        {
            warning("Failed to send netlink request: {ERROR}", "ERROR",
                    strerror(errno));
            close(sock);
            return neighbors;
        }

        // Receive responses
        char buffer[65536];
        socklen_t peerlen = sizeof(peer);

        while (true)
        {
            int len = recvfrom(sock, buffer, sizeof(buffer), 0,
                               (struct sockaddr*)&peer, &peerlen);
            if (len < 0)
            {
                warning("Failed to receive netlink response: {ERROR}", "ERROR",
                        strerror(errno));
                break;
            }

            if (len == 0)
            {
                break;
            }

            // Parse netlink messages
            for (struct nlmsghdr* nlh = (struct nlmsghdr*)buffer;
                 NLMSG_OK(nlh, (unsigned int)len);
                 nlh = NLMSG_NEXT(nlh, len))
            {
                if (nlh->nlmsg_type == NLMSG_DONE)
                {
                    close(sock);
                    return neighbors;
                }

                if (nlh->nlmsg_type == NLMSG_ERROR)
                {
                    warning("Netlink error response received");
                    close(sock);
                    return neighbors;
                }

                if (nlh->nlmsg_type != RTM_NEWNEIGH)
                {
                    continue;
                }

                struct ndmsg* ndm = (struct ndmsg*)NLMSG_DATA(nlh);
                struct rtattr* rta = (struct rtattr*)((char*)ndm + NLMSG_ALIGN(sizeof(*ndm)));
                int rtlen = NLMSG_PAYLOAD(nlh, sizeof(*ndm));

                MCTPNeighbor neighbor;
                neighbor.eid = 0;
                neighbor.physAddr = 0;

                // Process attributes
                while (RTA_OK(rta, rtlen))
                {
                    if (rta->rta_type == NDA_DST)
                    {
                        // DST is the address (EID for MCTP)
                        if (RTA_PAYLOAD(rta) >= sizeof(neighbor.eid))
                        {
                            neighbor.eid = *(uint8_t*)RTA_DATA(rta);
                        }
                    }
                    else if (rta->rta_type == NDA_LLADDR)
                    {
                        // LLADDR is the physical address
                        if (RTA_PAYLOAD(rta) >= sizeof(neighbor.physAddr))
                        {
                            neighbor.physAddr = *(uint8_t*)RTA_DATA(rta);
                        }
                    }

                    rta = RTA_NEXT(rta, rtlen);
                }

                // Only include MCTP neighbors (check interface name)
                char ifname[IF_NAMESIZE];
                if (if_indextoname(ndm->ndm_ifindex, ifname) == nullptr)
                {
                    continue;
                }

                std::string devName(ifname);
                if (devName.find("mctpi2c") != 0)
                {
                    continue;
                }

                neighbor.device = devName;

                // Only add if we have meaningful data
                if (neighbor.eid > 0 || neighbor.physAddr > 0)
                {
                    neighbors.push_back(neighbor);
                    debug("Found neighbor: EID={EID}, Device={DEVICE}, PhysAddr=0x{ADDR}",
                          "EID", neighbor.eid, "DEVICE", neighbor.device, "ADDR",
                          std::format("{:02x}", neighbor.physAddr));
                }
            }
        }

        close(sock);
    }
    catch (const std::exception& e)
    {
        warning("Exception in getNeighborsViaNetlink: {EXCEPTION}", "EXCEPTION",
                e);
    }

    return neighbors;
}

bool MCTPI2CDiscovery::neighborExistsViaNetlink(const std::string& hexAddr, const std::string& ifname)
{
    try
    {
        std::uint8_t targetAddr = std::stoi(hexAddr, nullptr, 16);
        std::vector<MCTPNeighbor> neighbors = getNeighborsViaNetlink();

        for (const auto& neighbor : neighbors)
        {
            if (neighbor.device == ifname && neighbor.physAddr == targetAddr)
            {
                return true;
            }
        }
    }
    catch (const std::exception& e)
    {
        warning("Exception checking neighbor: {EXCEPTION}", "EXCEPTION", e);
    }

    return false;
}

bool MCTPI2CDiscovery::isDeviceExistsInDBus(std::uint8_t busNum,
                                               const std::string& hexAddr)
{
    try
    {
        // Get all endpoints under /au/com/codeconstruct/mctp1/networks/1/endpoints
        std::string endpointsPath = std::string(mctp::dbus::basePath) + "/networks/" +
                                    std::to_string(i2cNet) + "/endpoints";

        auto method = bus->new_method_call(mctp::dbus::service.data(),
                                            std::string(mctp::dbus::basePath).c_str(),
                                            "org.freedesktop.DBus.ObjectManager",
                                            "GetManagedObjects");

        auto reply = bus->call(method);

        ManagedObjectType objects;
        reply.read(objects);

        std::uint8_t hexAddrDec = std::stoi(hexAddr, nullptr, 16);

        for (const auto& [objPath, interfaces] : objects)
        {
            std::string pathStr = objPath.str;

            if (pathStr.find(endpointsPath) != 0)
            {
                continue;
            }

            auto i2cIfaceIt = interfaces.find("xyz.openbmc_project.Inventory.Decorator.I2CDevice");
            if (i2cIfaceIt == interfaces.end())
            {
                continue;
            }

            const auto& i2cProps = i2cIfaceIt->second;

            auto busIt = i2cProps.find("Bus");
            auto addrIt = i2cProps.find("Address");

            if (busIt != i2cProps.end() && addrIt != i2cProps.end())
            {
                try
                {
                    std::uint8_t propBus =
                        std::get<std::uint8_t>(busIt->second);
                    std::uint8_t propAddr =
                        std::get<std::uint8_t>(addrIt->second);

                    if (propBus == busNum && propAddr == hexAddrDec)
                    {
                        return true;
                    }
                }
                catch (const std::exception&)
                {
                    // Type conversion failed, continue
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        warning("Failed to check D-Bus endpoints: {EXCEPTION}", "EXCEPTION", e);
    }

    return false;
}


void MCTPI2CDiscovery::performARPAssignment()
{
    debug("=== Phase 2: Dynamic ARP Assignment ===");

    std::vector<std::string> devices = getLinksViaNetlink("mctpi2c");
    if (devices.empty())
    {
        debug("No MCTP links found for ARP");
        return;
    }

    for (const auto& device : devices)
    {
        std::string busStr = device.substr(7);
        std::uint8_t busNum = static_cast<std::uint8_t>(std::stoi(busStr));

        if (busNum < minBusNum)
        {
            continue;
        }

        if (isBusARPProcessed(busNum))
        {
            debug("Bus {BUS} already processed by ARP, skipping", "BUS", busNum);
            continue;
        }

        performARPOnBus(busNum, device);
    }
}

bool MCTPI2CDiscovery::performARPOnBus(std::uint8_t busNum,
                                          const std::string& device)
{
    debug("Checking for ARP device at 0x{ADDR} on bus {BUS}", "ADDR",
         std::format("{:02x}", arpDefaultAddress), "BUS", busNum);

    // Check if device exists at default address
    std::string probePath = "/dev/i2c-" + std::to_string(busNum);
    int probeFd = open(probePath.c_str(), O_RDWR);
    bool found = false;
    if (probeFd >= 0)
    {
        found = probeI2CAddress(probeFd, arpDefaultAddress);
        close(probeFd);
    }
    if (!found)
    {
        debug("No device at default address on bus {BUS}", "BUS", busNum);
        return false;
    }

    debug("Found device at default address on bus {BUS}. Starting ARP...",
         "BUS", busNum);

    try
    {
        // Get next available address
        std::uint8_t nextAddr = getNextARPAddress();

        // Send prepare commands
        std::vector<std::uint8_t> cmd1 = {0x01};
        std::vector<std::uint8_t> cmd2 = {0x02};

        if (!i2cWrite(busNum, arpDefaultAddress, cmd1) ||
            !i2cWrite(busNum, arpDefaultAddress, cmd2))
        {
            warning("Failed to send ARP prepare commands on bus {BUS}", "BUS",
                    busNum);
            return false;
        }

        // Read 19 bytes (17-byte UDID + 18th byte with shifted address)
        std::vector<std::uint8_t> readCmd = {0x03};
        std::vector<std::uint8_t> arpData;
        arpData.resize(19);

        if (!i2cWriteRead(busNum, arpDefaultAddress, readCmd, arpData))
        {
            warning("Failed to read UDID from bus {BUS}", "BUS", busNum);
            return false;
        }

        // Extract 17-byte UDID (first 17 bytes)
        std::vector<std::uint8_t> udid(arpData.begin(), arpData.begin() + 17);

        debug("Got UDID from bus {BUS}, performing assignment to 0x{ADDR}", "BUS",
             busNum, "ADDR", std::format("{:02x}", nextAddr));

        // Prepare assignment command: 0x04 + 17-byte UDID + 1-byte shifted address
        std::vector<std::uint8_t> assignCmd;
        assignCmd.push_back(0x04);
        assignCmd.insert(assignCmd.end(), udid.begin(), udid.end());

        // Add shifted address (7-bit address << 1 | 1)
        std::uint8_t shiftedAddr = (nextAddr << 1) | 1;
        assignCmd.push_back(shiftedAddr);

        if (!i2cWrite(busNum, arpDefaultAddress, assignCmd))
        {
            warning("Failed to send ARP assignment command on bus {BUS}",
                    "BUS", busNum);
            return false;
        }

        // Wait a bit for device to settle
        usleep(100000); // 100ms

        // Verify by reading from new address
        std::vector<std::uint8_t> verifyCmd = {shiftedAddr};
        std::vector<std::uint8_t> verifyData;
        verifyData.resize(19);

        if (!i2cWriteRead(busNum, arpDefaultAddress, verifyCmd, verifyData))
        {
            warning("Failed to verify assignment on bus {BUS}", "BUS", busNum);
            return false;
        }

        // Compare UDIDs
        std::vector<std::uint8_t> verifyUdid(verifyData.begin(),
                                              verifyData.begin() + 17);
        if (udid != verifyUdid)
        {
            warning("UDID mismatch after assignment on bus {BUS}", "BUS",
                    busNum);
            return false;
        }

        debug("Verified assignment on bus {BUS}. Assigning endpoint to 0x{ADDR}",
             "BUS", busNum, "ADDR", std::format("{:02x}", nextAddr));

        // Assign endpoint via D-Bus using parent class method
        std::string hexAddr = std::format("{:02x}", nextAddr);
        std::vector<std::uint8_t> addrVector;
        addrVector.push_back(nextAddr);

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
        manageDeviceViaReactor(busNum, nextAddr, device);
        // Update next address and mark bus as processed
        setNextARPAddress(nextAddr + 1);
        markBusARPProcessed(busNum);
        return true;
#else
        auto response = assignEndpoint(device, addrVector);

        if (response.eid != 0)
        {
            debug("Successfully assigned endpoint on bus {BUS} at 0x{ADDR}: EID={EID}",
                 "BUS", busNum, "ADDR", hexAddr, "EID", static_cast<int>(response.eid));

            // Update next address and mark bus as processed
            setNextARPAddress(nextAddr + 1);
            markBusARPProcessed(busNum);
            return true;
        }
        else
        {
            warning("Failed to assign endpoint on bus {BUS}", "BUS", busNum);
            return false;
        }
#endif
    }
    catch (const std::exception& e)
    {
        warning("Exception during ARP assignment on bus {BUS}: {EXCEPTION}",
                "BUS", busNum, "EXCEPTION", e);
        return false;
    }
}

bool MCTPI2CDiscovery::isBusARPProcessed(std::uint8_t busNum)
{
    return processedBuses.find(busNum) != processedBuses.end();
}

void MCTPI2CDiscovery::markBusARPProcessed(std::uint8_t busNum)
{
    processedBuses.insert(busNum);
}

std::uint8_t MCTPI2CDiscovery::getNextARPAddress()
{
    return nextArpAddress;
}

void MCTPI2CDiscovery::setNextARPAddress(std::uint8_t addr)
{
    nextArpAddress = addr;
}

bool MCTPI2CDiscovery::i2cWrite(std::uint8_t busNum, std::uint8_t addr,
                                    const std::vector<std::uint8_t>& buffer)
{
    try
    {
        std::string devPath = "/dev/i2c-" + std::to_string(busNum);
        int fd = open(devPath.c_str(), O_RDWR);

        if (fd < 0)
        {
            warning("Failed to open I2C device {DEVICE}: {ERROR}", "DEVICE",
                    devPath, "ERROR", strerror(errno));
            return false;
        }

        // Make a mutable copy for ioctl
        std::vector<std::uint8_t> data_copy = buffer;

        struct i2c_msg msg = {
            .addr = addr,
            .flags = 0, // Write
            .len = static_cast<std::uint16_t>(buffer.size()),
            .buf = data_copy.data(),
        };

        struct i2c_rdwr_ioctl_data ioctl_data = {
            .msgs = &msg,
            .nmsgs = 1,
        };

        bool success = (ioctl(fd, I2C_RDWR, &ioctl_data) == 1);
        close(fd);
        return success;
    }
    catch (const std::exception& e)
    {
        warning(
            "Exception writing to I2C address 0x{ADDR} on bus {BUS}: {EXCEPTION}",
            "ADDR", std::format("{:02x}", addr), "BUS", busNum, "EXCEPTION", e);
        return false;
    }
}

bool MCTPI2CDiscovery::i2cWriteRead(std::uint8_t busNum, std::uint8_t addr,
                                        const std::vector<std::uint8_t>& writeBuffer,
                                        std::vector<std::uint8_t>& readBuffer)
{
    try
    {
        std::string devPath = "/dev/i2c-" + std::to_string(busNum);
        int fd = open(devPath.c_str(), O_RDWR);

        if (fd < 0)
        {
            warning("Failed to open I2C device {DEVICE}: {ERROR}", "DEVICE",
                    devPath, "ERROR", strerror(errno));
            return false;
        }

        // Make a mutable copy for ioctl
        std::vector<std::uint8_t> write_copy = writeBuffer;
        readBuffer.resize(readBuffer.size());

        struct i2c_msg msgs[2] = {
            {
                .addr = addr,
                .flags = 0, // Write
                .len = static_cast<std::uint16_t>(writeBuffer.size()),
                .buf = write_copy.data(),
            },
            {
                .addr = addr,
                .flags = I2C_M_RD,
                .len = static_cast<std::uint16_t>(readBuffer.size()),
                .buf = readBuffer.data(),
            },
        };

        struct i2c_rdwr_ioctl_data ioctl_data = {
            .msgs = msgs,
            .nmsgs = 2,
        };

        bool success = (ioctl(fd, I2C_RDWR, &ioctl_data) == 2);
        close(fd);
        return success;
    }
    catch (const std::exception& e)
    {
        warning(
            "Exception in I2C write-read on address 0x{ADDR} on bus {BUS}: {EXCEPTION}",
            "ADDR", std::format("{:02x}", addr), "BUS", busNum, "EXCEPTION", e);
        return false;
    }
}

bool MCTPI2CDiscovery::isEEPROMAddress(std::uint8_t addr) const
{
    // EEPROM address ranges that require special probing
    // Range 0x30-0x37 and 0x50-0x5F are typically EEPROM addresses
    return (addr >= 0x30 && addr <= 0x37) || (addr >= 0x50 && addr <= 0x5F);
}

bool MCTPI2CDiscovery::probeI2CAddress(int fd, std::uint8_t addr)
{
    try
    {
        // Based on SMBusBinding::scanPort probing logic
        if (ioctl(fd, I2C_SLAVE, addr) < 0)
        {
            return false; // Address not accessible
        }

        // For EEPROM addresses, use read method; for others, use write_quick
        if (isEEPROMAddress(addr))
        {
            // Try to read 1 byte from EEPROM
            if (i2c_smbus_read_byte(fd) < 0)
            {
                return false;
            }
        }
        else
        {
            // For regular devices, use quick write probe
            if (i2c_smbus_write_quick(fd, I2C_SMBUS_WRITE) < 0)
            {
                return false;
            }
        }

        debug("Advanced probe found responsive I2C device at 0x{ADDR}", "ADDR",
             std::format("{:02x}", addr));
        return true;
    }
    catch (const std::exception& e)
    {
        debug("Exception in advanced probe for address 0x{ADDR}: {EXCEPTION}",
              "ADDR", std::format("{:02x}", addr), "EXCEPTION", e);
        return false;
    }
}

void MCTPI2CDiscovery::setMuxIdleMode(const MuxIdleModes mode)
{
    namespace fs = std::filesystem;

    const std::string modeValue =
        (mode == MuxIdleModes::muxIdleModeConnect) ? "-1" : "-2";

    try
    {
        muxIdleModeMap.clear();

        for (const auto& link : getLinksViaNetlink("mctpi2c"))
        {
            if (link.size() <= 7)
            {
                continue;
            }

            std::string busStr;
            try
            {
                busStr = link.substr(7);
                std::stoi(busStr);
            }
            catch (const std::exception&)
            {
                continue;
            }

            fs::path idlePath = fs::path("/sys/bus/i2c/devices/i2c-" + busStr +
                                         "/mux_device/idle_state");

            std::error_code ec;
            if (!fs::exists(idlePath, ec))
            {
                continue;
            }

            std::string idlePathStr = idlePath.string();

            if (muxIdleModeMap.count(idlePathStr))
            {
                continue;
            }

            std::string current;
            {
                std::ifstream in(idlePath);
                if (in.good())
                {
                    std::getline(in, current);
                }
            }
            muxIdleModeMap[idlePathStr] = current;

            std::ofstream idleFile(idlePath);
            if (!idleFile.good())
            {
                warning("Failed to open mux idle mode file: {PATH}", "PATH",
                        idlePathStr);
                continue;
            }

            idleFile << modeValue;
            idleFile.close();

            if (idleFile.fail())
            {
                warning("Failed to write mux idle mode to {PATH}", "PATH",
                        idlePathStr);
            }
            else
            {
                debug("Set mux idle mode to {MODE} for {PATH} (was {PREV})",
                      "MODE", modeValue, "PATH", idlePathStr, "PREV",
                      current);
            }
        }

        debug("Applied mux idle mode {MODE} to {COUNT} path(s)", "MODE",
              modeValue, "COUNT", muxIdleModeMap.size());
    }
    catch (const std::exception& e)
    {
        warning("Exception setting mux idle mode: {EXCEPTION}", "EXCEPTION", e);
    }
}

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
void MCTPI2CDiscovery::manageDeviceViaReactor(std::uint8_t busNum,
                                               std::uint8_t addr,
                                               const std::string& ifaceName)
{
    if (!reactor)
    {
        warning("Reactor not set, falling back to assignEndpoint for "
                "0x{ADDR} on {IFACE}",
                "ADDR", std::format("{:02x}", addr), "IFACE", ifaceName);
        std::vector<std::uint8_t> addrVector = {addr};
        assignEndpoint(ifaceName, addrVector);
        return;
    }

    try
    {
        auto device = std::make_shared<I2CMCTPDDevice>(bus, busNum, addr);
        std::string path = std::format(
            "/xyz/openbmc_project/mctp/discovery/i2c/{}/{:02x}",
            busNum, addr);
        debug("Managing device via reactor: bus={BUS}, addr=0x{ADDR}, "
              "path={PATH}",
              "BUS", busNum, "ADDR", std::format("{:02x}", addr),
              "PATH", path);
        reactor->manageMCTPDevice(path, device);
    }
    catch (const std::exception& e)
    {
        warning("Failed to manage device via reactor: {ERROR}", "ERROR",
                e.what());
    }
}
#endif


