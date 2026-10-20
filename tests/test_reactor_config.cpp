#include "MCTPReactorConfig.hpp"

#ifdef FAIL
#undef FAIL
#endif
#ifdef ERROR
#undef ERROR
#endif

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

// MCTPD_CONF_FILE_DEFAULT is "/tmp/test-mctpd.conf" in test config.h
// MCTPD_JSON_FILE_DEFAULT is "/tmp/test-mctp-ext.json" in test config.h

// ---------------------------------------------------------------------------
// Helper: write a TOML file at the test config path
// ---------------------------------------------------------------------------
static void writeTempToml(const std::string& content)
{
    std::ofstream out(MCTPD_CONF_FILE_DEFAULT);
    out << content;
}

static void removeTempToml()
{
    std::remove(MCTPD_CONF_FILE_DEFAULT);
}

// ---------------------------------------------------------------------------
// Helper: write a JSON file at the test config path
// ---------------------------------------------------------------------------
static void writeTempJson(const std::string& content,
                          const std::string& path = MCTPD_JSON_FILE_DEFAULT)
{
    std::ofstream out(path);
    out << content;
}

static void removeTempJson(const std::string& path = MCTPD_JSON_FILE_DEFAULT)
{
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// fromTomlFile — file not found
// ---------------------------------------------------------------------------

TEST(MCTPReactorConfigTomlTest, FromTomlFile_NoFile_ReturnsDefaultConfig)
{
    removeTempToml();

    MCTPReactorConfig config = MCTPReactorConfig::fromTomlFile();

    // Defaults from meson_options.txt
    EXPECT_EQ(config.localEid, DEFAULT_LOCAL_EID);
    EXPECT_EQ(config.i2c.i2cNet, DEFAULT_I2C_NET);
    EXPECT_EQ(config.i3c.i3cNet, DEFAULT_I3C_NET);
}

// ---------------------------------------------------------------------------
// fromTomlFile — mctp section
// ---------------------------------------------------------------------------

TEST(MCTPReactorConfigTomlTest, FromTomlFile_MctpSection_SetsLocalEid)
{
    writeTempToml("[mctp]\nlocal_eid = 20\n");

    MCTPReactorConfig config = MCTPReactorConfig::fromTomlFile();
    removeTempToml();

    EXPECT_EQ(config.localEid, 20u);
}

// ---------------------------------------------------------------------------
// fromTomlFile — I2C section
// ---------------------------------------------------------------------------

TEST(MCTPReactorConfigTomlTest, FromTomlFile_I2CSection_SetsNetAndInterval)
{
    writeTempToml("[mctp_i2c]\n"
                  "mctp_i2c_net = 42\n"
                  "mctp_i2c_poll_interval_secs = 15\n");

    MCTPReactorConfig config = MCTPReactorConfig::fromTomlFile();
    removeTempToml();

    EXPECT_EQ(config.i2c.i2cNet, 42u);
    EXPECT_EQ(config.i2c.pollingInterval, std::chrono::seconds(15));
}

TEST(MCTPReactorConfigTomlTest, FromTomlFile_I2CWhitelist_ParsesSpaceSeparated)
{
    writeTempToml("[mctp_i2c]\n"
                  "mctp_i2c_whitelist = \"0x50 0x51 0x52\"\n");

    MCTPReactorConfig config = MCTPReactorConfig::fromTomlFile();
    removeTempToml();

    EXPECT_TRUE(config.i2c.whitelist.contains("0x50"));
    EXPECT_TRUE(config.i2c.whitelist.contains("0x51"));
    EXPECT_TRUE(config.i2c.whitelist.contains("0x52"));
}

// ---------------------------------------------------------------------------
// fromTomlFile — I3C section
// ---------------------------------------------------------------------------

TEST(MCTPReactorConfigTomlTest, FromTomlFile_I3CSection_SetsPlatformAndNet)
{
    writeTempToml("[mctp_i3c]\n"
                  "mctp_i3c_net = 8\n"
                  "mctp_i3c_platform = \"nuvoton\"\n"
                  "mctp_i3c_poll_interval_secs = 90\n");

    MCTPReactorConfig config = MCTPReactorConfig::fromTomlFile();
    removeTempToml();

    EXPECT_EQ(config.i3c.i3cNet, 8u);
    EXPECT_EQ(config.i3c.platformSoc, "nuvoton");
    EXPECT_EQ(config.i3c.pollingInterval, std::chrono::seconds(90));
}

TEST(MCTPReactorConfigTomlTest, FromTomlFile_I3CDevicesArray_ParsesDeviceEntry)
{
    writeTempToml(
        "[mctp_i3c]\n"
        "[[mctp_i3c.devices]]\n"
        "name = \"BMC_FPGA\"\n"
        "bus_num = 2\n"
        "pid_mask = 255\n"
        "device_pid = \"cafe\"\n"
        "role = \"endpoint\"\n"
        "is_i3c_target = true\n"
        "is_secondary_bus_owner = false\n");

    MCTPReactorConfig config = MCTPReactorConfig::fromTomlFile();
    removeTempToml();

    ASSERT_EQ(config.i3c.devices.size(), 1u);
    const auto& dev = config.i3c.devices[0];
    EXPECT_EQ(dev.name, "BMC_FPGA");
    EXPECT_EQ(dev.busNum, 2u);
    EXPECT_EQ(dev.pidMask, 255u);
    EXPECT_EQ(dev.devicePid, "cafe");
    EXPECT_EQ(dev.role, "endpoint");
    EXPECT_TRUE(dev.isTarget);
    EXPECT_FALSE(dev.isSecondaryBusOwner);
}

// ---------------------------------------------------------------------------
// fromTomlFile — PCIe section
// ---------------------------------------------------------------------------

TEST(MCTPReactorConfigTomlTest, FromTomlFile_PCIeSection_SetsNetAndRole)
{
    writeTempToml("[mctp_pcie]\n"
                  "mctp_pcie_net = 3\n"
                  "mctp_pcie_role = \"bus-owner\"\n"
                  "mctp_pcie_poll_interval_secs = 45\n");

    MCTPReactorConfig config = MCTPReactorConfig::fromTomlFile();
    removeTempToml();

    EXPECT_EQ(config.pcie.pcieNet, 3u);
    EXPECT_EQ(config.pcie.pcieRole, "bus-owner");
    EXPECT_EQ(config.pcie.pollingInterval, std::chrono::seconds(45));
}

// ---------------------------------------------------------------------------
// fromTomlFile — USB section
// ---------------------------------------------------------------------------

TEST(MCTPReactorConfigTomlTest, FromTomlFile_USBSection_SetsNetAndInterval)
{
    writeTempToml("[mctp_usb]\n"
                  "mctp_usb_net = 5\n"
                  "mctp_usb_poll_interval_secs = 30\n");

    MCTPReactorConfig config = MCTPReactorConfig::fromTomlFile();
    removeTempToml();

    EXPECT_EQ(config.usb.usbNet, 5u);
    EXPECT_EQ(config.usb.pollingInterval, std::chrono::seconds(30));
}

// ---------------------------------------------------------------------------
// fromTomlFile — invalid TOML
// ---------------------------------------------------------------------------

TEST(MCTPReactorConfigTomlTest, FromTomlFile_InvalidToml_ReturnsDefaultConfig)
{
    writeTempToml("this is not valid TOML ][{{\n");

    MCTPReactorConfig config = MCTPReactorConfig::fromTomlFile();
    removeTempToml();

    // Parsing fails — should return default (no throw, no crash)
    EXPECT_EQ(config.localEid, DEFAULT_LOCAL_EID);
}

// ---------------------------------------------------------------------------
// fromTomlFile — multiple sections combined
// ---------------------------------------------------------------------------

TEST(MCTPReactorConfigTomlTest, FromTomlFile_MultipleTopLevelSections_AllParsed)
{
    writeTempToml(
        "[mctp]\n"
        "local_eid = 12\n"
        "\n"
        "[mctp_i2c]\n"
        "mctp_i2c_net = 2\n"
        "\n"
        "[mctp_pcie]\n"
        "mctp_pcie_net = 4\n");

    MCTPReactorConfig config = MCTPReactorConfig::fromTomlFile();
    removeTempToml();

    EXPECT_EQ(config.localEid, 12u);
    EXPECT_EQ(config.i2c.i2cNet, 2u);
    EXPECT_EQ(config.pcie.pcieNet, 4u);
}

// ---------------------------------------------------------------------------
// fromJsonFile — file not found
// ---------------------------------------------------------------------------

TEST(MCTPReactorConfigJsonTest, FromJsonFile_NoFile_ReturnsDefaultConfig)
{
    removeTempJson();

    MCTPReactorConfig config =
        MCTPReactorConfig::fromJsonFile(MCTPD_JSON_FILE_DEFAULT);

    EXPECT_EQ(config.localEid, DEFAULT_LOCAL_EID);
}

// ---------------------------------------------------------------------------
// fromJsonFile — minimal valid JSON
// ---------------------------------------------------------------------------

TEST(MCTPReactorConfigJsonTest, FromJsonFile_ValidJsonWithI2CConfig_SetsI2CNet)
{
    const std::string json = R"([{
  "Exposes": [
    {
      "Type": "MCTPI2CConfiguration",
      "Enabled": "enabled",
      "Net": 7,
      "PollingInterval": 20
    }
  ]
}])";
    writeTempJson(json);

    MCTPReactorConfig config =
        MCTPReactorConfig::fromJsonFile(MCTPD_JSON_FILE_DEFAULT);
    removeTempJson();

    EXPECT_TRUE(config.i2c.enabled);
    EXPECT_EQ(config.i2c.i2cNet, 7u);
    EXPECT_EQ(config.i2c.pollingInterval, std::chrono::seconds(20));
}

