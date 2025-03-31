#include "timer.h"

namespace sylar {

    // 这个函数的主要目的是取消一个定时器，删除该定时器的回调函数并将其从定时器管理器中移除。
    bool Timer::cancel()
    {
        // 写锁互斥锁unique_lock+shared_mutex
        std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

        if(m_cb == nullptr) // 如果为空，说明该定时器已经被取消或未初始化
        {
            return false;
        }
        else
        {
            m_cb = nullptr; // 回调函数如果存在设置为nullptr
        }

        auto it = m_manager->m_timers.find(shared_from_this()); // 从定时管理器中找到需要删除的定时器
        // shared_from_this() 返回的 std::shared_ptr 指向当前对象（即调用 shared_from_this() 的 Timer 对象）
        if(it!=m_manager->m_timers.end())
        {
            m_manager->m_timers.erase(it); // 删除定时器
        }
        return true;
    }

    // 刷新定时器超时时间，这个刷新操作会将定时器的下次触发延后。
    bool Timer::refresh()
    {
        std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

        if(!m_cb)
        {
            return false;
        }

        auto it = m_manager->m_timers.find(shared_from_this()); // 同上
        if(it==m_manager->m_timers.end())
        {
            return false;
        }

        m_manager->m_timers.erase(it);
        m_next = std::chrono::system_clock::now() + std::chrono::milliseconds(m_ms);
        // std::chrono::system_clock::now()是C++中用来获取当前系统时间的标准方法，返回的时间是系统(绝对时间)，通常用于记录当前的实际时间点。

        m_manager->m_timers.insert(shared_from_this());
        return true;
    }

    bool Timer::reset(uint64_t ms, bool from_now)
    {
        if(ms==m_ms && !from_now) // 检查是否要重置
        {
            return true;
        }
        // 如果不满足上面的条件需要重置，删除当前的定时器然后重新计算超时时间并重新插入定时器

        {
            std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

            if(!m_cb) // 如果为空，说明该定时器已经被取消或未初始化，因此无法重置
            {
                return false;
            }

            auto it = m_manager->m_timers.find(shared_from_this());
            if(it==m_manager->m_timers.end())
            {
                return false;
            }
            m_manager->m_timers.erase(it);
        }

        // reinsert
        auto start = from_now ? std::chrono::system_clock::now() : m_next - std::chrono::milliseconds(m_ms);
        m_ms = ms;
        m_next = start + std::chrono::milliseconds(m_ms);
        m_manager->addTimer(shared_from_this()); // insert with lock
        return true;
    }

