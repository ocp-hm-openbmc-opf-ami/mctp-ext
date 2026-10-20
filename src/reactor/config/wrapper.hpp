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

#include <boost/asio/spawn.hpp>
#include <expected>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace mctp::dbus
{
namespace wrappers
{
class ConfigWrapper
{
  public:
    using MCTPConfigCallback =
        std::function<void(boost::asio::yield_context yield, std::string)>;
    using ConfigurationField =
        std::variant<std::monostate, bool, uint64_t, std::string,
                     std::vector<uint64_t>, std::vector<std::string>>;
    using ConfigurationMap =
        std::unordered_map<std::string, ConfigurationField>;
    virtual void getMCTPConfigurations(boost::asio::yield_context,
                                       MCTPConfigCallback callback)
    {
        onNewMCTPConfig = std::move(callback);
    }
    virtual ConfigurationField
        getConfigurationField(boost::asio::yield_context yield,
                              const std::string& configPath,
                              const std::string& property, bool throwOnErr) = 0;

  protected:
    MCTPConfigCallback onNewMCTPConfig;
};
} // namespace wrappers
} // namespace mctp::dbus