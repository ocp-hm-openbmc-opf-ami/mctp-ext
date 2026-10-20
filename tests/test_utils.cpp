#include "DeviceMgmt.hpp"
#include "MCTPEndpoint.hpp"
#include "Utils.hpp"

#ifdef FAIL
#undef FAIL
#endif
#ifdef ERROR
#undef ERROR
#endif

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// escapeName
// ---------------------------------------------------------------------------

TEST(EscapeNameTest, EscapeName_NoSpaces_ReturnsUnchanged)
{
    EXPECT_EQ(escapeName("sensor_temp"), "sensor_temp");
}

TEST(EscapeNameTest, EscapeName_WithSpaces_ReplacesWithUnderscores)
{
    EXPECT_EQ(escapeName("CPU Temp"), "CPU_Temp");
}

TEST(EscapeNameTest, EscapeName_MultipleSpaces_AllReplaced)
{
    EXPECT_EQ(escapeName("Fan Speed RPM"), "Fan_Speed_RPM");
}

TEST(EscapeNameTest, EscapeName_EmptyString_ReturnsEmpty)
{
    EXPECT_EQ(escapeName(""), "");
}

// ---------------------------------------------------------------------------
// configInterfaceName
// ---------------------------------------------------------------------------

TEST(ConfigInterfaceNameTest,
     ConfigInterfaceName_ValidType_PrependsPrefixCorrectly)
{
    EXPECT_EQ(configInterfaceName("Temp"),
              "xyz.openbmc_project.Configuration.Temp");
}

TEST(ConfigInterfaceNameTest, ConfigInterfaceName_EmptyType_ReturnsOnlyPrefix)
{
    EXPECT_EQ(configInterfaceName(""), "xyz.openbmc_project.Configuration.");
}

// ---------------------------------------------------------------------------
// sensorNameFind (DeviceMgmt.hpp inline)
// ---------------------------------------------------------------------------

TEST(SensorNameFindTest, SensorNameFind_ExactMatch_ReturnsPosition)
{
    EXPECT_NE(sensorNameFind("CPU_Temp", "CPU_Temp"), std::string::npos);
}

TEST(SensorNameFindTest, SensorNameFind_SpaceVsUnderscore_MatchesIgnoringDiff)
{
    EXPECT_NE(sensorNameFind("CPU_Temp", "CPU Temp"), std::string::npos);
}

TEST(SensorNameFindTest, SensorNameFind_NoMatch_ReturnsNpos)
{
    EXPECT_EQ(sensorNameFind("CPU_Temp", "GPU_Temp"), std::string::npos);
}

// ---------------------------------------------------------------------------
// openAndRead
// ---------------------------------------------------------------------------

class OpenAndReadTest : public ::testing::Test
{
  protected:
    fs::path tempFile;

    void SetUp() override
    {
        tempFile = fs::temp_directory_path() / "test_openandread.txt";
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove(tempFile, ec);
    }
};

TEST_F(OpenAndReadTest, OpenAndRead_ExistingFile_ReturnsFirstLine)
{
    std::ofstream out(tempFile);
    out << "hello world\n";
    out.close();

    auto result = openAndRead(tempFile.string());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "hello world");
}

TEST_F(OpenAndReadTest, OpenAndRead_NonExistentFile_ReturnsNullopt)
{
    auto result = openAndRead("/nonexistent/path/file.txt");

    EXPECT_FALSE(result.has_value());
}

TEST_F(OpenAndReadTest, OpenAndRead_EmptyFile_ReturnsEmptyString)
{
    std::ofstream out(tempFile);
    out.close();

    auto result = openAndRead(tempFile.string());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "");
}