    // Timer构造函数
    Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager):
    m_recurring(recurring), m_ms(ms), m_cb(cb), m_manager(manager)
    {
        auto now = std::chrono::system_clock::now(); // 记录当前绝对时间
        m_next = now + std::chrono::milliseconds(m_ms); // 下一次绝对超时时间
    }

    // 从小到大排序
    bool Timer::Comparator::operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const
    {
        assert(lhs!=nullptr&&rhs!=nullptr);
        return lhs->m_next < rhs->m_next;
    }

    // 初始化当前系统时间，为后续检查系统时间错误时进行校对。
    TimerManager::TimerManager()
    {
        m_previouseTime = std::chrono::system_clock::now();
    }

    TimerManager::~TimerManager()
    {
    }

    // 将一个新定时器添加到定时器管理器中，并在必要时唤醒管理中的线程，准确的来说是在ioscheduler类的阻塞中的epoll，以确保定时器能够及时触发后执行回调函数。
    std::shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring)
    {
        std::shared_ptr<Timer> timer(new Timer(ms, cb, recurring, this));
        addTimer(timer);
        return timer;
    }

    // 如果条件存在 -> 执行cb()
    static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb)
    {
        // 弱引用提升检查条件有效性
        std::shared_ptr<void> tmp = weak_cond.lock();
        if(tmp)
        {
            cb(); // 条件存在才执行回调
        }
    }

    // 条件定时器实现
    // 添加一个条件定时器，并在定时器触发的时候执行的cb的会触发OnTimer，在OnTimer中会真正触发任务。
    std::shared_ptr<Timer> TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring)
    {
        // 通过weak_ptr绑定条件，避免循环引用
        return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
        // 将OnTimer的真正指向交给了第一个addtimer。然后创建timer对象。
    }

    // 获取定时器管理器中下一个定时器的超时时间。
    uint64_t TimerManager::getNextTimer()
    {
        std::shared_lock<std::shared_mutex> read_lock(m_mutex); // 读锁

        // reset m_tickled
        // 重置为未通知状态
        m_tickled = false;

        if (m_timers.empty())
        {
            // 返回最大值（无效值）
            return ~0ull;
        }

        auto now = std::chrono::system_clock::now(); // 获取当前系统时间
        auto time = (*m_timers.begin())->m_next; // 获取最小时间堆中的第一个超时定时器判断超时

        if(now>=time) // 判断当前时间是否已经超过了下一个定时器的超时时间
        {
            // 已经有timer超时
            return 0;
        }
        else
        {
            //计算从当前时间到下一个定时器超时时间的时间差，结果是一个 std::chrono::milliseconds 对象
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time - now);

            //将时间差转换为毫秒，并返回这个值。
            return static_cast<uint64_t>(duration.count());
        }
    }

    // 处理超时定时器的函数，它的主要功能是将所有已经超时的定时器的回调函数收集到一个向量（cbs）中，并处理定时器的循环逻辑
    void TimerManager::listExpiredCb(std::vector<std::function<void()>>& cbs)
    {
        auto now = std::chrono::system_clock::now();

        std::unique_lock<std::shared_mutex> write_lock(m_mutex);

        // 调用 detectClockRollover(now_ms) 检测系统时间是否发生了回滚（即时间被调后）。如果发生回滚，则认为所有定时器都已超时。
        bool rollover = detectClockRollover();

        // 回退 -> 清理所有timer || 超时 -> 清理超时timer,如果rollover为false就没发生系统时间回退
        // 如果时间回滚发生或者定时器的超时时间早于或等于当前时间，则需要处理这些定时器。为什么说早于或等于都要处理，因为超时时间都是基于now后的
        while (!m_timers.empty() && rollover || !m_timers.empty() && (*m_timers.begin())->m_next <= now)
        {
            std::shared_ptr<Timer> temp = *m_timers.begin();
            m_timers.erase(m_timers.begin());

            cbs.push_back(temp->m_cb); // 收集回调延迟执行（减少锁持有时间）

            // 如果定时器是循环的,m_next 属性设置为当前时间加上定时器的间隔（m_ms），然后重新插入到定时器集合中。
            if (temp->m_recurring) // 循环定时器重新插入
            {
                // 重新加入时间堆
                temp->m_next = now + std::chrono::milliseconds(temp->m_ms);
                m_timers.insert(temp); // 触发红黑树重新排序
            }
            else
            {
                // 清理cb，防止悬空回调
                temp->m_cb = nullptr;
            }
        }
    }

    // 查看超时时间堆是否为空
    bool TimerManager::hasTimer()
    {
        std::shared_lock<std::shared_mutex> read_lock(m_mutex);
        return !m_timers.empty();
    }

    // lock + tickle()
    void TimerManager::addTimer(std::shared_ptr<Timer> timer)
    {
        bool at_front = false; // 标识插入的是最早超时的定时器
        {
            std::unique_lock<std::shared_mutex> write_lock(m_mutex);

            //将定时器插入到 m_timers 集合中。由于 m_timers 是一个 std::set，插入时会自动按定时器的超时时间排序。
            auto it = m_timers.insert(timer).first;
            // 判断插入的定时器是否是集合超时时间中最早的定时器
            at_front = (it == m_timers.begin()) && !m_tickled;

            // only tickle once till one thread wakes up and runs getNextTime()
            if(at_front) // 标识有一个新的最早定时器被插入了，防止重复唤醒。
            {
                m_tickled = true;
            }
        }

        if(at_front)
        {
            // wake up
            onTimerInsertedAtFront(); // 虚函数具体执行在ioscheduler
        }
    }

    // 检测系统时间是否发生了回滚(即时间是否倒退)。
    bool TimerManager::detectClockRollover()
    {
        bool rollover = false;
        // 当前时间 now 与上次记录的时间 m_previouseTime 减去一个小时的时间量 (60 * 60 * 1000 毫秒)。
        // 当前时间 now 小于这个时间值，说明系统时间回滚了，因此将rollover设置为true
        auto now = std::chrono::system_clock::now();
        if(now < (m_previouseTime - std::chrono::milliseconds(60 * 60 * 1000)))
        {
            rollover = true;
        }
        m_previouseTime = now;
        return rollover;
    }

}

