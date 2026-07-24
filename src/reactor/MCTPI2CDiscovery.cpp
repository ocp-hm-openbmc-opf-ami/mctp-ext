#include "MCTPI2CDiscovery.hpp"

#include "Utils.hpp"

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
#include "MCTPEndpoint.hpp"
#include "MCTPReactor.hpp"
#endif

#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/message.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
extern "C"
{
#include <i2c/smbus.h>
}
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/socket.h>

#ifndef AF_MCTP
#define AF_MCTP 45
#endif

PHOSPHOR_LOG2_USING;

// Forbidden I2C addresses (reserved, reserved, reserved, etc.)
const std::set<std::string> MCTPI2CDiscovery::forbiddenAddresses = {
    "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",  "a",  "b",  "c",
    "d",  "e",  "f",  "00", "01", "02", "03", "04", "05", "06", "07", "30",
    "3c", "50", "61", "78", "79", "7a", "7b", "7c", "7d", "7e", "7f"};

MCTPI2CDiscovery::MCTPI2CDiscovery(
    const std::shared_ptr<sdbusplus::asio::connection>& bus,
    const I2CDiscoveryConfig& cfg) :
    MCTPDiscovery(bus), config(cfg), i2cNet(cfg.i2cNet)
{
    if (!cfg.whitelist.empty())
    {
        whitelist = cfg.whitelist;
    }
}