TEST_F(OpenAndReadTest, OpenAndRead_PathTraversal_ReturnsNulloptForNonExistent)
{
    // Path traversal input must not crash — it simply returns nullopt when the
    // resolved path does not exist.
    auto result = openAndRead("../../../etc/passwd_nonexistent");
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// getPermitSet
// ---------------------------------------------------------------------------

TEST(GetPermitSetTest, GetPermitSet_NoLabelsKey_ReturnsEmptySet)
{
    SensorBaseConfigMap config;
    config["Type"] = std::string("Temp");

    auto result = getPermitSet(config);

    EXPECT_TRUE(result.empty());
}

TEST(GetPermitSetTest, GetPermitSet_WithLabels_ReturnsPopulatedSet)
{
    SensorBaseConfigMap config;
    config["Labels"] = std::vector<std::string>{"temp1", "temp2"};

    auto result = getPermitSet(config);

    EXPECT_EQ(result.size(), 2u);
    EXPECT_TRUE(result.contains("temp1"));
    EXPECT_TRUE(result.contains("temp2"));
}

TEST(GetPermitSetTest,
     GetPermitSet_WrongVariantType_ReturnsEmptySetWithoutThrow)
{
    // Pre-scan finding: swallowed bad_variant_access — caller gets empty set
    SensorBaseConfigMap config;
    config["Labels"] = std::string("not_a_vector");

    EXPECT_NO_THROW({
        auto result = getPermitSet(config);
        EXPECT_TRUE(result.empty());
    });
}

// ---------------------------------------------------------------------------
// getFullHwmonFilePath
// ---------------------------------------------------------------------------

class GetFullHwmonFilePathTest : public ::testing::Test
{
  protected:
    fs::path tempDir;

    void SetUp() override
    {
        tempDir = fs::temp_directory_path() / "test_hwmon";
        fs::create_directories(tempDir);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }
};

TEST_F(GetFullHwmonFilePathTest,
       GetFullHwmonFilePath_EmptyPermitSet_ReturnsInputPath)
{
    std::set<std::string> emptyPermit;

    auto result = getFullHwmonFilePath(tempDir.string(), "temp1", emptyPermit);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, tempDir.string() + "/temp1_input");
}

TEST_F(GetFullHwmonFilePathTest,
       GetFullHwmonFilePath_LabelInPermitSet_ReturnsInputPath)
{
    // Create label file with permitted value
    std::ofstream labelFile(tempDir / "temp1_label");
    labelFile << "CPU\n";
    labelFile.close();

    std::set<std::string> permitSet{"CPU"};

    auto result = getFullHwmonFilePath(tempDir.string(), "temp1", permitSet);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, tempDir.string() + "/temp1_input");
}

TEST_F(GetFullHwmonFilePathTest,
       GetFullHwmonFilePath_LabelNotInPermitSet_ReturnsNullopt)
{
    std::ofstream labelFile(tempDir / "temp1_label");
    labelFile << "GPU\n";
    labelFile.close();

    std::set<std::string> permitSet{"CPU"};

    auto result = getFullHwmonFilePath(tempDir.string(), "temp1", permitSet);

    EXPECT_FALSE(result.has_value());
}

TEST_F(GetFullHwmonFilePathTest,
       GetFullHwmonFilePath_NoLabelFile_FallsBackToBaseName)
{
    // No label file — basename is used as search value
    std::set<std::string> permitSet{"temp1"};

    auto result = getFullHwmonFilePath(tempDir.string(), "temp1", permitSet);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, tempDir.string() + "/temp1_input");
}

// ---------------------------------------------------------------------------
// findFiles
// ---------------------------------------------------------------------------

class FindFilesTest : public ::testing::Test
{
  protected:
    fs::path tempDir;

    void SetUp() override
    {
        tempDir = fs::temp_directory_path() / "test_findfiles";
        fs::create_directories(tempDir);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }
};

TEST_F(FindFilesTest, FindFiles_NonExistentDir_ReturnsFalse)
{
    std::vector<fs::path> found;
    bool result = findFiles(tempDir / "nonexistent", ".*", found);

    EXPECT_FALSE(result);
}

TEST_F(FindFilesTest, FindFiles_MatchingFile_ReturnsTrue_And_FindsFile)
{
    std::ofstream f(tempDir / "temp1_input");
    f.close();

    std::vector<fs::path> found;
    bool result = findFiles(tempDir, "temp1_input", found);

    EXPECT_TRUE(result);
    EXPECT_EQ(found.size(), 1u);
}

TEST_F(FindFilesTest, FindFiles_NoMatch_ReturnsTrueWithEmptyResults)
{
    std::ofstream f(tempDir / "temp1_input");
    f.close();

    std::vector<fs::path> found;
    bool result = findFiles(tempDir, "nomatch_file", found);

    EXPECT_TRUE(result);
    EXPECT_TRUE(found.empty());
}

TEST_F(FindFilesTest, FindFiles_RegexPattern_MatchesMultipleFiles)
{
    std::ofstream f1(tempDir / "temp1_input");
    std::ofstream f2(tempDir / "temp2_input");
    f1.close();
    f2.close();

    std::vector<fs::path> found;
    bool result = findFiles(tempDir, "temp[0-9]_input", found);

    EXPECT_TRUE(result);
    EXPECT_EQ(found.size(), 2u);
}

// ---------------------------------------------------------------------------
// MCTPException
// ---------------------------------------------------------------------------

