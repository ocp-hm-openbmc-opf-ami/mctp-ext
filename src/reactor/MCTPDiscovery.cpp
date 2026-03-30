
#include "MCTPDiscovery.hpp"
#include "MCTPConstants.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <format>
#include <linux/if_link.h>
#include <linux/mctp.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <errno.h>

#include <algorithm>
#include <mutex>

PHOSPHOR_LOG2_USING;

namespace
{
// Process-wide registry of live MCTPDiscovery instances. Pointers are
// raw on purpose: each instance manages its own lifetime via ctor/dtor
// of MCTPDiscovery, which (un)registers under the mutex below.
std::mutex& registryMutex()
{
    static std::mutex m;
    return m;
}

std::vector<MCTPDiscovery*>& registry()
{
    static std::vector<MCTPDiscovery*> v;
    return v;
}
} // namespace

MCTPDiscovery::MCTPDiscovery(
    const std::shared_ptr<sdbusplus::asio::connection>& connection) :
    bus(connection)
{
    std::lock_guard<std::mutex> lk(registryMutex());
    registry().push_back(this);
}

MCTPDiscovery::~MCTPDiscovery()
{
    std::lock_guard<std::mutex> lk(registryMutex());
    auto& v = registry();
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
}

void MCTPDiscovery::dispatchHostOn()
{
    std::vector<MCTPDiscovery*> snapshot;
    {
        std::lock_guard<std::mutex> lk(registryMutex());
        snapshot = registry();
    }
    for (auto* d : snapshot)
    {
        d->onHostOn();
    }
}

void MCTPDiscovery::dispatchHostOff()
{
    std::vector<MCTPDiscovery*> snapshot;
    {
        std::lock_guard<std::mutex> lk(registryMutex());
        snapshot = registry();
    }
    for (auto* d : snapshot)
    {
        d->onHostOff();
    }
}

void MCTPDiscovery::dispatchPlatformReset()
{
    std::vector<MCTPDiscovery*> snapshot;
    {
        std::lock_guard<std::mutex> lk(registryMutex());
        snapshot = registry();
    }
    for (auto* d : snapshot)
    {
        d->onPlatformReset();
    }
}

MCTPDiscovery::AssignEndpointResponse MCTPDiscovery::assignEndpoint(
    const std::string& interfaceName,
    const std::vector<std::uint8_t>& address)
{
    AssignEndpointResponse response = {0, 0, "", false};

    info("Calling AssignEndpoint on interface {NAME}", "NAME", interfaceName);

    try
    {
        std::string objPath = std::format("{}{}",
                                          mctp::dbus::interfacesPath,
                                          interfaceName);

        auto m = bus->new_method_call(
            std::string(mctp::dbus::service).c_str(),
            objPath.c_str(),
            std::string(mctp::dbus::busOwnerInterface).c_str(),
            "AssignEndpoint");
        m.append(address);

        auto reply = bus->call(m);

        reply.read(response.eid, response.networkId, response.interface,
                   response.probed);

        info("AssignEndpoint returned: EID={EID}, net={NET}, intf={INTF}",
             "EID", static_cast<int>(response.eid), "NET", response.networkId,
             "INTF", response.interface);

        return response;
    }
    catch (const std::exception& e)
    {
        warning("Failed to call AssignEndpoint on {NAME}: {ERROR}", "NAME",
                interfaceName, "ERROR", e.what());
        return response;
    }
}

bool MCTPDiscovery::removeEndpoint(std::uint16_t networkId, std::uint8_t eid)
{
    debug("Removing endpoint: network={NET}, EID={EID}", "NET", networkId,
          "EID", static_cast<int>(eid));

    try
    {
        std::string objPath = std::format(
            "{}/{}/endpoints/{}",
            mctp::dbus::networksPath, networkId, static_cast<int>(eid));

        auto m = bus->new_method_call(
            std::string(mctp::dbus::service).c_str(),
            objPath.c_str(),
            std::string(mctp::dbus::endpointInterface).c_str(),
            "Remove");
        bus->call(m);

        debug("Endpoint removed successfully: network={NET}, EID={EID}", "NET",
              networkId, "EID", static_cast<int>(eid));
        return true;
    }
    catch (const std::exception& e)
    {
        warning("Failed to remove endpoint: {ERROR}", "ERROR", e.what());
        return false;
    }
}

