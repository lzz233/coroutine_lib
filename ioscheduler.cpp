#include <unistd.h>    
#include <sys/epoll.h> 
#include <fcntl.h>     
#include <cstring>

#include "ioscheduler.h"

static bool debug = true;

namespace sylar {

    IOManager* IOManager::GetThis()
    {
        return dynamic_cast<IOManager*>(Scheduler::GetThis());
    }

    // 根据传入的事件event，返回对应事件上下文的引用。
    IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(Event event)
    {
        assert(event==READ || event==WRITE); // 判断事件要么是读事件，或者写事件
        switch (event)
        {
        case READ:
            return read;
        case WRITE:
            return write;
        }
        throw std::invalid_argument("Unsupported event type");
    }

    // 重置EventContext事件的上下文，将其恢复到初始或者空的状态。主要作用是清理并重置传入的 EventContext 对象，使其不再与任何调度器、线程或回调函数相关联。
    void IOManager::FdContext::resetEventContext(EventContext &ctx)
    {
        ctx.scheduler = nullptr;
        ctx.fiber.reset();
        ctx.cb = nullptr;
    }

    // 函数负责在指定的 IO 事件被触发时，执行相应的回调函数或线程，并且在执行完之后清理相关的事件上下文。
    // no lock
    void IOManager::FdContext::triggerEvent(IOManager::Event event) {
        assert(events & event); // 确保event是中有指定的事件，否则程序中断。

        // delete event
        // 清理该事件，表示不再关注，也就是说，注册IO事件是一次性的，
        // 如果想持续关注某个Socket fd的读写事件，那么每次触发事件后都要重新添加
        events = (Event)(events & ~event); // 对标志位取反再相加就是相当于将event从events中删除

        // trigger
        // 这个过程就相当于scheduler文件中的main.cc测试一样，把真正要执行的函数放入到任务队列中等线程取出后任务后，协程执行，执行完成后返回主协程继续，执行run方法取任务执行任务(不过可能是不同的线程的协程执行了)。
        EventContext& ctx = getEventContext(event);
        if (ctx.cb)
        {
            // call ScheduleTask(std::function<void()>* f, int thr)
            ctx.scheduler->scheduleLock(&ctx.cb);
        }
        else
        {
            // call ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
            ctx.scheduler->scheduleLock(&ctx.fiber);
        }

        // reset event context
        resetEventContext(ctx);
        return;
    }

    IOManager::IOManager(size_t threads, bool use_caller, const std::string &name):
    Scheduler(threads, use_caller, name), TimerManager()
    {
        // create epoll fd
        m_epfd = epoll_create(5000);
        // 5000，epoll_create 的参数实际上在现代 Linux 内核中已经被忽略，最早版本的 Linux 中，这个参数用于指定 epoll 内部使用的事件表的大小。

        assert(m_epfd > 0); // 错误就终止程序

        // create pipe
        int rt = pipe(m_tickleFds); // 创建管道的函数规定了m_tickleFds[0]是读端，1是写段
        assert(!rt); // 错误就终止程序

        // add read event to epoll // 将管道的监听注册到epoll上
        epoll_event event;
        event.events  = EPOLLIN | EPOLLET; // Edge Triggered，设置标志位，并且采用边缘触发和读事件。
        event.data.fd = m_tickleFds[0];

        // non-blocked
        // 修改管道文件描述符以非阻塞的方式，配合边缘触发。
        rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
        // 是一个文件控制函数，用于改变文件描述符的行为。
        // m_tickleFds[0]：是一个文件描述符，在这个例子中是管道的读端。
        // O_NONBLOCK：将文件描述符设置为非阻塞。
        // F_SETFL：是设置文件描述符的标识,如F_GETFD获取文件描述符的标志，F_SETFD设置文件描述符的标志。
        assert(!rt);

        //将 m_tickleFds[0];作为读事件放入到event监听集合中
        rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
        assert(!rt);

        contextResize(32); // 初始化了一个包含 32 个文件描述符上下文的数组

        start(); // 启动 Scheduler，开启线程池，准备处理任务。
    }

    // IOManager的析构函数
    IOManager::~IOManager() {
        stop(); // 关闭scheduler类中的线程池，让任务全部执行完后线程安全退出
        close(m_epfd); // 关闭epoll的句柄（文件描述符）
        close(m_tickleFds[0]); // 关闭管道读端写端
        close(m_tickleFds[1]);

        // 将fdcontext文件描述符一个个关闭
        for (size_t i = 0; i < m_fdContexts.size(); ++i)
        {
            if (m_fdContexts[i])
            {
                delete m_fdContexts[i];
            }
        }
    }

