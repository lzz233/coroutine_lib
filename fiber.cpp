#include "fiber.h"

static bool debug = false;

namespace sylar {

	// 当前线程上的协程控制信息

	// 正在运行的协程
	static thread_local Fiber* t_fiber = nullptr;
	// 主协程
	static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
	// 调度协程
	static thread_local Fiber* t_scheduler_fiber = nullptr;

	// 全局协程 ID 计数器
	static std::atomic<uint64_t> s_fiber_id{0};
	// s_fiber_count: 活跃协程数量计数器。
	static std::atomic<uint64_t> s_fiber_count{0}; // 活跃协程数量

	void Fiber::SetThis(Fiber *f) // 设置当前运行的协程
	{
		t_fiber = f;
	}

	// 首先运行该函数创建主协程
	/// @brief 获取当前线程正在执行的协程实例（若不存在则创建主协程）。
	/// 主协程作为线程的默认执行载体，负责协程调度器的初始化和资源管理
	std::shared_ptr<Fiber> Fiber::GetThis()
	{
		if(t_fiber) // 检查当前线程是否已存在激活的协程
		{
			// 如果有，返回当前协程的智能指针
			// shared_from_this()返回一个指向当前对象的 std::shared_ptr
			return t_fiber->shared_from_this();
		}

		// 创建主协程（首次调用时初始化）
		// 使用 shared_ptr 管理生命周期，确保与线程生命周期解耦
		std::shared_ptr<Fiber> main_fiber(new Fiber());

		// 设置线程局部存储（TLS）的协程指针
		t_thread_fiber = main_fiber; // 线程主协程的智能指针
		t_scheduler_fiber = main_fiber.get(); // 调度器使用的原始指针（默认主协程即调度协程）

		// 验证 TLS 指针一致性（防御性编程）
		// 用于判断，t_fiber是否等于main_fiber。是则继续执行，否则程序终止。
		assert(t_fiber == main_fiber.get());
		// 返回主协程的智能指针
		return t_fiber->shared_from_this();
	}

	void Fiber::SetSchedulerFiber(Fiber* f) // 设置当前的调度协程
	{
		t_scheduler_fiber = f;
	}

	uint64_t Fiber::GetFiberId() // 获取当前运行的协程的ID。
	{
		if(t_fiber)
		{
			return t_fiber->getId();
		}
		return (uint64_t)-1; // 返回-1，并且是(Uint64_t)-1那就会转换成UINT64_max，所以用来表示错误的情况
	}

	// 作用：在getThis中被调用到的时候创建主协程。设置状态，初始化上下文，并分配ID;
	Fiber::Fiber()
	{
		SetThis(this); // 在getThis中使用了无参的Fiber来构造t_fiber
		m_state = RUNNING; // 设置协程的状态为可运行

		if(getcontext(&m_ctx))
		{
			std::cerr << "Fiber() failed\n";
			pthread_exit(NULL);
		}

		m_id = s_fiber_id++; // 分配id，协程id从0开始，用完加1
		s_fiber_count ++; // 活跃的协程数量+1；
		if(debug) std::cout << "Fiber(): main id = " << m_id << std::endl;
	}

