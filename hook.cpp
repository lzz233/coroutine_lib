#include "hook.h"
#include "ioscheduler.h"
#include <dlfcn.h>
#include <iostream>
#include <cstdarg>
#include "fd_manager.h"
#include <string.h>

// apply XX to all functions
#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt) 

namespace sylar{

    // if this thread is using hooked function
    // 使用线程局部变量，每个线程都会判断一下是否启用了钩子
    static thread_local bool t_hook_enable = false; // 表示当前线程是否启用了钩子功能。初始值为 false，即钩子功能默认关闭。
    bool is_hook_enable() // 返回当前线程的钩子功能是否启用。
    {
        return t_hook_enable;
    }

    void set_hook_enable(bool flag) // 设置当前线程的钩子功能是否启用。
    {
        t_hook_enable = flag;
    }

    void hook_init()
    {
        static bool is_inited = false; // 通过一个静态局部变量来确保 hook_init() 只初始化一次，防止重复初始化。
        // 第一次调用时初始化为 false，后续调用跳过初始化
        if(is_inited)
        {
            return;
        }

        // test
        is_inited = true;

        // assignment -> sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep"); -> dlsym -> fetch the original symbols/function
        #define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
        HOOK_FUN(XX)
        #undef XX
    }

    // static variable initialisation will run before the main function
    struct HookIniter
    {
        HookIniter() // 构造函数
        {
            hook_init(); // 初始化hook，让原始调用绑定到宏展开的函数指针中
        }
    };

    static HookIniter s_hook_initer; // 定义了一个静态的 HookIniter 实例。由于静态变量的初始化发生在 main() 函数之前，所以 hook_init() 会在程序开始时被调用，从而初始化钩子函数。

} // end namespace sylar

// 跟踪定时器的状态
struct timer_info
{
    int cancelled = 0; // 用于表示定时器是否已经被取消。
};

// do_io的通用模板：
// 可以发现项目代码中的：自定义的系统调用最后都将其参数放入do_io模板来做一个统一的规范化
// do_io主要是判断全局钩子是否启用，并且根据文件描述符的是否有效，和是否设置了非阻塞，来选择是否使用原始系统调用
// universal template for read and write function
template<typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char* hook_fun_name, uint32_t event, int timeout_so, Args&&... args)
{
    if(!sylar::t_hook_enable) // 如果全局钩子功能未启用，则直接调用原始的 I/O 函数。
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取与文件描述符 fd 相关联的上下文 ctx。如果上下文不存在，则直接调用原始的 I/O 函数。
    // typedef Singleton<FdManager> FdMgr各位彦祖不要忘记了。
    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
    if(!ctx) // 如果在Fdmanager类中没找到相应的fd那就调用原始的系统调用
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    if(ctx->isClosed()) // 如果文件描述符已经关闭，设置errno为EBADF并返回-1。
    {
        errno = EBADF; //表示文件描述符无效或已经关闭
        return -1;
    }

    // 如果文件描述符不是一个socket或者用户设置了非阻塞模式，则直接调用原始的I/O操作函数。
    if(!ctx->isSocket() || ctx->getUserNonblock())
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // get the timeout
    // 获取超时设置并初始化timer_info结构体，用于后续的超时管理和取消操作。
    uint64_t timeout = ctx->getTimeout(timeout_so);
    // timer condition
    std::shared_ptr<timer_info> tinfo(new timer_info);

    // 调用原始的I/O函数，如果由于系统中断（EINTR）导致操作失败，函数会重试。
retry:
	// run the function
    ssize_t n = fun(fd, std::forward<Args>(args)...);

    // EINTR ->Operation interrupted by system ->retry
    while(n == -1 && errno == EINTR)
    {
        n = fun(fd, std::forward<Args>(args)...);
    }

    // 0 resource was temporarily unavailable -> retry until ready
    // 如果I/O操作因为资源暂时不可用（EAGAIN）而失败，函数会添加一个事件监听器来等待资源可用。同时，如果有超时设置，还会启动一个条件计时器来取消事件。
    if(n == -1 && errno == EAGAIN)
    {
        sylar::IOManager* iom = sylar::IOManager::GetThis();
        // timer
        std::shared_ptr<sylar::Timer> timer;
        std::weak_ptr<timer_info> winfo(tinfo);

        // 1 timeout has been set -> add a conditional timer for canceling this operation
        // 如果执行的read等函数在Fdmanager管理的Fdctx中fd设置了超时时间，就会走到这里。添加addconditionTimer事件
        if(timeout != (uint64_t)-1)
        {
            timer = iom->addConditionTimer(timeout, [winfo, fd, iom, event]()
            {
                auto t = winfo.lock();
                if(!t || t->cancelled) // 如果 timer_info 对象已被释放（!t），或者操作已被取消（t->cancelled 非 0），则直接返回。
                {
                    return;
                }
                t->cancelled = ETIMEDOUT; // 如果超时时间到达并且事件尚未被处理(即cancelled任然是0)；
                // cancel this event and trigger once to return to this fiber
                // 取消该文件描述符上的事件，并立即触发一次事件（即恢复被挂起的协程）
                iom->cancelEvent(fd, (sylar::IOManager::Event)(event));
            }, winfo);
        }

        // 2 add event -> callback is this fiber
        // 这行代码的作用是将 fd（文件描述符）和 event（要监听的事件，如读或写事件）添加到 IOManager 中进行管理。IOManager 会监听这个文件描述符上的事件，当事件触发时，它会调度相应的协程来处理这个事件。
        int rt = iom->addEvent(fd, (sylar::IOManager::Event)(event));
        if(rt)
        {
            std::cout << hook_fun_name << " addEvent("<< fd << ", " << event << ")";
            if(timer) // 如果 rt 为-1，说明 addEvent 失败。此时，会打印一条调试信息，并且因为添加事件失败所以要取消之前设置的定时器，避免误触发。
            {
                timer->cancel();
            }
            return -1;
        }
        else // 如果 addEvent 成功（rt 为 0），当前协程会调用 yield() 函数，将自己挂起，等待事件的触发。
        {
            sylar::Fiber::GetThis()->yield();

            // 3 resume either by addEvent or cancelEvent
            // 当协程被恢复时（例如，事件触发后），它会继续执行 yield() 之后的代码。
            // 如果之前设置了定时器（timer 不为 nullptr），则在事件处理完毕后取消该定时器。
            // 取消定时器的原因是，该定时器的唯一目的是在 I/O 操作超时时取消事件。如果事件已经正常处理完毕，那么定时器就不再需要了。
            if(timer)
            {
                timer->cancel();
            }
            // by cancelEvent
            // 接下来检查 tinfo->cancelled 是否等于 ETIMEDOUT。
            // 如果等于，说明该操作因超时而被取消，因此设置 errno 为 ETIMEDOUT 并返回 -1，表示操作失败。
            if(tinfo->cancelled == ETIMEDOUT)
            {
                errno = tinfo->cancelled;
                return -1;
            }
            // 如果没有超时，则跳转到 retry 标签，重新尝试这个操作。
            goto retry;
        }
    }
    return n;
}