TEST(MCTPReactorConfigJsonTest, FromJsonFile_MCTPGeneralSetting_SetsLocalEid)
{
    const std::string json = R"([{
  "Exposes": [
    {
      "Type": "MCTPGeneralSetting",
      "DefaultLocalEid": 25
    }
  ]
}])";
    writeTempJson(json);

    MCTPReactorConfig config =
        MCTPReactorConfig::fromJsonFile(MCTPD_JSON_FILE_DEFAULT);
    removeTempJson();

    EXPECT_EQ(config.localEid, 25u);
}

TEST(MCTPReactorConfigJsonTest,
     FromJsonFile_I3CConfiguration_SetsNetAndPlatform)
{
    const std::string json = R"([{
  "Exposes": [
    {
      "Type": "MCTPI3CConfiguration",
      "Enabled": "enabled",
      "Net": 9,
      "SoC": "aspeed-2700"
    }
  ]
}])";
    writeTempJson(json);

    MCTPReactorConfig config =
        MCTPReactorConfig::fromJsonFile(MCTPD_JSON_FILE_DEFAULT);
    removeTempJson();

    EXPECT_TRUE(config.i3c.enabled);
    EXPECT_EQ(config.i3c.i3cNet, 9u);
    EXPECT_EQ(config.i3c.platformSoc, "aspeed-2700");
}

// ---------------------------------------------------------------------------
// fromJsonFile — invalid JSON format
// ---------------------------------------------------------------------------

TEST(MCTPReactorConfigJsonTest, FromJsonFile_NotAnArray_ReturnsDefaultConfig)
{
    writeTempJson(R"({"Type": "invalid_root"})");

    MCTPReactorConfig config =
        MCTPReactorConfig::fromJsonFile(MCTPD_JSON_FILE_DEFAULT);
    removeTempJson();

    EXPECT_EQ(config.localEid, DEFAULT_LOCAL_EID);
}
