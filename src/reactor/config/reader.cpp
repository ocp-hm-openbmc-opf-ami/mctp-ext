/*
// Copyright (c) 2025 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "reader.hpp"

#include "constants.hpp"

#include <boost/asio/detached.hpp>
#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

using mctp::config::Reader;
using namespace mctp::config::property;

static mctp::types::PhysicalLinkType
    toPhysicalLinkType(const std::string_view linkType);
static mctp::types::Role toRole(const std::string_view role);
static mctp::types::ResetEvent toResetEvent(const std::string_view event);
static std::shared_ptr<mctp::config::Configuration>
    createConfig(const mctp::types::PhysicalLinkType linkType);

Reader::Reader(std::shared_ptr<mctp::dbus::wrappers::ConfigWrapper> dbusWrapper,
               Callback callback) : callback(callback), dbusWrapper(dbusWrapper)
{
}

void Reader::readMCTPConfigurations(boost::asio::yield_context yield)
{
    this->dbusWrapper->getMCTPConfigurations(
        yield, std::bind_front(&Reader::processMCTPConfigObject, this));
}

void Reader::processMCTPConfigObject(boost::asio::yield_context yield,
                                     const std::string& objectPath)
{
    if (!callback)
    {
        lg2::warning(
            "No callback function provided to process MCTP configuration");
        return;
    }

    constexpr size_t defaultCtrlCmdRetries = 3;
    std::shared_ptr<mctp::config::Configuration> config;
    try
    {
        lg2::info("Processing MCTP config {PATH}", "PATH", objectPath);
        dbus::wrappers::ConfigWrapper::ConfigurationField confVal;
        auto isValid =
            [](dbus::wrappers::ConfigWrapper::ConfigurationField& val) {
                return !std::holds_alternative<std::monostate>(val);
            };

        confVal = dbusWrapper->getConfigurationField(
            yield, objectPath, physicalLinkType.data(), true);
        auto physicalLinkType =
            toPhysicalLinkType(std::get<std::string>(confVal));

        config = createConfig(physicalLinkType);
        confVal = dbusWrapper->getConfigurationField(yield, objectPath,
                                                     name.data(), true);
        config->configName = std::get<std::string>(confVal);

        confVal = dbusWrapper->getConfigurationField(
            yield, objectPath, physicalLinkName.data(), true);
        config->physicalLinkName = std::get<std::string>(confVal);

        confVal = dbusWrapper->getConfigurationField(yield, objectPath,
                                                     priority.data(), false);
        if (isValid(confVal))
        {
            config->priority =
                static_cast<uint8_t>(std::get<uint64_t>(confVal));
            // Only override the default-initialized priority when a valid
            // configuration value is provided.
        }

        confVal = dbusWrapper->getConfigurationField(yield, objectPath,
                                                     networkID.data(), true);
        config->networkID = std::get<uint64_t>(confVal);

        confVal = dbusWrapper->getConfigurationField(yield, objectPath,
                                                     role.data(), true);
        config->role = toRole(std::get<std::string>(confVal));

        confVal = dbusWrapper->getConfigurationField(yield, objectPath,
                                                     mtu.data(), false);
        if (isValid(confVal))
        {
            config->mtu = static_cast<uint32_t>(std::get<uint64_t>(confVal));
        }

        confVal = dbusWrapper->getConfigurationField(yield, objectPath,
                                                     retryCount.data(), false);
        if (isValid(confVal))
        {
            config->retryCount =
                static_cast<size_t>(std::get<uint64_t>(confVal));
        }
        if (config->retryCount == 0)
        {
            config->retryCount = defaultCtrlCmdRetries;
        }

        int timeoutMs = 300;
        confVal = dbusWrapper->getConfigurationField(yield, objectPath,
                                                     timeOut.data(), false);
        if (isValid(confVal))
        {
            timeoutMs = static_cast<int>(std::get<uint64_t>(confVal));
        }
        config->timeOut = std::chrono::milliseconds(timeoutMs);

        confVal = dbusWrapper->getConfigurationField(yield, objectPath,
                                                     resetEvents.data(), false);
        if (isValid(confVal))
        {
            auto events = std::get<std::vector<std::string>>(confVal);
            for (const auto& event : events)
            {
                config->resetEvents.push_back(toResetEvent(event));
            }
        }

        if (config->role == mctp::types::Role::topmostBusOwner)
        {
            confVal = dbusWrapper->getConfigurationField(yield, objectPath,
                                                         eidPool.data(), false);
            if (isValid(confVal))
            {
                auto eidPool = std::get<std::vector<uint64_t>>(confVal);
                config->eidPool =
                    std::vector<uint8_t>(eidPool.begin(), eidPool.end());
            }

            confVal = dbusWrapper->getConfigurationField(yield, objectPath,
                                                         ownEID.data(), false);
            if (isValid(confVal))
            {
                config->ownEID =
                    static_cast<uint8_t>(std::get<uint64_t>(confVal));
            }
        }
        else if (config->role == mctp::types::Role::endpoint)
        {
            confVal = dbusWrapper->getConfigurationField(
                yield, objectPath, requestEIDPool.data(), false);
            if (isValid(confVal))
            {
                config->requiredEIDPoolSize =
                    static_cast<uint8_t>(std::get<uint64_t>(confVal));
            }
        }

        confVal = dbusWrapper->getConfigurationField(yield, objectPath,
                                                     configIndex.data(), false);
        if (isValid(confVal))
        {
            config->markInterfaceAsMatched = false;
            uint64_t hpmIdx = std::get<uint64_t>(confVal);
            lg2::info("HPM config index: {INDEX}", "INDEX", hpmIdx);

            confVal = dbusWrapper->getConfigurationField(
                yield, objectPath, useSharedEID.data(), true);
            auto sharedEIDVals = std::get<std::vector<uint64_t>>(confVal);
            if (sharedEIDVals.size() < (hpmIdx + 1))
            {
                throw std::runtime_error(
                    "UseSharedEID array size is less than ConfigIndex");
            }
            config->useSharedEID =
                static_cast<bool>(sharedEIDVals[hpmIdx] == 1);

            confVal = dbusWrapper->getConfigurationField(
                yield, objectPath, sharedRequestEIDPool.data(), true);
            auto sharedEIDPoolVals = std::get<std::vector<uint64_t>>(confVal);
            if (sharedEIDPoolVals.size() < (hpmIdx + 1))
            {
                throw std::runtime_error(
                    "SharedRequestEIDPool array size is less than ConfigIndex");
            }
            config->requiredEIDPoolSize =
                static_cast<uint8_t>(sharedEIDPoolVals[hpmIdx]);
        }

        switch (config->physicalLinkType)
        {
            case types::PhysicalLinkType::smbus: {
                auto i2cConfig =
                    std::static_pointer_cast<mctp::config::I2CConfiguration>(
                        config);
                confVal = dbusWrapper->getConfigurationField(
                    yield, objectPath, "DefaultAddress", false);
                if (isValid(confVal))
                {
                    i2cConfig->defaultAddress =
                        static_cast<uint8_t>(std::get<uint64_t>(confVal));
                }
            }
            break;
            case types::PhysicalLinkType::i3c: {
                auto i3cConfig =
                    std::static_pointer_cast<mctp::config::I3CConfiguration>(
                        config);
                confVal = dbusWrapper->getConfigurationField(
                    yield, objectPath, busNumber.data(), true);
                if (isValid(confVal))
                {
                    i3cConfig->busNumber =
                        static_cast<uint16_t>(std::get<uint64_t>(confVal));
                }
                confVal = dbusWrapper->getConfigurationField(
                    yield, objectPath, pidMask.data(), true);
                i3cConfig->pidMask = std::get<std::string>(confVal);
            }
            break;
            case types::PhysicalLinkType::pcie: {
                auto pcieConfig =
                    std::static_pointer_cast<mctp::config::PCIeConfiguration>(
                        config);
                // Read more
            }
            break;
            case types::PhysicalLinkType::usb: {
                auto usbConfig =
                    std::static_pointer_cast<mctp::config::USBConfiguration>(
                        config);
                // Read more
            }
            break;
        };

        callback(config);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to process MCTP configuration object {OBJ}: {ERROR}",
                   "OBJ", objectPath, "ERROR", e.what());
    }
}

constexpr bool isEqualIgnoreCase(const std::string_view a,
                                 const std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
};

mctp::types::PhysicalLinkType
    toPhysicalLinkType(const std::string_view linkType)
{
    if (isEqualIgnoreCase(linkType, "i2c") ||
        isEqualIgnoreCase(linkType, "smbus"))
    {
        return mctp::types::PhysicalLinkType::smbus;
    }
    else if (isEqualIgnoreCase(linkType, "i3c"))
    {
        return mctp::types::PhysicalLinkType::i3c;
    }
    else if (isEqualIgnoreCase(linkType, "pcie"))
    {
        return mctp::types::PhysicalLinkType::pcie;
    }
    else if (isEqualIgnoreCase(linkType, "usb"))
    {
        return mctp::types::PhysicalLinkType::usb;
    }
    else
    {
        throw std::invalid_argument("Unknown PhysicalLinkType: " +
                                    std::string(linkType));
    }
}

std::shared_ptr<mctp::config::Configuration>
    createConfig(const mctp::types::PhysicalLinkType linkType)
{
    std::shared_ptr<mctp::config::Configuration> config;
    switch (linkType)
    {
        case mctp::types::PhysicalLinkType::smbus: {
            config = std::make_shared<mctp::config::I2CConfiguration>();
        }
        break;
        case mctp::types::PhysicalLinkType::i3c: {
            config = std::make_shared<mctp::config::I3CConfiguration>();
        }
        break;
        case mctp::types::PhysicalLinkType::pcie: {
            config = std::make_shared<mctp::config::PCIeConfiguration>();
        }
        break;
        case mctp::types::PhysicalLinkType::usb: {
            config = std::make_shared<mctp::config::USBConfiguration>();
        }
        break;
        default:
            throw std::invalid_argument(
                "Unsupported PhysicalLinkType enum value");
    }
    config->physicalLinkType = linkType;
    return config;
}

static mctp::types::Role toRole(const std::string_view role)
{
    if (isEqualIgnoreCase(role, "endpoint"))
    {
        return mctp::types::Role::endpoint;
    }
    else if (isEqualIgnoreCase(role, "bridge"))
    {
        return mctp::types::Role::bridge;
    }
    else if (isEqualIgnoreCase(role, "busowner"))
    {
        return mctp::types::Role::topmostBusOwner;
    }
    else
    {
        throw std::invalid_argument("Unknown Role: " + std::string(role));
    }
}

mctp::types::ResetEvent toResetEvent(const std::string_view event)
{
    if (isEqualIgnoreCase(event, "espiReset"))
    {
        return mctp::types::ResetEvent::espiReset;
    }
    else if (isEqualIgnoreCase(event, "powerStateChange"))
    {
        return mctp::types::ResetEvent::powerStateChange;
    }
    else if (isEqualIgnoreCase(event, "pcieEnum"))
    {
        return mctp::types::ResetEvent::pcieEnum;
    }
    else
    {
        throw std::invalid_argument("Unknown ResetEvent: " +
                                    std::string(event));
    }
}