#ifndef _THREAD_H_
#define _THREAD_H_

#include <mutex>
#include <condition_variable>
#include <functional>
#include <string>

namespace sylar
{

// 用于线程方法间的同步
class Semaphore // 信号量
{
private:
    std::mutex mtx; // 互斥锁
    std::condition_variable cv; // 条件变量
    int count; // 信号量的计数

public:
    // 信号量初始化为0
    explicit Semaphore(int count_ = 0) : count(count_) {} // explicit不允许隐式类型转换，提高代码可读性
    
    // P操作，请求资源
    void wait() 
    {
        std::unique_lock<std::mutex> lock(mtx); // 这里不使用lock_guard的原因是不允许手动解锁，还有因为wait中会将锁释放，如果使用lock_guard函数没有结束的话释放不了锁
        while (count == 0) { // 这里是为了防止虚假唤醒，直到count>0才跳出循环。信号量大于0你再拿
            cv.wait(lock); // wait for signals
        }
        count--;
    }

    // V操作，释放资源。这里是负责给count++，然后通知wait唤醒等待的线程
    void signal() 
    {
        std::unique_lock<std::mutex> lock(mtx); // 出了代码块自动析构，自动开锁
        // 假设 count 初始为 0，两个线程同时执行 count++，若无锁保护，可能两个线程都读取到 0，最终结果为 1 而非 2。
        count++;
        cv.notify_one();  // signal，要注意这里的one指的不是只有一个可能是多个线程
    }
};

// 一共两种线程: 1 由系统自动创建的主线程 2 由Thread类创建的线程 
class Thread 
{
public:
    Thread(std::function<void()> cb, const std::string& name);
    // std::function<void()> 表示一个通用可调用对象的封装器，用于定义线程执行的入口函数
    // cb是线程真正需要运行的任务
    // 通过 std::function，线程构造函数无需关心用户传入的是函数指针、Lambda 还是仿函数。
    ~Thread();

    pid_t getId() const { return m_id; } // pid_t定义的类型都是进程号类型，这里时一个long long类型变量
    const std::string& getName() const { return m_name; }

    void join();

public:
    // 获取系统分配的线程id
	static pid_t GetThreadId();
    // 获取当前所在线程
    static Thread* GetThis();

    // 获取当前线程的名字
    static const std::string& GetName();
    // 设置当前线程的名字
    static void SetName(const std::string& name);

private:
	// 线程函数
    static void* run(void* arg);
    // 为什么run()要设置成静态？
    // pthread_create 的函数指针参数要求是 C 风格函数（即普通函数或静态函数），不能直接传递非静态成员函数
    // pthread_create 的线程函数签名应为 void* (*)(void*)，而成员函数的实际签名是 void* (ClassName::*)(void*)，导致类型不兼容

private:
    pid_t m_id = -1; // 线程的id
    pthread_t m_thread = 0; // 线程的唯一标识符

    // 线程需要运行的函数
    std::function<void()> m_cb;
    std::string m_name; // 线程的name

    Semaphore m_semaphore; // 引入信号量的类来完成线程的同步创建。
};
}
#endif