    // no lock
    // 主要作用是调整m_fdContexts数组的大小，并为新的文件描述符(fd)创建并初始化相应的Fdcontext对象。
    void IOManager::contextResize(size_t size)
    {
        m_fdContexts.resize(size); // 调整m_fdContexts的大小

        // 遍历 m_fdContexts 向量，初始化尚未初始化的 FdContext 对象
        for (size_t i = 0; i < m_fdContexts.size(); ++i)
        {
            if (m_fdContexts[i]==nullptr)
            {
                m_fdContexts[i] = new FdContext();
                m_fdContexts[i]->fd = i; // 将文件描述符的编号赋值给 fd
            }
        }
    }

    // 主要作用是为一个上面contextResize()分配好的fd，添加一个event事件，并在事件触发时执行指定的回调函数(cb)或回调协程具体的触发是在triggerEvent。
    int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
    {
        // 查找FdContext对象
        // attemp to find FdContext
        FdContext *fd_ctx = nullptr;

        std::shared_lock<std::shared_mutex> read_lock(m_mutex); // 使用读写锁
        if ((int)m_fdContexts.size() > fd) // 如果说传入的fd在数组里面则查找然后初始化FdContext的对象
        {
            fd_ctx = m_fdContexts[fd];
            read_lock.unlock();
        }
        else // 不存在则重新分配数组的size来初始化FdContext的对象
        {
            read_lock.unlock();
            std::unique_lock<std::shared_mutex> write_lock(m_mutex);
            contextResize(fd * 1.5);
            fd_ctx = m_fdContexts[fd];
        }

        // 一旦找到或者创建Fdcontext的对象后，加上互斥锁，确保Fdcontext的状态不会被其他线程修改
        std::lock_guard<std::mutex> lock(fd_ctx->mutex);

        // the event has already been added
        if(fd_ctx->events & event) // 判断事件是否存在存在？是就返回-1，因为相同的事件不能重复添加
        {
            return -1;
        }

        // add new event
        // 所以这里就很好判断了如果已经存在就fd_ctx->events本身已经有读或写，就是修改已经有事件，如果不存在就是none事件的情况，就添加事件。
        int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        epoll_event epevent;
        epevent.events   = EPOLLET | fd_ctx->events | event;
        epevent.data.ptr = fd_ctx;

        // 函数将事件添加到 epoll 中。如果添加失败，打印错误信息并返回 -1。
        int rt = epoll_ctl(m_epfd, op, fd, &epevent);
        if (rt)
        {
            std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        ++m_pendingEventCount; // 原子计数器，待处理的事件++；

        // update fdcontext
        // 更新 FdContext 的 events 成员，记录当前的所有事件。注意events可以监听读和写的组合，如果fd_ctx->events为none,就相当于直接是fd_ctx->events = event
        fd_ctx->events = (Event)(fd_ctx->events | event);
        // "|"运算符相当于把他俩加起来了，因为二进制中有一个为1，结果就为1

        // update event context
        // 设置事件上下文
        FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
        assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb); // 确保 EventContext 中没有其他正在执行的调度器、协程或回调函数。
        event_ctx.scheduler = Scheduler::GetThis(); //设置调度器为当前的调度器实例Scheduler::GetThis()。

        // 如果提供了回调函数 cb，则将其保存到 EventContext 中；否则，将当前正在运行的协程保存到 EventContext 中，并确保协程的状态是正在运行。
        if (cb)
        {
            event_ctx.cb.swap(cb);
        }
        else
        {
            event_ctx.fiber = Fiber::GetThis(); // 需要确保存在主协程
            assert(event_ctx.fiber->getState() == Fiber::RUNNING);
        }
        return 0;
    }

