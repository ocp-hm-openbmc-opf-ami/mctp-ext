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
#pragma once

#include "types.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace mctp::config
{
struct Configuration
{
    types::PhysicalLinkType physicalLinkType;
    std::string physicalLinkName;
    uint8_t priority = 0;
    uint32_t networkID = 0;
    types::Role role;
    std::vector<types::EID> eidPool;
    types::EID ownEID = 0;
    size_t retryCount = 0;
    std::chrono::milliseconds timeOut;
    std::vector<types::ResetEvent> resetEvents;
    uint8_t requiredEIDPoolSize = 0;
    uint32_t mtu = 0;
    bool useSharedEID = false;
    // Normally a config will match for a single interface. Set this to false if
    // single interface can match to multiple configs
    bool markInterfaceAsMatched = true;
    std::string configName = "";

    virtual ~Configuration() = default;
    virtual std::shared_ptr<Configuration> clone() const = 0;
};

struct I2CConfiguration : public Configuration
{
    uint8_t defaultAddress = 0x09;
    uint16_t busNumber = 0;

    std::shared_ptr<Configuration> clone() const override
    {
        return std::make_shared<I2CConfiguration>(*this);
    }
};

struct I3CConfiguration : public Configuration
{
    uint16_t busNumber = 0;
    std::string pidMask;

    std::shared_ptr<Configuration> clone() const override
    {
        return std::make_shared<I3CConfiguration>(*this);
    }
};
struct PCIeConfiguration : public Configuration
{
    std::shared_ptr<Configuration> clone() const override
    {
        return std::make_shared<PCIeConfiguration>(*this);
    }
};
struct USBConfiguration : public Configuration
{
    std::shared_ptr<Configuration> clone() const override
    {
        return std::make_shared<USBConfiguration>(*this);
    }
};
}; // namespace mctp::config