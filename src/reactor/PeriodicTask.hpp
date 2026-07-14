#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <functional>

/**
 * @brief A repeating timer that calls a task at a fixed interval
 *
 * Replaces the boilerplate pattern of steady_timer + recursive lambda
 * used throughout MCTPReactorMain. Supports pause/resume for host
 * power state gating.
 */
class PeriodicTask
{
  public:
    template <typename Duration>
    PeriodicTask(boost::asio::io_context& io, Duration interval,
                 std::function<void()> task) :
        timer(io),
        interval(
            std::chrono::duration_cast<std::chrono::nanoseconds>(interval)),
        task(std::move(task))
    {
        schedule();
    }

    ~PeriodicTask()
    {
        try
        {
            timer.cancel();
        }
        catch (...)
        {
            // Destructor must not throw
        }
    }
    PeriodicTask(const PeriodicTask&) = delete;
    PeriodicTask(PeriodicTask&&) = delete;
    PeriodicTask& operator=(const PeriodicTask&) = delete;
    PeriodicTask& operator=(PeriodicTask&&) = delete;

    void pause()
    {
        paused = true;
        timer.cancel();
    }

    void resume()
    {
        if (paused)
        {
            paused = false;
            schedule();
        }
    }

    bool isPaused() const
    {
        return paused;
    }

  private:
    void schedule()
    {
        timer.expires_after(interval);
        timer.async_wait([this](const boost::system::error_code& ec) {
            if (ec || paused)
            {
                return;
            }
            task();
            schedule();
        });
    }

    boost::asio::steady_timer timer;
    std::chrono::nanoseconds interval;
    std::function<void()> task;
    bool paused = false;
};
