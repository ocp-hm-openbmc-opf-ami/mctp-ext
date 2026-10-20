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

#include <string_view>

namespace mctp::config::property
{
static constexpr std::string_view name = "Name";
static constexpr std::string_view physicalLinkType = "PhysicalLinkType";
static constexpr std::string_view physicalLinkName = "PhysicalLinkName";
static constexpr std::string_view priority = "Priority";
static constexpr std::string_view networkID = "NetworkID";
static constexpr std::string_view role = "Role";
static constexpr std::string_view retryCount = "RetryCount";
static constexpr std::string_view timeOut = "Timeout";
static constexpr std::string_view resetEvents = "ResetEvents";
static constexpr std::string_view mtu = "MTU";

static constexpr std::string_view busNumber = "BusNumber";
static constexpr std::string_view pidMask = "PIDMask";

static constexpr std::string_view ownEID = "OwnEID";
static constexpr std::string_view eidPool = "EIDPool";

static constexpr std::string_view configIndex = "ConfigIndex";
static constexpr std::string_view useSharedEID = "UseSharedEID";
static constexpr std::string_view sharedRequestEIDPool = "SharedRequestEIDPool";

static constexpr std::string_view requestEIDPool = "RequestEIDPool";

static constexpr std::string_view defaultAddress = "DefaultAddress";
} // namespace mctp::config::property