bool MCTPDiscovery::assignEndpointStatic(const std::string& device,
                                         std::uint8_t eid,
                                         const std::string& hexAddr)
{
    try
    {
        std::string ifacePath =
            std::string(mctp::dbus::basePath) + "/interfaces/" + device;

        auto method = bus->new_method_call(
            mctp::dbus::service.data(), ifacePath.c_str(),
            mctp::dbus::busOwnerInterface.data(), "AssignEndpointStatic");

        std::vector<std::uint8_t> addr;
        addr.push_back(std::stoi(hexAddr, nullptr, 16));

        method.append(addr);
        method.append(eid);

        bus->call(method);
        return true;
    }
    catch (const std::exception& e)
    {
        warning(
            "Failed to assign static endpoint {EID} for device {DEVICE}: {EXCEPTION}",
            "EID", eid, "DEVICE", device, "EXCEPTION", e);
        return false;
    }
}

bool MCTPDiscovery::ensureInterfaceReady(const std::string& interfaceName,
                                         uint8_t eid, int net)
{
    unsigned int ifIdx = if_nametoindex(interfaceName.c_str());
    if (ifIdx == 0)
    {
        warning("ensureInterfaceReady: interface {INTF} not found",
                "INTF", interfaceName);
        return false;
    }

    int ifIndex = static_cast<int>(ifIdx);
    bool isUp = false;
    bool eidMatched = false;
    std::vector<uint8_t> otherEids;

    // Single netlink socket for all operations
    int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sock < 0)
    {
        error("ensureInterfaceReady: socket creation failed");
        return false;
    }

    struct sockaddr_nl nlAddr = {};
    nlAddr.nl_family = AF_NETLINK;

    // Step 1: Check IFF_UP via RTM_GETLINK
    {
        struct {
            struct nlmsghdr nh;
            struct ifinfomsg ifmsg;
        } req = {};

        req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifmsg));
        req.nh.nlmsg_type = RTM_GETLINK;
        req.nh.nlmsg_flags = NLM_F_REQUEST;
        req.ifmsg.ifi_index = ifIndex;

        if (sendto(sock, &req, req.nh.nlmsg_len, 0,
                   reinterpret_cast<struct sockaddr*>(&nlAddr),
                   sizeof(nlAddr)) >= 0)
        {
            char buf[4096];
            ssize_t len = recv(sock, buf, sizeof(buf), 0);
            if (len > 0)
            {
                auto* nlh = reinterpret_cast<struct nlmsghdr*>(buf);
                if (NLMSG_OK(nlh, static_cast<size_t>(len)) &&
                    nlh->nlmsg_type == RTM_NEWLINK)
                {
                    auto* ifm = reinterpret_cast<struct ifinfomsg*>(
                        NLMSG_DATA(nlh));
                    isUp = (ifm->ifi_flags & IFF_UP) != 0;
                }
            }
        }
    }

    // Step 2: Check local EID via RTM_GETADDR dump
    {
        struct {
            struct nlmsghdr nh;
            struct ifaddrmsg ifmsg;
        } areq = {};

        areq.nh.nlmsg_len = NLMSG_LENGTH(sizeof(areq.ifmsg));
        areq.nh.nlmsg_type = RTM_GETADDR;
        areq.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
        areq.nh.nlmsg_seq = 2;
        areq.ifmsg.ifa_family = AF_MCTP;

        if (sendto(sock, &areq, areq.nh.nlmsg_len, 0,
                   reinterpret_cast<struct sockaddr*>(&nlAddr),
                   sizeof(nlAddr)) >= 0)
        {
            char abuf[4096];
            bool done = false;
            while (!done)
            {
                ssize_t alen = recv(sock, abuf, sizeof(abuf), 0);
                if (alen <= 0)
                    break;

                for (auto* nlh = reinterpret_cast<struct nlmsghdr*>(abuf);
                     NLMSG_OK(nlh, static_cast<size_t>(alen));
                     nlh = NLMSG_NEXT(nlh, alen))
                {
                    if (nlh->nlmsg_type == NLMSG_DONE)
                    {
                        done = true;
                        break;
                    }
                    if (nlh->nlmsg_type == NLMSG_ERROR ||
                        nlh->nlmsg_type != RTM_NEWADDR)
                    {
                        done = (nlh->nlmsg_type == NLMSG_ERROR);
                        continue;
                    }

                    auto* ifa = reinterpret_cast<struct ifaddrmsg*>(
                        NLMSG_DATA(nlh));
                    if (static_cast<int>(ifa->ifa_index) != ifIndex)
                        continue;

                    int rtaLen = IFA_PAYLOAD(nlh);
                    for (auto* attr = IFA_RTA(ifa); RTA_OK(attr, rtaLen);
                         attr = RTA_NEXT(attr, rtaLen))
                    {
                        if (attr->rta_type == IFA_LOCAL &&
                            RTA_PAYLOAD(attr) >= sizeof(uint8_t))
                        {
                            uint8_t localEid = 0;
                            memcpy(&localEid, RTA_DATA(attr),
                                   sizeof(localEid));
                            if (localEid == eid)
                            {
                                eidMatched = true;
                            }
                            else
                            {
                                otherEids.push_back(localEid);
                            }
                        }
                    }
                }
            }
        }
    }

    // Remove other local EIDs that don't match the expected one
    for (uint8_t other : otherEids)
    {
        struct {
            struct nlmsghdr nh;
            struct ifaddrmsg ifmsg;
            struct rtattr rta;
            uint8_t data[4];
        } dreq = {};

        dreq.nh.nlmsg_type = RTM_DELADDR;
        dreq.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        dreq.ifmsg.ifa_index = ifIndex;
        dreq.ifmsg.ifa_family = AF_MCTP;
        dreq.rta.rta_type = IFA_LOCAL;
        dreq.rta.rta_len = RTA_LENGTH(sizeof(other));
        memcpy(RTA_DATA(&dreq.rta), &other, sizeof(other));
        dreq.nh.nlmsg_len =
            NLMSG_LENGTH(sizeof(dreq.ifmsg)) + RTA_SPACE(sizeof(other));

        if (sendto(sock, &dreq, dreq.nh.nlmsg_len, 0,
                   reinterpret_cast<struct sockaddr*>(&nlAddr),
                   sizeof(nlAddr)) < 0)
        {
            warning("ensureInterfaceReady: failed to remove stale EID {EID} "
                    "from {INTF}",
                    "EID", lg2::hex, other, "INTF", interfaceName);
        }
        else
        {
            info("ensureInterfaceReady: removed stale EID {EID} from {INTF}",
                 "EID", lg2::hex, other, "INTF", interfaceName);
        }
    }

    // Already up with correct EID — nothing to do
    if (isUp && eidMatched)
    {
        debug("ensureInterfaceReady: {INTF} is up with EID {EID}",
              "INTF", interfaceName, "EID", lg2::hex, eid);
        close(sock);
        return true;
    }

    info("ensureInterfaceReady: {INTF} not ready (up={UP}, eidMatch={MATCH}), "
         "configuring",
         "INTF", interfaceName, "UP", isUp, "MATCH", eidMatched);

    // Step 3: Bring interface up (RTM_NEWLINK with IFF_UP)
    if (!isUp)
    {
        struct {
            struct nlmsghdr nh;
            struct ifinfomsg ifmsg;
        } req = {};

        req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifmsg));
        req.nh.nlmsg_type = RTM_NEWLINK;
        req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        req.ifmsg.ifi_index = ifIndex;
        req.ifmsg.ifi_change = IFF_UP;
        req.ifmsg.ifi_flags = IFF_UP;

        if (sendto(sock, &req, req.nh.nlmsg_len, 0,
                   reinterpret_cast<struct sockaddr*>(&nlAddr),
                   sizeof(nlAddr)) < 0)
        {
            error("ensureInterfaceReady: send RTM_NEWLINK (up) failed for {INTF}",
                  "INTF", interfaceName);
            close(sock);
            return false;
        }
    }

    // Step 4: Add MCTP address (RTM_NEWADDR) if EID not present
    if (!eidMatched)
    {
        struct {
            struct nlmsghdr nh;
            struct ifaddrmsg ifmsg;
            struct rtattr rta;
            uint8_t data[4];
        } req = {};

        req.nh.nlmsg_type = RTM_NEWADDR;
        req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        req.ifmsg.ifa_index = ifIndex;
        req.ifmsg.ifa_family = AF_MCTP;
        req.rta.rta_type = IFA_LOCAL;
        req.rta.rta_len = RTA_LENGTH(sizeof(eid));
        memcpy(RTA_DATA(&req.rta), &eid, sizeof(eid));
        req.nh.nlmsg_len =
            NLMSG_LENGTH(sizeof(req.ifmsg)) + RTA_SPACE(sizeof(eid));

        int ret = sendto(sock, &req, req.nh.nlmsg_len, 0,
                         reinterpret_cast<struct sockaddr*>(&nlAddr),
                         sizeof(nlAddr));
        if (ret < 0)
        {
            error("ensureInterfaceReady: send RTM_NEWADDR failed for EID {EID}",
                  "EID", lg2::hex, eid);
            close(sock);
            return false;
        }
        if (ret != static_cast<int>(req.nh.nlmsg_len))
        {
            error("ensureInterfaceReady: RTM_NEWADDR sendto short");
            close(sock);
            return false;
        }
    }

    // Step 5: Set MCTP network on link (RTM_NEWLINK with nested attrs)
    {
        struct {
            struct nlmsghdr nh;
            struct ifinfomsg ifmsg;
            uint8_t attrBuf[128];
        } req = {};

        req.nh.nlmsg_type = RTM_NEWLINK;
        req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        req.ifmsg.ifi_index = ifIndex;
        req.ifmsg.ifi_change = IFF_UP;
        req.ifmsg.ifi_flags = IFF_UP;
        req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifmsg));

        uint32_t mctpNet = static_cast<uint32_t>(net);

        // Build nested: IFLA_AF_SPEC { AF_MCTP { IFLA_MCTP_NET } }

        // Inner: IFLA_MCTP_NET
        uint8_t innerBuf[32];
        auto* innerRta = reinterpret_cast<struct rtattr*>(innerBuf);
        innerRta->rta_type = IFLA_MCTP_NET;
        innerRta->rta_len = RTA_LENGTH(sizeof(mctpNet));
        memcpy(RTA_DATA(innerRta), &mctpNet, sizeof(mctpNet));
        size_t innerLen = RTA_SPACE(sizeof(mctpNet));

        // Middle: AF_MCTP nested container
        uint8_t midBuf[64];
        auto* midRta = reinterpret_cast<struct rtattr*>(midBuf);
        midRta->rta_type = AF_MCTP | NLA_F_NESTED;
        midRta->rta_len =
            static_cast<unsigned short>(RTA_LENGTH(innerLen));
        memcpy(RTA_DATA(midRta), innerBuf, innerLen);
        size_t midLen = RTA_SPACE(innerLen);

        // Outer: IFLA_AF_SPEC
        auto* outerRta = reinterpret_cast<struct rtattr*>(
            reinterpret_cast<uint8_t*>(&req) + req.nh.nlmsg_len);
        outerRta->rta_type = IFLA_AF_SPEC | NLA_F_NESTED;
        outerRta->rta_len =
            static_cast<unsigned short>(RTA_LENGTH(midLen));
        memcpy(RTA_DATA(outerRta), midBuf, midLen);
        req.nh.nlmsg_len += RTA_SPACE(midLen);

        int ret = sendto(sock, &req, req.nh.nlmsg_len, 0,
                         reinterpret_cast<struct sockaddr*>(&nlAddr),
                         sizeof(nlAddr));
        if (ret < 0)
        {
            error("ensureInterfaceReady: send RTM_NEWLINK failed for net {NET}",
                  "NET", net);
            close(sock);
            return false;
        }
        if (ret != static_cast<int>(req.nh.nlmsg_len))
        {
            error("ensureInterfaceReady: RTM_NEWLINK sendto short");
            close(sock);
            return false;
        }
    }

    close(sock);
    info("ensureInterfaceReady: configured EID {EID} on {INTF} net {NET}",
         "EID", lg2::hex, eid, "INTF", interfaceName, "NET", net);
    return true;
}

