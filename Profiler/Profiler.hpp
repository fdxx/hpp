#pragma once 

#include <chrono>

class Profiler
{
public:
    explicit Profiler(bool start = true)
    {
        if (start)
            Start();
    }

    ~Profiler() = default;

    void Start()
    {
        m_elapsed = {}; 
        m_start = clock_t::now();
        m_running = true;
    }

    void Stop()
    {
        if (m_running)
        {
            m_elapsed = clock_t::now() - m_start;
            m_running = false;
        }
    }

    // 返回秒数（浮点）
    float GetTime()
    {
        if (m_running)
            return std::chrono::duration<float>(clock_t::now() - m_start).count();
        return std::chrono::duration<float>(m_elapsed).count();
    }

private:
    using clock_t = std::chrono::high_resolution_clock;
    clock_t::time_point m_start{};
    clock_t::duration   m_elapsed{};
    bool                m_running = false;
};