void MCTPI2CDiscovery::run()
{
    debug("=== Starting I2C Discovery ===");

    // Ensure mux idle mode is set to connect; kernel may reset on deferred
    // probe
    ensureMuxIdleMode();

    // Clear per-cycle tracking
    staleArpAddresses.clear();
    arpAssignedThisCycle.clear();

    // Atomically create the busy file; O_EXCL fails if it already exists
    int busyFd = open(busyFilePath.data(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (busyFd < 0)
    {
        if (errno == EEXIST)
        {
            info("MCTPI2CDiscovery::run: {PATH} exists, skipping scan", "PATH",
                 busyFilePath);
        }
        else
        {
            warning("MCTPI2CDiscovery::run: failed to create {PATH}: {ERROR}",
                    "PATH", busyFilePath, "ERROR", strerror(errno));
        }
        return;
    }
    close(busyFd);

    // Phase 0: Validate existing routes FIRST (remove stale endpoints)
    validateExistingRoutes();

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

    std::filesystem::remove(busyFilePath);
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

        // Always ensure interface is up and local EID is configured,
        // even for buses below minBusNum — child MUX buses share the
        // same physical interface and need it ready.
        if (!ensureInterfaceReady(device, config.localEid, i2cNet,
                                  i2cDefaultMtu))
        {
            continue;
        }

        debug("Scanning bus {BUS_NUM} ({DEVICE})", "BUS_NUM", busNum, "DEVICE",
              device);

        // Scan I2C addresses via ioctl
        std::vector<std::uint8_t> detectedAddresses =
            scanI2CRange(busNum, 0x08, 0x77);

        for (std::uint8_t addr : detectedAddresses)
        {
            std::string hexAddr = std::format("{:02x}", addr);

            // Only scan addresses in the whitelist
            if (!isInWhitelist(hexAddr))
            {
                continue;
            }

            // Skip addresses that ARP has moved devices away from
            if (staleArpAddresses.count(hexAddr))
            {
                debug("Skipping stale ARP address 0x{HEX_ADDR} on bus "
                      "{BUS_NUM} (device was moved by ARP)",
                      "HEX_ADDR", hexAddr, "BUS_NUM", busNum);
                continue;
            }

            // Skip addresses that ARP already assigned and handled this cycle
            if (arpAssignedThisCycle.count(hexAddr))
            {
                debug("Skipping 0x{HEX_ADDR} on bus {BUS_NUM} "
                      "(already handled by ARP this cycle)",
                      "HEX_ADDR", hexAddr, "BUS_NUM", busNum);
                continue;
            }

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
                debug(
                    "Successfully assigned endpoint for 0x{HEX_ADDR}: EID={EID}",
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
            busNum =
                static_cast<std::uint8_t>(std::stoul(device.bus, nullptr, 0));
            addr = static_cast<std::uint8_t>(
                std::stoul(device.address, nullptr, 0));
        }
        catch (const std::exception& e)
        {
            warning("Invalid bus/address for device {NAME}: {ERR}", "NAME",
                    device.name, "ERR", e);
            continue;
        }

        std::string hexAddr = std::format("{:02x}", addr);
        std::string ifaceName =
            (busNum == 0) ? "mctpmbox0" : "mctpi2c" + std::to_string(busNum);

        // Ensure interface is up and local EID is configured
        if (!ensureInterfaceReady(ifaceName, config.localEid, i2cNet,
                                  i2cDefaultMtu))
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

        // Probe the I2C address to check if slave exists (only for I2C buses)
        if (busNum > 0)
        {
            std::string probePath = "/dev/i2c-" + std::to_string(busNum);
            int fd = open(probePath.c_str(), O_RDWR);
            if (fd < 0)
            {
                warning("Cannot open {PATH} for configured device {NAME}",
                        "PATH", probePath, "NAME", device.name);
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

            debug("Using static EID {EID} for configured device {NAME}", "EID",
                  staticEid, "NAME", device.name);

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
            std::uint8_t cfgBus =
                static_cast<std::uint8_t>(std::stoul(device.bus, nullptr, 0));
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

        req.msg.ndm_family =
            AF_MCTP;             // Must use AF_MCTP to query MCTP neighbors
        req.msg.ndm_ifindex = 0; // All interfaces

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
                 NLMSG_OK(nlh, (unsigned int)len); nlh = NLMSG_NEXT(nlh, len))
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
                struct rtattr* rta =
                    (struct rtattr*)((char*)ndm + NLMSG_ALIGN(sizeof(*ndm)));
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
                    debug(
                        "Found neighbor: EID={EID}, Device={DEVICE}, PhysAddr=0x{ADDR}",
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

bool MCTPI2CDiscovery::neighborExistsViaNetlink(const std::string& hexAddr,
                                                const std::string& ifname)
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

bool MCTPI2CDiscovery::removeNeighborByAddress(const std::string& ifname,
                                               const std::string& hexAddr)
{
    try
    {
        std::uint8_t targetAddr = std::stoi(hexAddr, nullptr, 16);
        std::vector<MCTPNeighbor> neighbors = getNeighborsViaNetlink();

        for (const auto& neighbor : neighbors)
        {
            if (neighbor.device == ifname && neighbor.physAddr == targetAddr &&
                neighbor.eid > 0)
            {
                debug("Removing stale neighbor EID={EID} at 0x{ADDR} on {DEV}",
                      "EID", neighbor.eid, "ADDR", hexAddr, "DEV", ifname);
                return removeEndpoint(i2cNet, neighbor.eid);
            }
        }
    }
    catch (const std::exception& e)
    {
        warning("Exception removing neighbor by address: {EXCEPTION}",
                "EXCEPTION", e);
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

        // Skip root buses (< 16) that have MUX children — ARP will run
        // on the child bus links instead.  Buses >= 16 (MUX child buses)
        // always get ARP.
        if (busNum < minBusNum && hasMuxChildren(busNum))
        {
            debug("Skipping ARP on root bus {BUS} ({DEV}): has MUX children",
                  "BUS", busNum, "DEV", device);
            continue;
        }

        performARPOnBus(busNum, device);
    }
}

bool MCTPI2CDiscovery::performARPOnBus(std::uint8_t busNum,
                                       const std::string& device)
{
    static constexpr int maxRetries = 3;

    debug("Checking for ARP device at 0x{ADDR} on bus {BUS}", "ADDR",
          std::format("{:02x}", arpDefaultAddress), "BUS", busNum);

    // Open I2C device and keep fd for the entire ARP sequence
    std::string devPath = "/dev/i2c-" + std::to_string(busNum);
    int fd = open(devPath.c_str(), O_RDWR);
    if (fd < 0)
    {
        warning("Failed to open {PATH}: {ERROR}", "PATH", devPath, "ERROR",
                strerror(errno));
        return false;
    }

    // Check if device exists at default ARP address
    bool found = probeI2CAddress(fd, arpDefaultAddress);
    if (!found)
    {
        debug("No device at default address on bus {BUS}", "BUS", busNum);
        close(fd);
        return false;
    }

    debug("Found device at default address on bus {BUS}. Starting ARP...",
          "BUS", busNum);

    // Enable PEC (Packet Error Checking) for SMBus data integrity
    if (ioctl(fd, I2C_PEC, 1) < 0)
    {
        warning(
            "Failed to enable PEC on bus {BUS}: {ERROR}, continuing without",
            "BUS", busNum, "ERROR", strerror(errno));
    }

    // Set slave address for SMBus operations
    if (ioctl(fd, I2C_SLAVE, arpDefaultAddress) < 0)
    {
        warning("Failed to set slave address 0x{ADDR} on bus {BUS}: {ERROR}",
                "ADDR", std::format("{:02x}", arpDefaultAddress), "BUS", busNum,
                "ERROR", strerror(errno));
        close(fd);
        return false;
    }

    try
    {
        // Step 1: Prepare ARP (0x01) and Reset Device (0x02)
        // Skip if this bus was already prepared in this session
        if (!isBusARPProcessed(busNum))
        {
            bool prepareOk = false;
            for (int retry = 0; retry < maxRetries && !prepareOk; ++retry)
            {
                if (i2c_smbus_write_byte(fd, 0x01) >= 0 &&
                    i2c_smbus_write_byte(fd, 0x02) >= 0)
                {
                    prepareOk = true;
                }
                else if (retry < maxRetries - 1)
                {
                    debug("ARP prepare retry {RETRY} on bus {BUS}", "RETRY",
                          retry + 1, "BUS", busNum);
                    usleep(10000); // 10ms between retries
                }
            }

            if (!prepareOk)
            {
                debug("Failed to send ARP prepare commands on bus {BUS} "
                      "after {RETRIES} retries",
                      "BUS", busNum, "RETRIES", maxRetries);
                close(fd);
                return false;
            }
            markBusARPProcessed(busNum);
        }
        else
        {
            debug("Bus {BUS} already prepared, skipping 0x01/0x02", "BUS",
                  busNum);
        }

        // Step 2: Get UDID with retry
        std::vector<std::uint8_t> udid;
        bool gotUdid = false;
        std::uint8_t originalAddrByte =
            0xFF; // Device's original address from UDID

        for (int retry = 0; retry < maxRetries && !gotUdid; ++retry)
        {
            std::vector<std::uint8_t> readCmd = {0x03};
            std::vector<std::uint8_t> arpData(19, 0);

            if (i2cWriteReadFd(fd, arpDefaultAddress, readCmd, arpData))
            {
                // Raw I2C read returns: [0]=block_count [1..16]=UDID [17]=addr
                // [18]=PEC Byte 17 contains the shifted slave address; 0xFF
                // means device hasn't been assigned yet (valid for ARP)
                if (arpData[17] != 0xFF || retry == maxRetries - 1)
                {
                    udid.assign(arpData.begin() + 1, arpData.begin() + 17);
                    originalAddrByte = arpData[17];
                    gotUdid = true;
                }
            }

            if (!gotUdid && retry < maxRetries - 1)
            {
                debug("Get UDID retry {RETRY} on bus {BUS}", "RETRY", retry + 1,
                      "BUS", busNum);
                usleep(10000);
            }
        }

        if (!gotUdid)
        {
            debug("Failed to read UDID from bus {BUS} after {RETRIES} retries",
                  "BUS", busNum, "RETRIES", maxRetries);
            close(fd);
            return false;
        }

        // Step 3: Validate ASF/MCTP support bit (Interface field bit 5).
        // The UDID Interface field is bits [79:64] = udid[6:7]; udid[7] holds
        // Interface[7:0] where bit 5 = ASF.  A device that doesn't set this
        // bit doesn't support MCTP; we keep its original slave address
        // instead of assigning a new one.
        bool mctpSupported = !(udid.size() > 7) || (udid[7] & 0x20);

        // Step 4: Determine address to assign
        std::uint8_t assignAddr = 0;
        std::uint8_t originalAddr = originalAddrByte >> 1;

        // Check UDID Device Capabilities byte (udid[0]) bits 6-5:
        //   00b = Fixed address
        //   01b = Dynamic and Persistent Slave Address (PSA)
        //   10b = Dynamic and volatile Slave Address (VSA)
        //   11b = Random number device
        std::uint8_t addrType = (udid[0] >> 6);
        bool isFixedAddr = (addrType == 0x00);

        // If the device's original address is in the ARP ignore list, keep
        // it as-is instead of assigning a new dynamic address.
        bool ignoreAddr = arpIgnoreAddresses.count(originalAddr) > 0;

        // Whether we kept the device's original address instead of
        // allocating a new dynamic one. Used to avoid advancing the dynamic
        // ARP address pointer for a non-dynamic assignment.
        bool keptOriginalAddr = false;

        if ((isFixedAddr || !mctpSupported || ignoreAddr) &&
            originalAddrByte != 0xFF && originalAddr > 0x07 &&
            originalAddr < 0x78 &&
            !isForbidden(std::format("{:02x}", originalAddr)))
        {
            // Device has a fixed/persistent address in UDID — keep it
            assignAddr = originalAddr;
            keptOriginalAddr = true;
            debug("Device on bus {BUS} has fixed address 0x{ADDR} "
                  "(UDID addr_type={TYPE}, mctp_supported={MCTP}, "
                  "ignored={IGNORE}), assigning current address",
                  "BUS", busNum, "ADDR", std::format("{:02x}", assignAddr),
                  "TYPE", addrType, "MCTP", mctpSupported, "IGNORE",
                  ignoreAddr);
        }
        else
        {
            // Find a free address with collision detection
            // Temporarily disable PEC for probing, then re-enable
            ioctl(fd, I2C_PEC, 0);

            std::uint8_t candidate = getNextARPAddress();

            for (; candidate <= 0x77; ++candidate)
            {
                std::string hexCandidate = std::format("{:02x}", candidate);
                if (isForbidden(hexCandidate))
                {
                    continue;
                }

                // Probe to verify address is not already occupied
                bool occupied = probeI2CAddress(fd, candidate);
                if (!occupied)
                {
                    assignAddr = candidate;
                    break;
                }
                else
                {
                    debug(
                        "Address 0x{ADDR} on bus {BUS} already occupied, skipping",
                        "ADDR", hexCandidate, "BUS", busNum);
                }
            }

            // Re-enable PEC and restore slave address for assignment
            ioctl(fd, I2C_PEC, 1);
            ioctl(fd, I2C_SLAVE, arpDefaultAddress);
        }

        if (assignAddr == 0)
        {
            warning("No free address available for ARP on bus {BUS}", "BUS",
                    busNum);
            close(fd);
            return false;
        }

        debug("Got UDID from bus {BUS}, assigning to 0x{ADDR}", "BUS", busNum,
              "ADDR", std::format("{:02x}", assignAddr));

        // Step 5: Assign address with retry (SMBus Block Write)
        std::uint8_t shiftedAddr = (assignAddr << 1) | 1;
        bool assigned = false;

        // Build assign data: 16-byte UDID + shifted address = 17 bytes
        std::vector<std::uint8_t> assignData;
        assignData.insert(assignData.end(), udid.begin(), udid.end());
        assignData.push_back(shiftedAddr);

        for (int retry = 0; retry < maxRetries && !assigned; ++retry)
        {
            if (i2c_smbus_write_block_data(fd, 0x04,
                                           static_cast<__u8>(assignData.size()),
                                           assignData.data()) >= 0)
            {
                assigned = true;
            }
            else if (retry < maxRetries - 1)
            {
                debug("ARP assign retry {RETRY} on bus {BUS}", "RETRY",
                      retry + 1, "BUS", busNum);
                usleep(10000);
            }
        }

        if (!assigned)
        {
            warning("Failed to send ARP assignment on bus {BUS} after "
                    "{RETRIES} retries",
                    "BUS", busNum, "RETRIES", maxRetries);
            close(fd);
            return false;
        }

        // Wait for device to settle
        usleep(100000); // 100ms

        // Step 6: Verify assignment with retry
        bool verified = false;

        for (int retry = 0; retry < maxRetries && !verified; ++retry)
        {
            std::vector<std::uint8_t> verifyCmd = {
                static_cast<std::uint8_t>((assignAddr << 1) | 0x01)};
            std::vector<std::uint8_t> verifyData(19, 0);

            if (i2cWriteReadFd(fd, arpDefaultAddress, verifyCmd, verifyData))
            {
                // Skip block count byte [0], compare UDID at [1..16]
                std::vector<std::uint8_t> verifyUdid(verifyData.begin() + 1,
                                                     verifyData.begin() + 17);
                if (udid == verifyUdid && (verifyData[17] >> 1) == assignAddr)
                {
                    verified = true;
                }
            }

            if (!verified && retry < maxRetries - 1)
            {
                debug("ARP verify retry {RETRY} on bus {BUS}", "RETRY",
                      retry + 1, "BUS", busNum);
                usleep(10000);
            }
        }

        if (!verified)
        {
            warning("Failed to verify ARP assignment on bus {BUS} after "
                    "{RETRIES} retries",
                    "BUS", busNum, "RETRIES", maxRetries);
            close(fd);
            return false;
        }

        close(fd);

        debug(
            "Verified assignment on bus {BUS}. Assigning endpoint to 0x{ADDR}",
            "BUS", busNum, "ADDR", std::format("{:02x}", assignAddr));

        // Clean up stale MCTP neighbor at device's original address
        // After ARP, the device has moved from originalAddr to assignAddr.
        // Any existing MCTP endpoint at the original address is now stale.
        if (originalAddr != assignAddr && originalAddr > 0x07 &&
            originalAddr < 0x78 &&
            !isForbidden(std::format("{:02x}", originalAddr)))
        {
            std::string origHexAddr = std::format("{:02x}", originalAddr);
            staleArpAddresses.insert(origHexAddr);
            if (removeNeighborByAddress(device, origHexAddr))
            {
                debug("Removed stale MCTP endpoint at original address "
                      "0x{OLD} on {DEV} (device moved to 0x{NEW})",
                      "OLD", origHexAddr, "DEV", device, "NEW",
                      std::format("{:02x}", assignAddr));
            }
        }

        // Update next address only when a dynamic address was allocated.
        // Keeping a device's original (fixed/ignored/non-MCTP) address must
        // not advance the dynamic ARP pointer, otherwise the next round is
        // seeded from an unrelated address.
        if (!keptOriginalAddr)
        {
            setNextARPAddress(assignAddr + 1);
        }

        // Add ARP-assigned address to whitelist so the scan phase can
        // rediscover the device if endpoint assignment fails here.
        std::string hexAddr = std::format("{:02x}", assignAddr);
        if (!isInWhitelist(hexAddr))
        {
            whitelist.insert(hexAddr);
            debug("Added ARP-assigned address 0x{ADDR} to whitelist", "ADDR",
                  hexAddr);
        }

        // Assign endpoint
        std::vector<std::uint8_t> addrVector = {assignAddr};

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
        manageDeviceViaReactor(busNum, assignAddr, device);
        return true;
#else
        auto response = assignEndpoint(device, addrVector);

        if (response.eid != 0)
        {
            debug("Successfully assigned endpoint on bus {BUS} at 0x{ADDR}: "
                  "EID={EID}",
                  "BUS", busNum, "ADDR", hexAddr, "EID",
                  static_cast<int>(response.eid));
            arpAssignedThisCycle.insert(hexAddr);
            return true;
        }
        else
        {
            warning("Failed to assign endpoint on bus {BUS}", "BUS", busNum);
            // Still mark as assigned-this-cycle to avoid Phase 3 redundant
            // retry
            arpAssignedThisCycle.insert(hexAddr);
            return false;
        }
#endif
    }
    catch (const std::exception& e)
    {
        warning("Exception during ARP assignment on bus {BUS}: {EXCEPTION}",
                "BUS", busNum, "EXCEPTION", e);
        close(fd);
        return false;
    }
}

bool MCTPI2CDiscovery::isBusARPProcessed(std::uint8_t busNum)
{
    // Tracks whether Prepare ARP (0x01) + Reset Device (0x02) have been
    // sent to this bus in this session.  Subsequent ARP attempts on the
    // same bus skip those commands to avoid resetting already-assigned devices.
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

bool MCTPI2CDiscovery::i2cWriteRead(
    std::uint8_t busNum, std::uint8_t addr,
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

bool MCTPI2CDiscovery::i2cWriteReadFd(
    int fd, std::uint8_t addr, const std::vector<std::uint8_t>& writeBuffer,
    std::vector<std::uint8_t>& readBuffer)
{
    // Make a mutable copy for ioctl
    std::vector<std::uint8_t> write_copy = writeBuffer;

    struct i2c_msg msgs[2] = {
        {
            .addr = addr,
            .flags = 0, // Write
            .len = static_cast<std::uint16_t>(write_copy.size()),
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

    return (ioctl(fd, I2C_RDWR, &ioctl_data) == 2);
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

bool MCTPI2CDiscovery::hasMuxChildren(std::uint8_t busNum) const
{
    namespace fs = std::filesystem;

    try
    {
        fs::path busPath = "/sys/bus/i2c/devices/i2c-" + std::to_string(busNum);
        if (!fs::exists(busPath))
        {
            return false;
        }

        for (const auto& entry : fs::directory_iterator(busPath))
        {
            std::string name = entry.path().filename().string();
            // Child buses created by a MUX are named "i2c-N"
            if (name.starts_with("i2c-") && name.size() > 4)
            {
                std::string suffix = name.substr(4);
                if (std::all_of(suffix.begin(), suffix.end(), ::isdigit))
                {
                    return true;
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        warning("Exception checking MUX children on bus {BUS}: {EXCEPTION}",
                "BUS", busNum, "EXCEPTION", e);
    }

    return false;
}

void MCTPI2CDiscovery::ensureMuxIdleMode()
{
    namespace fs = std::filesystem;

    const std::string desiredValue = "-1"; // muxIdleModeConnect
    bool anyChanged = false;
    std::set<std::string> processedRootBuses;

    try
    {
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

            // Walk up the mux hierarchy to find the root bus
            std::string currentBus = busStr;
            while (true)
            {
                fs::path muxDevLink = fs::path(
                    "/sys/bus/i2c/devices/i2c-" + currentBus + "/mux_device");

                std::error_code ec;
                if (!fs::exists(muxDevLink, ec) || ec)
                {
                    break; // currentBus is the root bus
                }

                fs::path resolvedMux = fs::canonical(muxDevLink, ec);
                if (ec)
                {
                    break;
                }

                // Extract parent bus from mux device name (e.g. "8-0070" ->
                // "8")
                std::string muxDevName = resolvedMux.filename().string();
                auto dashPos = muxDevName.find('-');
                if (dashPos == std::string::npos)
                {
                    break;
                }

                currentBus = muxDevName.substr(0, dashPos);
            }

            std::string rootBus = currentBus;

            // Skip if this root bus was already processed
            if (!processedRootBuses.insert(rootBus).second)
            {
                continue;
            }

            // Only set idle_state on MUX devices directly under the root bus.
            // MUX devices are named "{rootBus}-XXXX" and expose idle_state.
            std::string prefix = rootBus + "-";
            fs::path i2cDevicesPath("/sys/bus/i2c/devices");

            std::error_code dirEc;
            for (const auto& entry :
                 fs::directory_iterator(i2cDevicesPath, dirEc))
            {
                std::string devName = entry.path().filename().string();
                if (devName.substr(0, prefix.size()) != prefix)
                {
                    continue;
                }

                fs::path idlePath = entry.path() / "idle_state";
                std::error_code ec;
                if (!fs::exists(idlePath, ec) || ec)
                {
                    continue; // Not a mux device
                }

                std::string idlePathStr = idlePath.string();

                // Read current value
                std::string current;
                {
                    std::ifstream in(idlePath);
                    if (in.good())
                    {
                        std::getline(in, current);
                    }
                }

                // Only write if not already set to desired value
                if (current != desiredValue)
                {
                    std::ofstream idleFile(idlePath);
                    if (idleFile.good())
                    {
                        idleFile << desiredValue;
                        idleFile.close();

                        if (!idleFile.fail())
                        {
                            info("Set mux idle_state to {MODE} for {PATH} "
                                 "(was {PREV})",
                                 "MODE", desiredValue, "PATH", idlePathStr,
                                 "PREV", current);
                            anyChanged = true;
                        }
                    }
                }
            }
        }

        if (anyChanged)
        {
            info("Mux idle mode correction applied");
        }
    }
    catch (const std::exception& e)
    {
        warning("Exception in ensureMuxIdleMode: {EXCEPTION}", "EXCEPTION", e);
    }
}

#if REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED
void MCTPI2CDiscovery::manageDeviceViaReactor(
    std::uint8_t busNum, std::uint8_t addr, const std::string& ifaceName)
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
            "/xyz/openbmc_project/mctp/discovery/i2c/{}/{:02x}", busNum, addr);
        debug("Managing device via reactor: bus={BUS}, addr=0x{ADDR}, "
              "path={PATH}",
              "BUS", busNum, "ADDR", std::format("{:02x}", addr), "PATH", path);
        reactor->manageMCTPDevice(path, device);
    }
    catch (const std::exception& e)
    {
        warning("Failed to manage device via reactor: {ERROR}", "ERROR",
                e.what());
    }
}
#endif
