#pragma once

#include "MCTPEndpoint.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

class MCTPDeviceRepository
{
  private:
    // FIXME: Ugh, hack. Figure out a better data structure?
    std::map<std::string, std::shared_ptr<MCTPDevice>> devices;

    auto lookup(const std::shared_ptr<MCTPDevice>& device)
    {
        auto pred = [&device](const auto& it) { return it.second == device; };
        return std::ranges::find_if(devices, pred);
    }

    auto lookup(const std::shared_ptr<MCTPDevice>& device) const
    {
        auto pred = [&device](const auto& it) { return it.second == device; };
        return std::ranges::find_if(devices, pred);
    }

  public:
    MCTPDeviceRepository() = default;
    MCTPDeviceRepository(const MCTPDeviceRepository&) = delete;
    MCTPDeviceRepository(MCTPDeviceRepository&&) = delete;
    ~MCTPDeviceRepository() = default;

    MCTPDeviceRepository& operator=(const MCTPDeviceRepository&) = delete;
    MCTPDeviceRepository& operator=(MCTPDeviceRepository&&) = delete;

    void add(const std::string& inventory,
             const std::shared_ptr<MCTPDevice>& device)
    {
        auto [entry, fresh] = devices.emplace(inventory, device);
        if (!fresh && entry->second.get() != device.get())
        {
            throw std::system_error(
                std::make_error_code(std::errc::device_or_resource_busy),
                std::format("Tried to add entry for existing device: {}",
                            device->describe()));
        }
    }

    void remove(const std::shared_ptr<MCTPDevice>& device)
    {
        auto entry = lookup(device);
        if (entry == devices.end())
        {
            throw std::system_error(
                std::make_error_code(std::errc::no_such_device),
                std::format("Trying to remove unknown device: {}",
                            device->describe()));
        }
        devices.erase(entry);
    }

    [[nodiscard]] bool contains(const std::shared_ptr<MCTPDevice>& device) const
    {
        return lookup(device) != devices.end();
    }

    [[nodiscard]] std::optional<std::string> inventoryFor(
        const std::shared_ptr<MCTPDevice>& device) const
    {
        auto entry = lookup(device);
        if (entry == devices.end())
        {
            return {};
        }
        return entry->first;
    }

    [[nodiscard]] std::shared_ptr<MCTPDevice> deviceFor(
        const std::string& inventory) const
    {
        auto entry = devices.find(inventory);
        if (entry == devices.end())
        {
            return {};
        }
        return entry->second;
    }

    auto begin()
    {
        return devices.begin();
    }

    auto end()
    {
        return devices.end();
    }

    [[nodiscard]] auto begin() const
    {
        return devices.begin();
    }

    [[nodiscard]] auto end() const
    {
        return devices.end();
    }
};