    // 目的是从IOManager中删除某个文件描述符(fd)的特定事件(event)。
    bool IOManager::delEvent(int fd, Event event) {
        // attemp to find FdContext
        FdContext *fd_ctx = nullptr;

        std::shared_lock<std::shared_mutex> read_lock(m_mutex); // 读锁
        if ((int)m_fdContexts.size() > fd) // 查找FdContext。如果没查找到代表数组中没这个文件描述符，直接返回false；
        {
            fd_ctx = m_fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            return false;
        }

        // 找到后添加互斥锁
        std::lock_guard<std::mutex> lock(fd_ctx->mutex);

        // the event doesn't exist
        if (!(fd_ctx->events & event)) // 如果事件不相同就返回false，否则就继续
        {
            return false;
        }

        // delete the event
        // 因为这里要删除事件，对原有的事件状态取反就是删除原有的状态。比如说传入参数是读事件，我们取反就是删除了这个读事件但可能还要写事件
        Event new_events = (Event)(fd_ctx->events & ~event);
        int op           = (new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL);
        epoll_event epevent;
        epevent.events   = EPOLLET | new_events;
        epevent.data.ptr = fd_ctx; // 这一步是为了在 epoll 事件触发时能够快速找到与该事件相关联的 FdContext 对象。

        int rt = epoll_ctl(m_epfd, op, fd, &epevent);
        if (rt)
        {
            std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        --m_pendingEventCount; // 减少了待处理的事件

        // update fdcontext
        // 因为要先将fd_ctx的状态放入epevent.data.ptr所以就没先去更新，这也是为什么需要单独写Event new_events
        fd_ctx->events = new_events;

        // update event context
        // 重置上下文
        FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
        fd_ctx->resetEventContext(event_ctx);
        return true;
    }

    // 取消特定文件描述符上的指定事件(如读事件或写事件)，并触发该事件的回调函数。
    // 这里相比delEvent不同在于删除事件后，还需要将删除的事件直接交给trigger函数放入到协程调度器中进行触发。
    bool IOManager::cancelEvent(int fd, Event event) {
        // attemp to find FdContext
        FdContext *fd_ctx = nullptr;

        // 这里步骤和delEvent一致
        std::shared_lock<std::shared_mutex> read_lock(m_mutex);
        if ((int)m_fdContexts.size() > fd)
        {
            fd_ctx = m_fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            return false;
        }

        std::lock_guard<std::mutex> lock(fd_ctx->mutex);

        // the event doesn't exist
        if (!(fd_ctx->events & event))
        {
            return false;
        }

        // delete the event
        Event new_events = (Event)(fd_ctx->events & ~event);
        int op           = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events   = EPOLLET | new_events;
        epevent.data.ptr = fd_ctx;

        int rt = epoll_ctl(m_epfd, op, fd, &epevent);
        if (rt)
        {
            std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        --m_pendingEventCount;

        // update fdcontext, event context and trigger
        fd_ctx->triggerEvent(event); // 这个代码和上面那个delEvent一致。就是最后的处理不同一个是重置，一个是调用事件的回调函数
        return true;
    }

    // 取消指定文件描述符(fd)上的所有事件，并且触发这些事件的回调。
    bool IOManager::cancelAll(int fd) {
        // attemp to find FdContext
        FdContext *fd_ctx = nullptr;

        std::shared_lock<std::shared_mutex> read_lock(m_mutex);
        if ((int)m_fdContexts.size() > fd)
        {
            fd_ctx = m_fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            return false;
        }

        std::lock_guard<std::mutex> lock(fd_ctx->mutex);

        // none of events exist
        if (!fd_ctx->events)
        {
            return false;
        }

        // delete all events
        int op = EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events   = 0;
        epevent.data.ptr = fd_ctx;

        int rt = epoll_ctl(m_epfd, op, fd, &epevent);
        if (rt)
        {
            std::cerr << "IOManager::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        // update fdcontext, event context and trigger
        if (fd_ctx->events & READ)
        {
            fd_ctx->triggerEvent(READ);
            --m_pendingEventCount;
        }

        if (fd_ctx->events & WRITE)
        {
            fd_ctx->triggerEvent(WRITE);
            --m_pendingEventCount;
        }

        assert(fd_ctx->events == 0);
        return true;
    }

    // 检测到有空闲线程时，通过写入一个字符到管(m_tickleFds[1]) 中，唤醒那些等待任务的线程。
    void IOManager::tickle()
    {
        // no idle threads
        if(!hasIdleThreads()) // 这个函数在scheduler检查当前是否有线程处于空闲状态。如果没有空闲线程，函数直接返回，不执行后续操作。
        {
            return;
        }
        //如果有空闲线程，函数会向管道 m_tickleFds[1] 写入一个字符 "T"。这个写操作的目的是向等待在 m_tickleFds[0]（管道的另一端）的线程发送一个信号，通知它有新任务可以处理了。
        int rt = write(m_tickleFds[1], "T", 1);
        assert(rt == 1);
    }

    // 检查定时器、挂起事件以及调度器状态，以决定是否可以安全地停止运行。
    bool IOManager::stopping()
    {
        uint64_t timeout = getNextTimer();
        // no timers left and no pending events left with the Scheduler::stopping()
        return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
    }

    // 在没有任务处理时运行(或者即使当前没有任务处理，线程也会在 idle() 中持续休眠并等待新的任务。保证了在所有任务完成之前调度器不会退出)，等待和处理 I/O 事件。
    void IOManager::idle()
    {
        static const uint64_t MAX_EVNETS = 256; // epoll_wait能同时处理的最大事件数为256个

        // 使用 std::unique_ptr 动态分配了一个大小为 MAX_EVENTS 的 epoll_event 数组，用于存储从 epoll_wait 获取的事件。
        std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVNETS]);

        while (true)
        {
            if(debug) std::cout << "IOManager::idle(),run in thread: " << Thread::GetThreadId() << std::endl;

            if(stopping())
            {
                if(debug) std::cout << "name = " << getName() << " idle exits in thread: " << Thread::GetThreadId() << std::endl;
                break;
            }

            // blocked at epoll_wait
            int rt = 0;
            while(true)
            {
                static const uint64_t MAX_TIMEOUT = 5000; //定义了最大超时时间为 5000 毫秒。
                uint64_t next_timeout = getNextTimer(); // 获取下一个超时的定时器
                // 注意：这里的epoll_wait的超时时间，用getNexTimer()从超时时间堆中取出了一开始超时的定时器的时间和epoll_wait原生超时时间5000ms进行一个min的比较。
                next_timeout = std::min(next_timeout, MAX_TIMEOUT);

                // epoll_wait陷入阻塞，等待tickle信号的唤醒，
                // 并且使用了定时器堆中最早超时的定时器作为epoll_wait超时时间。
                rt = epoll_wait(m_epfd, events.get(), MAX_EVNETS, (int)next_timeout);
                // EINTR -> retry
                if(rt < 0 && errno == EINTR) // rt小于0代表无限阻塞，errno是EINTR(表示信号中断)
                {
                    continue;
                }
                else
                {
                    break;
                }
            };

            // collect all timers overdue
            std::vector<std::function<void()>> cbs; // 用于存储超时的回调函数。
            listExpiredCb(cbs); // 用来获取所有超时的定时器回调，并将它们添加到 cbs 向量中。
            if(!cbs.empty())
            {
                for(const auto& cb : cbs)
                {
                    scheduleLock(cb);
                }
                cbs.clear();
            }

            // collect all events ready
            // 遍历所有的rt，代表有多少个事件准备了。
            for (int i = 0; i < rt; ++i)
            {
                epoll_event& event = events[i]; // 获取第 i 个 epoll_event，用于处理该事件。

                // tickle event
                // 检查当前事件是否是 tickle 事件（即用于唤醒空闲线程的事件）。
                if (event.data.fd == m_tickleFds[0]) // 检查事件是否来自管道的读端
                {
                    uint8_t dummy[256];
                    // edge triggered -> exhaust
                    while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0); // 因为是edge trigger(ET)模式循环读取管道数据直到清空
                    continue; // 跳过后续事件处理
                }

                // other events
                // 通过 event.data.ptr 获取与当前事件关联的 FdContext 指针 fd_ctx，该指针包含了与文件描述符相关的上下文信息。
                FdContext *fd_ctx = (FdContext *)event.data.ptr;
                std::lock_guard<std::mutex> lock(fd_ctx->mutex);

                // convert EPOLLERR or EPOLLHUP to -> read or write event
                // 如果当前事件是错误或挂起（EPOLLERR 或 EPOLLHUP），则将其转换为可读或可写事件（EPOLLIN 或 EPOLLOUT），以便后续处理。
                if (event.events & (EPOLLERR | EPOLLHUP))
                {
                    event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
                }
                // events happening during this turn of epoll_wait
                // 确定实际发生的事件类型（读取、写入或两者）。
                int real_events = NONE;
                if (event.events & EPOLLIN)
                {
                    real_events |= READ;
                }
                if (event.events & EPOLLOUT)
                {
                    real_events |= WRITE;
                }

                if ((fd_ctx->events & real_events) == NONE)
                {
                    continue;
                }

                // delete the events that have already happened
                // 这里进行取反就是计算剩余未发送的的事件
                int left_events = (fd_ctx->events & ~real_events);
                int op          = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
                //如果left_event没有事件了那么就只剩下边缘触发了events设置了
                event.events    = EPOLLET | left_events;

                // 根据之前计算的操作（op），调用 epoll_ctl 更新或删除 epoll 监听，如果失败，打印错误并继续处理下一个事件。
                int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
                if (rt2)
                {
                    std::cerr << "idle::epoll_ctl failed: " << strerror(errno) << std::endl;
                    continue;
                }

                // schedule callback and update fdcontext and event context
                // 触发事件，事件的执行
                if (real_events & READ)
                {
                    fd_ctx->triggerEvent(READ);
                    --m_pendingEventCount;
                }
                if (real_events & WRITE)
                {
                    fd_ctx->triggerEvent(WRITE);
                    --m_pendingEventCount;
                }
            } // end for

            // 当前线程的协程主动让出控制权，调度器可以选择执行其他任务或再次进入 idle 状态。
            Fiber::GetThis()->yield();

        } // end while(true)
    }

    // 函数的作用是在定时器被插入到最前面时，触发tickle事件，唤醒阻塞的epoll_wait回收超时的定时任务(回调cb和协程)放入协程调度器中等待调度。
    void IOManager::onTimerInsertedAtFront()
    {
        tickle();
    }

} // end namespace sylar