TEST(MCTPExceptionTest, What_WithStringLiteral_ReturnsDescriptionString)
{
    MCTPException ex("test error");
    EXPECT_STREQ(ex.what(), "test error");
}

TEST(MCTPExceptionTest, What_ThrownAndCaught_PreservesMessage)
{
    try
    {
        throw MCTPException("endpoint removed");
    }
    catch (const MCTPException& e)
    {
        EXPECT_STREQ(e.what(), "endpoint removed");
    }
}

// ---------------------------------------------------------------------------
// MCTPInterface comparison operator
// ---------------------------------------------------------------------------

TEST(MCTPInterfaceTest, Comparison_EqualInterfaces_AreEqual)
{
    MCTPInterface a{"eth0", MCTPTransport::SMBus};
    MCTPInterface b{"eth0", MCTPTransport::SMBus};

    EXPECT_EQ(a, b);
}

TEST(MCTPInterfaceTest, Comparison_DifferentName_NotEqual)
{
    MCTPInterface a{"eth0", MCTPTransport::SMBus};
    MCTPInterface b{"eth1", MCTPTransport::SMBus};

    EXPECT_NE(a, b);
}

TEST(MCTPInterfaceTest, Comparison_Ordering_SmallerNameComesFirst)
{
    MCTPInterface a{"eth0", MCTPTransport::SMBus};
    MCTPInterface b{"eth1", MCTPTransport::SMBus};

    EXPECT_LT(a, b);
}

// ---------------------------------------------------------------------------
// getI2CDeviceParams
// ---------------------------------------------------------------------------

TEST(GetI2CDeviceParamsTest, GetI2CDeviceParams_MissingType_ReturnsNullopt)
{
    const I2CDeviceType fakeType{"FakeDevice", false};
    const std::pair<std::string_view, I2CDeviceType> dtmapArr[] = {
        {"FakeDevice", fakeType}};

    SensorBaseConfigMap cfg;
    cfg["Bus"] = uint64_t{1};
    cfg["Address"] = uint64_t{0x50};

    auto result = getI2CDeviceParams(dtmapArr, cfg);

    EXPECT_FALSE(result.has_value());
}

TEST(GetI2CDeviceParamsTest, GetI2CDeviceParams_MissingBus_ReturnsNullopt)
{
    const I2CDeviceType fakeType{"FakeDevice", false};
    const std::pair<std::string_view, I2CDeviceType> dtmapArr[] = {
        {"FakeDevice", fakeType}};

    SensorBaseConfigMap cfg;
    cfg["Type"] = std::string{"FakeDevice"};
    cfg["Address"] = uint64_t{0x50};

    auto result = getI2CDeviceParams(dtmapArr, cfg);

    EXPECT_FALSE(result.has_value());
}

TEST(GetI2CDeviceParamsTest, GetI2CDeviceParams_MissingAddress_ReturnsNullopt)
{
    const I2CDeviceType fakeType{"FakeDevice", false};
    const std::pair<std::string_view, I2CDeviceType> dtmapArr[] = {
        {"FakeDevice", fakeType}};

    SensorBaseConfigMap cfg;
    cfg["Type"] = std::string{"FakeDevice"};
    cfg["Bus"] = uint64_t{1};

    auto result = getI2CDeviceParams(dtmapArr, cfg);

    EXPECT_FALSE(result.has_value());
}

TEST(GetI2CDeviceParamsTest, GetI2CDeviceParams_TypeNotInMap_ReturnsNullopt)
{
    const I2CDeviceType fakeType{"FakeDevice", false};
    const std::pair<std::string_view, I2CDeviceType> dtmapArr[] = {
        {"FakeDevice", fakeType}};

    SensorBaseConfigMap cfg;
    cfg["Type"] = std::string{"UnknownDevice"};
    cfg["Bus"] = uint64_t{1};
    cfg["Address"] = uint64_t{0x50};

    auto result = getI2CDeviceParams(dtmapArr, cfg);

    EXPECT_FALSE(result.has_value());
}

TEST(GetI2CDeviceParamsTest, GetI2CDeviceParams_AllFieldsValid_ReturnsParams)
{
    const I2CDeviceType fakeType{"FakeDevice", false};
    const std::pair<std::string_view, I2CDeviceType> dtmapArr[] = {
        {"FakeDevice", fakeType}};

    SensorBaseConfigMap cfg;
    cfg["Type"] = std::string{"FakeDevice"};
    cfg["Bus"] = uint64_t{1};
    cfg["Address"] = uint64_t{0x50};

    auto result = getI2CDeviceParams(dtmapArr, cfg);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->bus, 1u);
    EXPECT_EQ(result->address, 0x50u);
}
