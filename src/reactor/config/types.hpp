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

#include <cstdint>
#include <string>
#include <vector>

namespace mctp::types
{
using Byte = uint8_t;
using EID = uint8_t;

// Enum for internal use. Does not map to DMTF spec
enum class PhysicalLinkType : uint8_t
{
    i3c = 0,
    smbus = 1,
    pcie = 2,
    usb = 3
};

enum class Role
{
    endpoint = 0,
    bridge = 1,
    topmostBusOwner = 2
};

// Binding IDs from DMTF spec
enum class PhysicalTransportBinding
{
    smbus = 0x01,
    pcie = 0x02,
    usb = 0x03,
    i3c = 0x06,
    vdm = 0xff
};

enum class PhysicalMedium
{
    smbus100KHz = 0x01,
    pcie11 = 0x08,
    pcie20 = 0x09,
    usb11 = 0x10,
    usb20 = 0x11,
    i3cBasic = 0x30
};

enum class ResetEvent
{
    espiReset = 0,
    powerStateChange = 1,
    pcieEnum = 2
};

}; // namespace mctp::types