// Remove all endpoints by enumerating D-Bus endpoint objects and calling Remove
bool MCTPDiscovery::removeAllEndpoint()
{
    using namespace mctp::dbus;
    bool allSuccess = true;
    try
    {
        auto m = bus->new_method_call(
            std::string(service).c_str(),
            std::string(basePath).c_str(),
            "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects");
        auto reply = bus->call(m);

        std::map<sdbusplus::message::object_path, std::map<std::string, std::map<std::string, std::variant<std::vector<uint8_t>, uint8_t, uint16_t, std::string, bool, int32_t>>>> objects;
        reply.read(objects);

        for (const auto& [objPath, ifaces] : objects)
        {
            // Only consider objects implementing Endpoint1
            if (!ifaces.count(std::string(endpointInterface)))
            {
                continue;
            }

            info("Removing endpoint at {PATH}", "PATH",
                 std::string(objPath));

            try
            {
                auto rm = bus->new_method_call(
                    std::string(service).c_str(),
                    objPath.str.c_str(),
                    std::string(endpointInterface).c_str(),
                    "Remove");
                bus->call(rm);
            }
            catch (const sdbusplus::exception_t& e)
            {
                // Local endpoints cannot be removed - this is expected
                std::string err(e.what());
                if (err.find("local") != std::string::npos)
                {
                    info("Skipping local endpoint {PATH}", "PATH",
                         std::string(objPath));
                }
                else
                {
                    warning("Failed to remove endpoint {PATH}: {ERROR}",
                            "PATH", std::string(objPath), "ERROR", e.what());
                    allSuccess = false;
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        warning("Failed to enumerate or remove endpoints: {ERR}", "ERR", e.what());
        return false;
    }
    return allSuccess;
}

std::vector<std::string> MCTPDiscovery::getLinksViaNetlink(
    const std::string& prefix)
{
    std::vector<std::string> links;

    try
    {
        int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        if (sock < 0)
        {
            warning("Failed to create netlink socket for links: {ERROR}",
                    "ERROR", strerror(errno));
            return links;
        }

        // Bind to netlink socket
        struct sockaddr_nl local = {};
        local.nl_family = AF_NETLINK;
        local.nl_groups = 0;

        if (bind(sock, (struct sockaddr*)&local, sizeof(local)) < 0)
        {
            warning("Failed to bind netlink socket for links: {ERROR}", "ERROR",
                    strerror(errno));
            close(sock);
            return links;
        }

        // Prepare RTM_GETLINK request
        struct
        {
            struct nlmsghdr hdr;
            struct ifinfomsg msg;
        } req = {};

        req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
        req.hdr.nlmsg_type = RTM_GETLINK;
        req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
        req.hdr.nlmsg_seq = 1;
        req.hdr.nlmsg_pid = getpid();

        req.msg.ifi_family = AF_UNSPEC; // Get all families

        // Send request
        struct sockaddr_nl peer = {};
        peer.nl_family = AF_NETLINK;

        if (sendto(sock, &req, req.hdr.nlmsg_len, 0, (struct sockaddr*)&peer,
                   sizeof(peer)) < 0)
        {
            warning("Failed to send netlink request for links: {ERROR}",
                    "ERROR", strerror(errno));
            close(sock);
            return links;
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
                warning("Failed to receive netlink response for links: {ERROR}",
                        "ERROR", strerror(errno));
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
                    return links;
                }

                if (nlh->nlmsg_type == NLMSG_ERROR)
                {
                    warning("Netlink error response received for links");
                    close(sock);
                    return links;
                }

                if (nlh->nlmsg_type != RTM_NEWLINK)
                {
                    continue;
                }

                struct ifinfomsg* ifinfo = (struct ifinfomsg*)NLMSG_DATA(nlh);
                struct rtattr* rta =
                    (struct rtattr*)((char*)ifinfo + NLMSG_ALIGN(sizeof(*ifinfo)));
                int rtlen = NLMSG_PAYLOAD(nlh, sizeof(*ifinfo));

                std::string ifname;

                // Process attributes to find interface name
                while (RTA_OK(rta, rtlen))
                {
                    if (rta->rta_type == IFLA_IFNAME)
                    {
                        ifname = std::string((char*)RTA_DATA(rta));
                        break;
                    }
                    rta = RTA_NEXT(rta, rtlen);
                }

                // Filter for MCTP interfaces matching the prefix
                if (!ifname.empty() && ifname.find(prefix) == 0)
                {
                    links.push_back(ifname);
                    debug("Found MCTP link: {LINK}", "LINK", ifname);
                }
            }
        }

        close(sock);
    }
    catch (const std::exception& e)
    {
        warning("Exception in getLinksViaNetlink: {EXCEPTION}", "EXCEPTION", e);
    }

    return links;
}
