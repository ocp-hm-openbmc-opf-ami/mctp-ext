/*
// Copyright (c) 2025 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
*/
#pragma once

#include "wrapper.hpp"

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>

#include <memory>
#include <string>

namespace mctp::dbus
{
class ConfigWrapperImpl : public mctp::dbus::wrappers::ConfigWrapper
{
  public:
    explicit ConfigWrapperImpl(
        std::shared_ptr<sdbusplus::asio::connection> conn) :
        connection(std::move(conn))
    {}

    void getMCTPConfigurations(boost::asio::yield_context yield,
                               MCTPConfigCallback callback) override;

    ConfigurationField getConfigurationField(
        boost::asio::yield_context yield, const std::string& configPath,
        const std::string& property, bool throwOnErr) override;

  private:
    inline static const std::string mctpConfigInterfaceName =
        "xyz.openbmc_project.Configuration.MctpConfig";

    void onNewMCTPConfigurationSignal(sdbusplus::message_t& msg);
    std::string getInterfaceName(const std::string& property) const;

    std::shared_ptr<sdbusplus::asio::connection> connection;
    std::unique_ptr<sdbusplus::bus::match::match> mctpConfigMatch;
};
} // namespace mctp::dbus
