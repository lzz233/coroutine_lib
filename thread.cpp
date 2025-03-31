#include "thread.h"

#include <sys/syscall.h>
#include <unistd.h>
#include <iostream>

namespace sylar { // 在大型项目中，不同的模块或库可能会使用相同的类或者函数名，使用命名空间将这些元素包裹起来，避免了冲突。

    // 线程信息
    static thread_local Thread* t_thread          = nullptr; // 当前线程的Thread对象指针
    static thread_local std::string t_thread_name = "UNKNOWN"; // 当前线程的名称
        // static代表生命周期直到程序运行结束时候销毁，
        // thread_local表示线程是本地的，也就是每一个访问到这个类的线程都具有一个副本都会有Thread的指针及当前线程的名字，并且多个线程独立的副本互不影响。
        // thread_local 是 C++11 引入的关键字，用于声明线程局部存储（Thread-Local Storage）变量，其核心作用是让变量在每个线程中拥有独立的副本，实现线程间数据隔离。


    pid_t Thread::GetThreadId()
    {
	    return syscall(SYS_gettid);
        // 直接调用 Linux 内核接口，返回操作系统级别的线程唯一标识符（TID），在系统范围内唯一
    }

    Thread* Thread::GetThis()
    {
        return t_thread;
    }

    const std::string& Thread::GetName()
    {
        return t_thread_name;
    }

    void Thread::SetName(const std::string &name)
    {
        if (t_thread)
        {
            t_thread->m_name = name;
        }
        t_thread_name = name;
    }

    Thread::Thread(std::function<void()> cb, const std::string &name):
    m_cb(cb), m_name(name)
    {
        int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);
        // pthread，也就是POSIX标准的thread，比起C++11的thread复杂，但功能强大
        // 返回 0：表示线程创建成功。此时，新线程的 ID 会通过第一个参数 pthread_t *thread 返回
        // pthread_create四个参数：1.存储新创建线程的唯一标识符 2.设置线程的属性 3.新线程的入口函数 4.向线程函数传递数据
        // 这里的this传入的是当前构造的Thread对象实例的指针。不是rt，rt是函数返回值
        if (rt)
        {
            std::cerr << "pthread_create thread fail, rt=" << rt << " name=" << name;
            // cerr：输出到标准错误的ostream对象，常用于程序错误信息；
            throw std::logic_error("pthread_create error");
        }
        // 等待线程函数完成初始化
        m_semaphore.wait();
    }

    Thread::~Thread()
    {
        if (m_thread)
        {
            pthread_detach(m_thread);
            // pthread_detach的目的是为了确保线程在结束后能够正确地释放资源，而不需要主线程或其他线程进行join操作。
            // 一般适用的场景是在子线程要脱离主线程的管理，并且主线程不需要担心其资源的释放，简化了使用的复杂性。
            // 避免了内存泄漏防止忘记调用join导致的内存泄露
            m_thread = 0;
        }
    }

    // 等待一个线程的终止并回收它的资源
    void Thread::join()
    {
        if (m_thread)
        {
            int rt = pthread_join(m_thread, nullptr);
            // 等待一个线程的终止并回收它的资源
            // 它允许一个线程等待另一个线程的结束。这个函数的作用是阻塞调用它的线程，直到指定的线程结束。
            // 当你需要获取一个线程结束时返回的数据，或者确保一个线程已经运行完毕，这个函数就显得尤为重要。
            if (rt)
            {
                std::cerr << "pthread_join failed, rt = " << rt << ", name = " << m_name << std::endl;
                throw std::logic_error("pthread_join error");
            }
            m_thread = 0;
        }
    }

    void* Thread::run(void* arg) // 负责初始化线程和真正调用线程所需的任务cb
    {
        Thread* thread = (Thread*)arg; // 将传入的 void* 参数转换为 Thread 对象指针

        t_thread       = thread;
        t_thread_name  = thread->m_name;
        thread->m_id   = GetThreadId(); // 返回的是 Linux 内核分配的系统级线程 ID（TID），该 ID 全局唯一，用于操作系统的调度和资源管理
        pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());
        // pthread_self() 返回的是 POSIX 线程库管理的用户级线程 ID（pthread_t 类型），仅在进程内唯一
        // 设置m_name是前15个字节，目的就是设置线程的名字方便调试,
        // 存在可能被问的问题为什么是0，15，由于操作系统对线程名称长度的限制决定的，
        // 在linux中线程最大的名字只能是15，后面还有一个\0总共16.

        std::function<void()> cb;
        cb.swap(thread->m_cb); // swap -> 可以减少m_cb中智能指针的引用计数
        // 直接执行m_cb的话，如果m_cb包含智能指针，执行后可能不会立即释放，导致引用计数残留。
        // 而通过swap，将m_cb转移到cb中，使得原m_cb变为空，从而在执行后，局部变量cb析构时立即减少引用计数，避免内存泄漏。

        // 初始化完成
        thread->m_semaphore.signal(); // 通知主线程初始化完成，可安全释放资源

        cb(); // 执行用户定义的任务逻辑
        return 0;
    }

} 