	/**
	 * @brief 协程构造函数：初始化协程上下文和运行环境
	 * @param cb 协程任务回调函数（用户定义的实际业务逻辑）
	 * @param stacksize 自定义协程栈大小（默认128KB）
	 * @param run_in_scheduler 是否在调度器上下文运行（影响切换目标）
	 *
	 * 实现原理：
	 * 1. 基于ucontext的上下文切换机制
	 * 2. 独立分配协程栈空间实现隔离运行环境
	 * 3. 状态机管理保证协程生命周期
	 */
	// 作用：创建一个新协程，初始化回调函数，栈的大小和状态。
	// 分配栈空间，并通过make修改上下文当set或swap激活ucontext_t m_ctx上下文时候会执行make第二个参数的函数。
	Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler):
	m_cb(cb), m_runInScheduler(run_in_scheduler)
	{
		m_state = READY; // 初始状态设为就绪

		// 分配协程私有栈空间（默认128KB）
		m_stacksize = stacksize ? stacksize : 128000;
		m_stack = malloc(m_stacksize); // 独立内存空间保证运行隔离

		// 初始化ucontext执行上下文
		if(getcontext(&m_ctx))
		{
			std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
			pthread_exit(NULL);
		}

		// 配置上下文参数
		m_ctx.uc_link = nullptr; // 这里因为没有设置了后继所以在运行完mainfunc后协程退出，会调用一次yield返回主协程。
		m_ctx.uc_stack.ss_sp = m_stack; // 绑定私有栈空间
		m_ctx.uc_stack.ss_size = m_stacksize;

		// 绑定入口函数（MainFunc）到当前上下文
		makecontext(&m_ctx, &Fiber::MainFunc, 0);

		// 协程ID和计数管理
		m_id = s_fiber_id++;
		s_fiber_count ++;
		if(debug) std::cout << "Fiber(): child id = " << m_id << std::endl;
	}

	/**
	 * @brief 协程析构函数：资源回收
	 *
	 * 安全措施：
	 * 1. 仅子协程需要释放栈空间（主协程使用线程栈）
	 * 2. 计数器维护防止内存泄漏
	 */
	Fiber::~Fiber()
	{
		s_fiber_count --;
		if(m_stack) // 判断是否有独立栈，有的肯定是子协程
		{
			free(m_stack); // 释放私有栈空间
		}
		// 在协程的析构函数中，this 指向的是正在被销毁的协程对象。当然这里没写this
		if(debug) std::cout << "~Fiber(): id = " << m_id << std::endl;
	}

	/**
	 * @brief 重置协程任务（复用已终止的协程实例）
	 * @param cb 新的任务回调函数
	 *
	 * 特性：
	 * - 复用已有栈空间避免重复分配
	 * - 重置协程的回调函数，并重新设置上下文，使用与将协程从`TERM`状态重置READY
	 */
	void Fiber::reset(std::function<void()> cb)
	{
		assert(m_stack != nullptr&&m_state == TERM);

		m_state = READY;
		m_cb = cb; // 替换任务回调

		// 重新初始化上下文
		if(getcontext(&m_ctx))
		{
			std::cerr << "reset() failed\n";
			pthread_exit(NULL);
		}

		m_ctx.uc_link = nullptr;
		m_ctx.uc_stack.ss_sp = m_stack;
		m_ctx.uc_stack.ss_size = m_stacksize;
		makecontext(&m_ctx, &Fiber::MainFunc, 0);
	}

	/**
	 * @brief 恢复协程执行（上下文切换核心）
	 *
	 * 切换逻辑：
	 * - 根据运行模式选择保存目标（调度器/主协程）
	 * - 使用swapcontext原子化保存当前上下文并加载目标上下文
	 * - 状态变更为RUNNING标记执行中
	 *
	 * 作用：
	 * 将协程的状态设置为running，并恢复协程的执行。如果 m_runInScheduler 为 true，则将上下文切换到调度协程；否则，切换到主线程的协程。
	 */
	void Fiber::resume()
	{
		assert(m_state==READY);

		m_state = RUNNING;

		// 这里的切换就相当于非对称协程函数那个当a执行完成后会将执行权交给b
		if(m_runInScheduler) // 调度器模式
		{
			SetThis(this); // 这里的setThis实际是就是目前工作的协程。
			if(swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx))
			{
				std::cerr << "resume() to t_scheduler_fiber failed\n";
				pthread_exit(NULL);
			}
		}
		else // 线程主协程模式
		{
			SetThis(this);
			// 保存主协程上下文，加载当前协程上下文
			if(swapcontext(&(t_thread_fiber->m_ctx), &m_ctx))
			{
				std::cerr << "resume() to t_thread_fiber failed\n";
				pthread_exit(NULL);
			}
		}
	}

	/**
	 * @brief 挂起当前协程（让出执行权）
	 *
	 * 协作式特性：
	 * - 必须显式调用yield主动让出控制权
	 * - 切换回保存的上下文（调度器/主协程）
	 */
	void Fiber::yield()
	{
		assert(m_state==RUNNING || m_state==TERM);

		if(m_state!=TERM)
		{
			m_state = READY; // 非终止状态重置为就绪
		}

		if(m_runInScheduler) // 返回调度器上下文
		{
			SetThis(t_scheduler_fiber);
			if(swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx)))
			{
				std::cerr << "yield() to to t_scheduler_fiber failed\n";
				pthread_exit(NULL);
			}
		}
		else // 返回线程主协程
		{
			SetThis(t_thread_fiber.get());
			if(swapcontext(&m_ctx, &(t_thread_fiber->m_ctx)))
			{
				std::cerr << "yield() to t_thread_fiber failed\n";
				pthread_exit(NULL);
			}
		}
	}

	/**
	 * @brief 协程入口函数（所有用户任务的统一入口）
	 *
	 * 生命周期管理：
	 * 1. 通过智能指针保持协程对象存活
	 * 2. 任务完成后自动触发yield交还控制权
	 * 3. 清理回调函数防止重复执行
	 */
	void Fiber::MainFunc()
	{
		// 通过智能指针维持生命周期
		std::shared_ptr<Fiber> curr = GetThis(); // GetThis()的shared_from_this()方法让引用计数加1
		assert(curr!=nullptr);

		curr->m_cb(); // 执行用户任务
		curr->m_cb = nullptr; // 清理回调引用
		curr->m_state = TERM; // 标记为终止状态

		// 运行完毕 -> yield让出执行权
		auto raw_ptr = curr.get(); // 获取原始指针，不增加引用计数
		curr.reset(); // 减少引用计数（可能触发析构）
		raw_ptr->yield(); // 确保控制权交还
	}

}