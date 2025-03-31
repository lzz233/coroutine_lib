#include "fd_manager.h"
#include "hook.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sylar{

	// instantiate
	template class Singleton<FdManager>; // FdManager 类有一个全局唯一的单例实例。

	// Static variables need to be defined outside the class
	// 这些行代码定义了 Singleton 类模板的静态成员变量 instance 和 mutex。静态成员变量需要在类外部定义和初始化。
	template<typename T>
	T* Singleton<T>::instance = nullptr;

	template<typename T>
	std::mutex Singleton<T>::mutex;

	FdCtx::FdCtx(int fd):
	m_fd(fd)
	{
		init();
	}

	FdCtx::~FdCtx()
	{

	}

	bool FdCtx::init() {
		if (m_isInit) { // 如果已经初始化过了就直接返回 true
			return true;
		}

		struct stat statbuf;
		// fd is in valid
		// fstat 函数用于获取与文件描述符 m_fd 关联的文件状态信息存放到 statbuf 中。如果 fstat() 返回 -1，表示文件描述符无效或出现错误。
		if (-1 == fstat(m_fd, &statbuf)) {
			m_isInit = false;
			m_isSocket = false;
		} else {
			m_isInit = true;
			m_isSocket = S_ISSOCK(statbuf.st_mode); // S_ISSOCK(statbuf.st_mode) 用于判断文件类型是否为套接字
		}

		// if it is a socket -> set to nonblock
		if (m_isSocket) { // 表示 m_fd 关联的文件是一个套接字：
			int flags = fcntl_f(m_fd, F_GETFL, 0); // 获取文件描述符的状态
			if (!(flags & O_NONBLOCK)) {
				fcntl_f(m_fd, F_SETFL, flags | O_NONBLOCK); // 检查当前标志中是否已经设置了非阻塞标志。如果没有设置：
			}
			m_sysNonblock = true; // hook 非阻塞设置成功
		} else {
			m_sysNonblock = false; // 如果不是一个 socket 那就没必要设置非阻塞了。
		}

		return m_isInit; // 即初始化是否成功
	}

	void FdCtx::setTimeout(int type, uint64_t v) { // type指定超时类型的标志。可能的值包括 SO_RCVTIMEO 和 SO_SNDTIMEO，分别用于接收超时和发送超时。v代表设置的超时时间，单位是毫秒或者其他。
		if (type == SO_RCVTIMEO) { // 如果type类型是读事件，则超时事件设置到recvtimeout上，否则就设置到sendtimeout上。
			m_recvTimeout = v;
		} else {
			m_sendTimeout = v;
		}
	}

	uint64_t FdCtx::getTimeout(int type) { // 同理根据type类型返回对应读或写的超时时间。
		if (type == SO_RCVTIMEO) {
			return m_recvTimeout;
		} else {
			return m_sendTimeout;
		}
	}

	FdManager::FdManager()
	{
		m_datas.resize(64); // 分配空间
	}

	// 获取m_datas数组中FdCtx的对象，如果FdCtx对象不存在并且m_datas.size()<=fd的话会先扩展数组大小，然后通过Fd_Ctx的构造函数创建其对象存放到m_datas数组中。
	std::shared_ptr<FdCtx> FdManager::get(int fd, bool auto_create)
	{
		if(fd==-1) // 文件描述符无效则直接返回。
		{
			return nullptr;
		}

		std::shared_lock<std::shared_mutex> read_lock(m_mutex);
		// 如果 fd 超出了 m_datas 的范围，并且 auto_create 为 false，则返回 nullptr，表示没有创建新对象的需求。
		if(m_datas.size() <= fd)
		{
			if(auto_create==false)
			{
				return nullptr;
			}
		}
		else
		{
			if(m_datas[fd]||!auto_create)
			{
				return m_datas[fd];
			}
		}
		// 当fd的大小超出m_data.size的值也就是m_datas[fd]数组中没找到对应的fd并且auto_create为true时候会走到这里。
		read_lock.unlock();
		std::unique_lock<std::shared_mutex> write_lock(m_mutex); // 写锁

		if(m_datas.size() <= fd)
		{
			m_datas.resize(fd*1.5); // 扩容
		}

		m_datas[fd] = std::make_shared<FdCtx>(fd);
		return m_datas[fd];
	}

	// 删除指定文件描述的FdCtx对象
	void FdManager::del(int fd)
	{
		std::unique_lock<std::shared_mutex> write_lock(m_mutex);
		if(m_datas.size() <= fd)
		{
			return;
		}
		m_datas[fd].reset(); // 智能指针share调用reset()减少其对对象的引用计数，当引用计数为0销毁对象。
	}

}