extern "C"{

    // declaration -> sleep_fun sleep_f = nullptr;
    #define XX(name) name ## _fun name ## _f = nullptr;
	    HOOK_FUN(XX)
    #undef XX

    // only use at task fiber
    // 协程版本的sleep，通过钩子机制拦截sleep的调用，并将其改为使用协程来实现非阻塞的休眠。
    unsigned int sleep(unsigned int seconds)
    {
	    if(!sylar::t_hook_enable) // 如果钩子未启用，则调用原始的系统调用
	    {
		    return sleep_f(seconds);
	    }

	    // 获取当前正在执行的协程（Fiber），并将其保存到 fiber 变量中。
	    std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
	    sylar::IOManager* iom = sylar::IOManager::GetThis();
	    // add a timer to reschedule this fiber
	    // sleep*1000是转换微秒。
	    iom->addTimer(seconds*1000, [fiber, iom](){iom->scheduleLock(fiber, -1);});
	    // wait for the next resume
	    fiber->yield(); // 挂起当前协程的执行，将控制权交还给调度器。
	    return 0;
    }

    // useconds_t一个无符号整数类型，通常用于表示微秒数。
    int usleep(useconds_t usec)
    {
	    if(!sylar::t_hook_enable)
	    {
		    return usleep_f(usec);
	    }

	    std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
	    sylar::IOManager* iom = sylar::IOManager::GetThis();
	    // add a timer to reschedule this fiber
	    // usec表示延时的微秒数，将其转换为毫秒数(usec/1000)后用于定时器。
	    iom->addTimer(usec/1000, [fiber, iom](){iom->scheduleLock(fiber);});
	    // wait for the next resume
	    fiber->yield();
	    return 0;
    }

    // 纳秒级别的睡眠。
    int nanosleep(const struct timespec* req, struct timespec* rem)
    {
	    if(!sylar::t_hook_enable)
	    {
		    return nanosleep_f(req, rem);
	    }

	    // timeout_ms 将 tv_sec 转换为毫秒，并将 tv_nsec 转换为毫秒，然后两者相加得到总的超时毫秒数。所以从这里看出实现的也是一个毫秒级的操作。
	    int timeout_ms = req->tv_sec*1000 + req->tv_nsec/1000/1000;

	    std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
	    sylar::IOManager* iom = sylar::IOManager::GetThis();
	    // add a timer to reschedule this fiber
	    iom->addTimer(timeout_ms, [fiber, iom](){iom->scheduleLock(fiber, -1);});
	    // wait for the next resume
	    fiber->yield();
	    return 0;
    }

    int socket(int domain, int type, int protocol)
    {
	    if(!sylar::t_hook_enable)
	    {
		    return socket_f(domain, type, protocol);
	    }

	    // 如果钩子启用了，则通过调用原始的 socket 函数创建套接字，并将返回的文件描述符存储在 fd 变量中。
	    int fd = socket_f(domain, type, protocol);
	    if(fd==-1)
	    {
		    std::cerr << "socket() failed:" << strerror(errno) << std::endl;
		    return fd;
	    }
	    // 如果socket创建成功会利用Fdmanager的文件描述符管理类来进行管理，判断是否在其管理的文件描述符中，如果不在扩展存储文件描述数组大小，并且利用FDctx进行初始化判断是是不是套接字，是不是系统非阻塞模式。
	    sylar::FdMgr::GetInstance()->get(fd, true);
	    return fd;
    }

    // connect_with_timeout函数实际上是在原始connect系统调用基础上，增加了超时控制的逻辑。
    // 连接超时情况下处理非阻塞套接字连接的实现。
    // 它首先尝试使用钩子功能来捕获并管理连接请求的行为，然后使用IOManager和Timer来管理超时机制
    int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms)
    {
        if(!sylar::t_hook_enable)
        {
            return connect_f(fd, addr, addrlen);
        }

        std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
        if(!ctx || ctx->isClosed()) // 检查文件描述符上下文是否存在或是否已关闭。
        {
            errno = EBADF; // EBADF表示一个无效的文件描述符
            return -1;
        }

	    // 如果不是一个套接字调用原始的
        if(!ctx->isSocket())
        {
            return connect_f(fd, addr, addrlen);
        }

	    // 检查用户是否设置了非阻塞模式。如果是非阻塞模式，
        if(ctx->getUserNonblock())
        {

            return connect_f(fd, addr, addrlen);
        }

        // attempt to connect
        int n = connect_f(fd, addr, addrlen); // 尝试进行 connect 操作，返回值存储在 n 中。
        if(n == 0)
        {
            return 0;
        }
        else if(n != -1 || errno != EINPROGRESS)
        {
            return n;
        }

        // wait for write event is ready -> connect succeeds
	    sylar::IOManager* iom = sylar::IOManager::GetThis(); // 获取当前线程的 IOManager 实例。
	    std::shared_ptr<sylar::Timer> timer; // 声明一个定时器对象。
	    std::shared_ptr<timer_info> tinfo(new timer_info); // 创建追踪定时器是否取消的对象
	    std::weak_ptr<timer_info> winfo(tinfo); // 判断追踪定时器对象是否存在

        if(timeout_ms != (uint64_t)-1) // 检查是否设置了超时时间。如果 timeout_ms 不等于 -1，则创建一个定时器。
        {
            timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]()
            {
                auto t = winfo.lock();
                if(!t || t->cancelled)
                {
                    return;
                }
                t->cancelled = ETIMEDOUT;
                iom->cancelEvent(fd, sylar::IOManager::WRITE);
            }, winfo);
        }

        int rt = iom->addEvent(fd, sylar::IOManager::WRITE);
        if(rt == 0)
        {
            sylar::Fiber::GetThis()->yield();

            // resume either by addEvent or cancelEvent
            if(timer)
            {
                timer->cancel();
            }

            if(tinfo->cancelled)
            {
                errno = tinfo->cancelled;
                return -1;
            }
        }
        else
        {
            if(timer)
            {
                timer->cancel();
            }
            std::cerr << "connect addEvent(" << fd << ", WRITE) error";
        }

        // check out if the connection socket established
        int error = 0;
        socklen_t len = sizeof(int);
        if(-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len))
        {
            return -1;
        }
        if(!error)
        {
            return 0;
        }
        else
        {
            errno = error;
            return -1;
        }
    }


    // connect_with_timeout函数实际上是在原始connect系统调用基础上，增加了超时控制的逻辑。
    static uint64_t s_connect_timeout = -1; //s_connect_timeout 是一个 static 变量，表示默认的连接超时时间，类型为 uint64_t，可以存储 64 位无符号整数。
    // -1 通常用于表示一个无效或未设置的值。由于它是无符号整数，-1 实际上会被解释为 UINT64_MAX，表示没有超时限制。
    int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
    {
        return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout); // 调用hook启用后的connect_with_timeout函数
    }

    // 和recv等函数一样使用了do_io的模板，实现了非阻塞accpet的操作
    // 并且如果成功接收了一个新的连接，则将新的文件描述符fd添加到文件描述符管理器(FdManager)中进行跟踪管理。
    int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
    {
	    int fd = do_io(sockfd, accept_f, "accept", sylar::IOManager::READ, SO_RCVTIMEO, addr, addrlen);
	    if(fd>=0)
	    {
		    sylar::FdMgr::GetInstance()->get(fd, true);
	    }
	    return fd;
    }

    ssize_t read(int fd, void *buf, size_t count)
    {
	    return do_io(fd, read_f, "read", sylar::IOManager::READ, SO_RCVTIMEO, buf, count);
    }

    ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
    {
	    return do_io(fd, readv_f, "readv", sylar::IOManager::READ, SO_RCVTIMEO, iov, iovcnt);
    }

    ssize_t recv(int sockfd, void *buf, size_t len, int flags)
    {
	    return do_io(sockfd, recv_f, "recv", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags);
    }

    ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    {
	    return do_io(sockfd, recvfrom_f, "recvfrom", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
    }

    ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
    {
	    return do_io(sockfd, recvmsg_f, "recvmsg", sylar::IOManager::READ, SO_RCVTIMEO, msg, flags);
    }

    ssize_t write(int fd, const void *buf, size_t count)
    {
	    return do_io(fd, write_f, "write", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, count);
    }

    ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
    {
	    return do_io(fd, writev_f, "writev", sylar::IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);
    }

    ssize_t send(int sockfd, const void *buf, size_t len, int flags)
    {
	    return do_io(sockfd, send_f, "send", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags);
    }

    ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    {
	    return do_io(sockfd, sendto_f, "sendto", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);
    }

    ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
    {
	    return do_io(sockfd, sendmsg_f, "sendmsg", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, flags);
    }

    // 很简单，就是将所有文件描述符的事件处理了。
    // 调用IOManager的cancelAll函数将fd上的读写事件全部处理，最后从FdManger文件描述符管理中移除该fd
    int close(int fd)
    {
	    if(!sylar::t_hook_enable)
	    {
		    return close_f(fd);
	    }

	    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);

	    if(ctx)
	    {
		    auto iom = sylar::IOManager::GetThis();
		    if(iom)
		    {
			    iom->cancelAll(fd);
		    }
		    // del fdctx
		    sylar::FdMgr::GetInstance()->del(fd);
	    }
	    return close_f(fd);
    }

    // 操作文件描述符的系统调用，可以执行多种操作，比如设置文件描述符状态，锁定文件等。
    // 这里的可变参数应该只有一个，因为后面获取arg都是只获取了一次
    int fcntl(int fd, int cmd, ... /* arg */ )
    {
  	    va_list va; // to access a list of mutable parameters

        va_start(va, cmd);
        switch(cmd)
        {
            case F_SETFL: // 用于设置文件描述符的状态标志（例如，设置非阻塞模式）。
                {
                    int arg = va_arg(va, int); // Access the next int argument
                    va_end(va);
                    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
                    if(!ctx || ctx->isClosed() || !ctx->isSocket())
                    {
                        return fcntl_f(fd, cmd, arg);
                    }
                    // 用户是否设定了非阻塞
                    ctx->setUserNonblock(arg & O_NONBLOCK);
                    // 最后是否阻塞根据系统设置决定
                    if(ctx->getSysNonblock())
                    {
                        arg |= O_NONBLOCK;
                    }
                    else
                    {
                        arg &= ~O_NONBLOCK;
                    }
                    return fcntl_f(fd, cmd, arg);
                }
                break;

            case F_GETFL:
                {
                    va_end(va);
                    int arg = fcntl_f(fd, cmd);
                    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
                    if(!ctx || ctx->isClosed() || !ctx->isSocket())
                    {
                        return arg;
                    }
                    // 这里是呈现给用户 显示的为用户设定的值
                    // 但是底层还是根据系统设置决定的
                    if(ctx->getUserNonblock())
                    {
                        return arg | O_NONBLOCK;
                    } else
                    {
                        return arg & ~O_NONBLOCK;
                    }
                }
                break;

            case F_DUPFD:
            case F_DUPFD_CLOEXEC:
            case F_SETFD:
            case F_SETOWN:
            case F_SETSIG:
            case F_SETLEASE:
            case F_NOTIFY:

            // F_SETPIPE_SZ是一个可能并非在所有系统中都可以用的宏。
            // 没有定义就不会进入到ifdef和endif内了。
    #ifdef F_SETPIPE_SZ
            case F_SETPIPE_SZ:
    #endif
                {
                    int arg = va_arg(va, int);
                    va_end(va);
                    return fcntl_f(fd, cmd, arg);
                }
                break;


            case F_GETFD:
            case F_GETOWN:
            case F_GETSIG:
            case F_GETLEASE:
    #ifdef F_GETPIPE_SZ
            case F_GETPIPE_SZ:
    #endif
                {
                    va_end(va);
                    return fcntl_f(fd, cmd);
                }
                break;

            case F_SETLK:
            case F_SETLKW:
            case F_GETLK:
                {
                    struct flock* arg = va_arg(va, struct flock*);
                    va_end(va);
                    return fcntl_f(fd, cmd, arg);
                }
                break;

            case F_GETOWN_EX:
            case F_SETOWN_EX:
                {
                    struct f_owner_exlock* arg = va_arg(va, struct f_owner_exlock*);
                    va_end(va);
                    return fcntl_f(fd, cmd, arg);
                }
                break;

            default:
                va_end(va);
                return fcntl_f(fd, cmd);
        }
    }

    // 实际处理了文件描述符(fd)上的ioctl系统调用，并在特定条件下对FIONBIO(用于设置非阻塞模式)进行了特殊处理。
    int ioctl(int fd, unsigned long request, ...)
    {
        // va_list是可变参数列表的指针
        va_list va; // va持有处理可变参数的状态信息
        va_start(va, request); // 给va初始化让它指向可变参数的第一个参数位置。
        void* arg = va_arg(va, void*); // 将va的指向参数的以void*类型取出存放到arg中
        va_end(va); // 用于结束对 va_list 变量的操作。清理va占用的资源

        if(FIONBIO == request) // 用于设置非阻塞模式的命令
        {
            // ！！是为了保证明确的将其转为成一个bool类型，比如一开始结果是true，经过一次!转换成了false，然后！再一次转换成了true；
            bool user_nonblock = !!*(int*)arg; // 当前 ioctl 调用是为了设置或清除非阻塞模式。
            std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
            // 检查获取的上下文对象是否有效（即 ctx 是否为空）。如果上下文对象无效、文件描述符已关闭或不是一个套接字，则直接调用原始的 ioctl 函数，返回处理结果。
            if(!ctx || ctx->isClosed() || !ctx->isSocket())
            {
                return ioctl_f(fd, request, arg);
            }
            // 如果上下文对象有效，调用其 setUserNonblock 方法，将非阻塞模式设置为 user_nonblock 指定的值。这将更新文件描述符的非阻塞状态。
            ctx->setUserNonblock(user_nonblock);
        }
        return ioctl_f(fd, request, arg);
    }

    // setsockopt函数就是用来设置套接字，getsockopt就是用来查询套接字的状态和配置信息
    // 获取套接字选项值
    int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
    {
	    return getsockopt_f(sockfd, level, optname, optval, optlen);
    }

    // 设置套接字的超时时间、缓冲区大小、TCP协议选项
    int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
    {
        if(!sylar::t_hook_enable)
        {
            return setsockopt_f(sockfd, level, optname, optval, optlen);
        }

        // SOL_SOCKET 的全称是 Socket Level Socket，表示套接字级别的选项。它用于在 setsockopt 或 getsockopt 函数中指定操作的是通用套接字层的选项
        // （如 SO_REUSEADDR、SO_KEEPALIVE 等），而非特定协议（如 IPPROTO_TCP 或 IPPROTO_IP）的选项

        // 如果 level 是 SOL_SOCKET 且 optname 是 SO_RCVTIMEO（接收超时）或 SO_SNDTIMEO（发送超时），代码会获取与该文件描述符关联的 FdCtx 上下文对象：
        if(level == SOL_SOCKET)
        {
            if(optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)
            {
                std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(sockfd);
                if(ctx)
                {	// 那么代码会读取传入的 timeval 结构体，将其转化为毫秒数，并调用 ctx->setTimeout 方法，记录超时设置：
                    const timeval* v = (const timeval*)optval;
                    ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
                }
            }
        } // 无论是否执行了超时处理，最后都会调用原始的 setsockopt_f 函数来设置实际的套接字选项。
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }

}
