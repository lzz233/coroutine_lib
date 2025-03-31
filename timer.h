#ifndef __SYLAR_TIMER_H__
#define __SYLAR_TIMER_H__

#include <memory>
#include <vector>
#include <set>
#include <shared_mutex>
#include <assert.h>
#include <functional>
#include <mutex>

namespace sylar {

class TimerManager; // 定时器管理类
    // 继承的public是用来返回智能指针timer的this值
class Timer : public std::enable_shared_from_this<Timer> 
{
    friend class TimerManager; // 设置成友元访问timerManager类的函数和成员变量
public:
    // 从时间堆中删除timer
    bool cancel();

    // 刷新timer
    bool refresh();
    // 重设timer的超时时间
    // ms定时器执行间隔时间(ms)，from_now是否从当前时间开始计算
    bool reset(uint64_t ms, bool from_now);

private:
    Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager);
 
private:
    // 是否循环
    bool m_recurring = false;
    // 超时时间。指超出计时的总时间，而不是两个时间的差值
    uint64_t m_ms = 0;
    // 绝对超时时间，即该定时器下一次触发的时间点。
    std::chrono::time_point<std::chrono::system_clock> m_next;
    // 超时时触发的回调函数
    std::function<void()> m_cb;
    // 管理此timer的管理器
    TimerManager* m_manager = nullptr;

private:
    // 实现最小堆(set)的比较函数，用于比较两个Timer对象，比较的依据是绝对超时时间。
    struct Comparator 
    {
        bool operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const;
    };
};

class TimerManager 
{
    friend class Timer;
public:
    TimerManager();
    virtual ~TimerManager();

    // 添加timer
    // ms定时器执行间隔时间
    // cb定时器回调函数
    // recurring是否循环定时器
    std::shared_ptr<Timer> addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);

    // 添加条件timer
    // weak_cond条件
    std::shared_ptr<Timer> addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring = false);

    // 拿到堆中最近的超时时间
    uint64_t getNextTimer();

    // 取出所有超时定时器的回调函数
    void listExpiredCb(std::vector<std::function<void()>>& cbs);

    // 堆中是否有timer
    bool hasTimer();

protected:
    // 当一个最早的timer加入到堆中 -> 调用该函数
    virtual void onTimerInsertedAtFront() {};

    // 添加timer
    void addTimer(std::shared_ptr<Timer> timer);

private:
    // 当系统时间改变时 -> 调用该函数
    bool detectClockRollover();

private:
    std::shared_mutex m_mutex;
    // 时间堆。不过这里好像用的是set自动排序，而不是最小堆
    std::set<std::shared_ptr<Timer>, Timer::Comparator> m_timers;
    // 存储所有的 Timer 对象，并使用 Timer::Comparator 进行排序，确保最早超时的 Timer 在最前面。

    // 在下次getNextTimer()执行前 onTimerInsertedAtFront()是否已经被触发了 -> 在此过程中 onTimerInsertedAtFront()只执行一次
    // 表示是否已经通知过事件循环，也就是onTimerInsertedAtFront()。如果getNextTimer()，获取了新时钟了，也就要标记为没通知
    bool m_tickled = false;
    // 上次检查系统时间是否回退的绝对时间
    std::chrono::time_point<std::chrono::system_clock> m_previouseTime;
};

}

#endif