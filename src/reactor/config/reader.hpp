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

#include "config.hpp"
#include "wrapper.hpp"

#include <functional>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mctp::config
{
class Reader
{
  public:
    using Callback = std::function<void(std::shared_ptr<Configuration>)>;
    Reader(std::shared_ptr<mctp::dbus::wrappers::ConfigWrapper> dbusWrapper,
           Callback callback);
    void readMCTPConfigurations(boost::asio::yield_context yield);

  protected:
    Callback callback;
    std::shared_ptr<mctp::dbus::wrappers::ConfigWrapper> dbusWrapper;
    void processMCTPConfigObject(boost::asio::yield_context yield,
                                 const std::string& objectPath);
};
} // namespace mctp::config