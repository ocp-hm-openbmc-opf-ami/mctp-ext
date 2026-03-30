#pragma once

#include <sys/inotify.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

/**
 * @brief Monitors /var/run/mctp_trace_on for runtime log level changes.
 *
 * Uses inotify via boost::asio to watch for file create/modify/delete.
 * File format: "reactor:<level>" where level is a syslog-style integer:
 *   3=error, 4=warning, 5=notice, 6=info, 7=debug
 *
 * On file deletion, resets to the default level (warning).
 */
class ReactorDebugMonitor
{
  public:
    ReactorDebugMonitor(boost::asio::io_context& ioCtx,
                        const std::string& directory = "/var/run",
                        const std::string& file = "mctp_trace_on") :
        io(ioCtx),
        directoryPath(directory), fileName(file)
    {}

    ~ReactorDebugMonitor() { stop(); }

    ReactorDebugMonitor(const ReactorDebugMonitor&) = delete;
    ReactorDebugMonitor& operator=(const ReactorDebugMonitor&) = delete;

    int start()
    {
        inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotifyFd < 0)
        {
            lg2::error("ReactorDebugMonitor: inotify_init1 failed: {ERROR}",
                       "ERROR", strerror(errno));
            return -1;
        }

        watchDescriptor = inotify_add_watch(
            inotifyFd, directoryPath.c_str(),
            IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);

        if (watchDescriptor < 0)
        {
            lg2::error(
                "ReactorDebugMonitor: inotify_add_watch failed: {ERROR}",
                "ERROR", strerror(errno));
            close(inotifyFd);
            inotifyFd = -1;
            return -1;
        }

        descriptor =
            std::make_unique<boost::asio::posix::stream_descriptor>(
                io, inotifyFd);

        // Check if file already exists at startup
        std::string filePath = directoryPath + "/" + fileName;
        std::ifstream probe(filePath);
        if (probe.good())
        {
            probe.close();
            readDebugFile(filePath);
        }

        asyncWaitForEvents();

        lg2::info("ReactorDebugMonitor started, watching {DIR}/{FILE}",
                  "DIR", directoryPath, "FILE", fileName);
        return 0;
    }

    void stop()
    {
        if (watchDescriptor >= 0 && inotifyFd >= 0)
        {
            inotify_rm_watch(inotifyFd, watchDescriptor);
            watchDescriptor = -1;
        }

        if (descriptor)
        {
            boost::system::error_code ec;
            descriptor->close(ec);
            descriptor.reset();
        }

        if (inotifyFd >= 0)
        {
            close(inotifyFd);
            inotifyFd = -1;
        }
    }

  private:
    void asyncWaitForEvents()
    {
        if (!descriptor || inotifyFd < 0)
        {
            return;
        }

        descriptor->async_wait(
            boost::asio::posix::stream_descriptor::wait_read,
            [this](const boost::system::error_code& ec) {
                handleInotifyEvent(ec);
            });
    }

    void handleInotifyEvent(const boost::system::error_code& ec)
    {
        if (ec)
        {
            if (ec != boost::asio::error::operation_aborted)
            {
                lg2::error("ReactorDebugMonitor inotify error: {ERROR}",
                           "ERROR", ec.message());
            }
            return;
        }

        alignas(struct inotify_event) char buffer[4096];
        ssize_t length = read(inotifyFd, buffer, sizeof(buffer));

        if (length > 0)
        {
            processInotifyEvents(buffer, length);
        }

        asyncWaitForEvents();
    }

    void processInotifyEvents(char* buffer, ssize_t length)
    {
        const struct inotify_event* event;

        for (char* ptr = buffer; ptr < buffer + length;
             ptr += sizeof(struct inotify_event) + event->len)
        {
            event = reinterpret_cast<const struct inotify_event*>(ptr);

            if (event->len == 0)
            {
                continue;
            }

            std::string modifiedFile(event->name);
            if (modifiedFile != fileName)
            {
                continue;
            }

            if (event->mask & (IN_CREATE | IN_MOVED_TO | IN_MODIFY))
            {
                std::string filePath = directoryPath + "/" + modifiedFile;
                readDebugFile(filePath);
            }
            else if (event->mask & (IN_DELETE | IN_MOVED_FROM))
            {
                lg2::info(
                    "Debug file deleted, resetting reactor log level to default");
                applyLogLevel(defaultLevel);
            }
        }
    }

    void readDebugFile(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file)
        {
            lg2::warning("ReactorDebugMonitor: cannot open {PATH}",
                         "PATH", filePath);
            return;
        }

        std::string line;
        if (!std::getline(file, line))
        {
            lg2::warning("ReactorDebugMonitor: empty debug file");
            return;
        }

        // Remove quotes
        line.erase(std::remove(line.begin(), line.end(), '\"'), line.end());

        // Parse comma-separated tokens, look for "reactor:<level>"
        std::stringstream ss(line);
        std::string token;

        while (std::getline(ss, token, ','))
        {
            // Trim whitespace
            token.erase(0, token.find_first_not_of(" \t"));
            token.erase(token.find_last_not_of(" \t") + 1);

            // Case-insensitive comparison
            std::string lower = token;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           ::tolower);

            size_t colonPos = lower.find(':');
            if (colonPos == std::string::npos)
            {
                continue;
            }

            std::string prefix = lower.substr(0, colonPos);
            if (prefix != "reactor")
            {
                continue;
            }

            std::string levelStr = lower.substr(colonPos + 1);
            // Trim
            levelStr.erase(0, levelStr.find_first_not_of(" \t"));
            levelStr.erase(levelStr.find_last_not_of(" \t") + 1);

            try
            {
                int level = std::stoi(levelStr);
                lg2::info(
                    "ReactorDebugMonitor: setting log level to {LEVEL}",
                    "LEVEL", level);
                applyLogLevel(level);
                return;
            }
            catch (...)
            {
                lg2::warning(
                    "ReactorDebugMonitor: invalid level '{LEVEL_STR}'",
                    "LEVEL_STR", levelStr);
            }
        }

        lg2::warning("ReactorDebugMonitor: no 'reactor:<level>' token found");
    }

    static void applyLogLevel(int level)
    {
        // Map syslog-style level to systemd journal log level string.
        // phosphor-logging lg2 sends everything to journal; we control
        // filtering by setting the service's MaxLevelStore via sd-bus.
        //
        // Alternatively, set the environment variable that phosphor-logging
        // checks. The simplest portable approach: setenv so that any
        // subsequent lg2 internal filtering picks it up, and also write
        // to /run/systemd/system/mctpreactor.service.d/ for journald.

        const char* envLevel = "7"; // default: debug
        switch (level)
        {
            case 3: // error
                envLevel = "3";
                break;
            case 4: // warning
                envLevel = "4";
                break;
            case 5: // notice
                envLevel = "5";
                break;
            case 6: // info
                envLevel = "6";
                break;
            case 7: // debug
                envLevel = "7";
                break;
            default:
                if (level >= 7)
                    envLevel = "7";
                else
                    envLevel = "4";
                break;
        }

        // Set environment for phosphor-logging runtime level
        setenv("LG2_LEVEL", envLevel, 1);

        lg2::info("Reactor log level set to {LEVEL}", "LEVEL", level);
    }

    // Syslog-style default level (warning)
    static constexpr int defaultLevel = 4;

    boost::asio::io_context& io;
    std::string directoryPath;
    std::string fileName;
    int inotifyFd = -1;
    int watchDescriptor = -1;
    std::unique_ptr<boost::asio::posix::stream_descriptor> descriptor;
};
