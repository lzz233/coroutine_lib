#include "scheduler.h"

static bool debug = false;

namespace sylar {

	// 线程局部存储当前调度器实例（每个线程独立）
	static thread_local Scheduler* t_scheduler = nullptr;

	// 获取当前线程的调度器实例（实现线程本地存储）
	Scheduler* Scheduler::GetThis()
	{
		return t_scheduler;
	}

	// 获取当前线程的调度器实例（实现线程本地存储）
	void Scheduler::SetThis()
	{
		t_scheduler = this;
	}

	/**
	 * @brief 调度器构造函数
	 * @param threads 工作线程总数（当use_caller=true时，包含主线程）
	 * @param use_caller 是否将主线程作为工作线程
	 * @param name 调度器名称（用于调试）
	 *
	 * 核心逻辑：
	 * 1. 当use_caller=true时，主线程参与调度并创建调度协程
	 * 2. 初始化线程ID集合
	 */
	// 设置当前线程的调度器实例
	Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name):
	m_useCaller(use_caller), m_name(name)
	{
		// 首先判断线程的数量是否大于0，并且调度器的对象是否是空指针，是就调用setThis()进行设置.
		assert(threads>0 && Scheduler::GetThis()==nullptr);
		SetThis();

		Thread::SetName(m_name); // 设置当前线程的名称为调度器的名称 m_name。

		// 使用主线程当作工作线程，创建协程的主要原因是为了实现更高效的任务调度和管理
		if(use_caller) // 如果user_caller为true，表示当前线程也要作为一个工作线程使用。
		{
			threads --; // 主线程占用一个工作线程名额
			Fiber::GetThis(); // 创建主协程

			// 创建调度协程（绑定run函数）
			m_schedulerFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false)); // false -> 该调度协程退出后将返回主协程
			Fiber::SetSchedulerFiber(m_schedulerFiber.get()); // 设置协程的调度器对象

			m_rootThread = Thread::GetThreadId(); // 获取主线程ID
			m_threadIds.push_back(m_rootThread); // 加入线程ID集合
		}

		m_threadCount = threads; // 将剩余的线程数量（即总线程数量减去是否使用调用者线程）赋值给 m_threadCount。
		if(debug) std::cout << "Scheduler::Scheduler() success\n";
	}

	// 调度器析构函数（确保停止状态且资源释放）
	Scheduler::~Scheduler()
	{
		assert(stopping()==true); // 判断调度器是否终止
		if (GetThis() == this) // 获取调度器的对象
		{
	        t_scheduler = nullptr; // 将其设置为nullptr防止悬空指针
	    }
	    if(debug) std::cout << "Scheduler::~Scheduler() success\n";
	}

	/**
	 * @brief 启动调度器
	 *
	 * 功能：
	 * 1. 创建指定数量的工作线程
	 * 2. 每个线程绑定run方法作为入口函数
	 */
	void Scheduler::start()
	{
		std::lock_guard<std::mutex> lock(m_mutex); // 互斥锁防止共享资源的竞争
		if(m_stopping) // 如果调度器退出直接报错打印cerr后面的话
		{
			std::cerr << "Scheduler is stopped" << std::endl;
			return;
		}

		assert(m_threads.empty()); // 确保线程池未初始化
		m_threads.resize(m_threadCount); // 将其线程池里的线程数量多少重置成和
		for(size_t i=0;i<m_threadCount;i++)
		{
			// 创建工作线程
			m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
			m_threadIds.push_back(m_threads[i]->getId()); // 记录线程ID
		}
		if(debug) std::cout << "Scheduler::start() success\n";
	}

	/**
	 * @brief 调度器主循环
	 *
	 * 执行流程：
	 * 1. 从任务队列获取可执行任务（带线程绑定检查）
	 * 2. 执行协程任务或回调函数
	 * 3. 空闲时运行idle协程
	 */

	// 作用：调度器的核心，负责从任务队列中取出任务并通过协程执行
	void Scheduler::run()
	{
		int thread_id = Thread::GetThreadId(); // 获取当前线程的ID
		if(debug) std::cout << "Schedule::run() starts in thread: " << thread_id << std::endl;

		//set_hook_enable(true);

		SetThis(); // 设置调度器对象

		// 运行在新创建的线程 -> 需要创建主协程
		if(thread_id != m_rootThread) // 如果不是主线程，创建主协程
		{
			Fiber::GetThis(); // 分配了线程的主协程和调度协程
		}

		std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));
		ScheduleTask task;

		while(true)
		{
			task.reset();
			bool tickle_me = false; // 是否唤醒了其他线程进行任务调度

			{
				std::lock_guard<std::mutex> lock(m_mutex);
				auto it = m_tasks.begin();
				// 1 遍历任务队列
				while(it!=m_tasks.end())
				{
					if(it->thread!=-1 && it->thread!=thread_id)
					{
						it++;
						tickle_me = true;
						continue;
					}

					// 2 取出任务并更新活跃线程数
					assert(it->fiber||it->cb);
					task = *it;
					m_tasks.erase(it);
					m_activeThreadCount++;
					break; // 这里取到任务的线程就直接break所以并没有遍历到队尾
				}
				tickle_me = tickle_me || (it != m_tasks.end()); // 确保仍然存在未处理的任务
			}

			if(tickle_me) // 这里虽然写了唤醒但并没有具体的逻辑代码，具体的在io+scheduler
			{
				tickle();
			}

			// 3 执行协程任务
			if(task.fiber) // 执行协程任务
			{ // resume协程，resume返回时此时任务要么执行完了，要么半路yield了，总之任务完成了，活跃线程-1；
				{
					std::lock_guard<std::mutex> lock(task.fiber->m_mutex);
					if(task.fiber->getState()!=Fiber::TERM)
					{
						task.fiber->resume();
					}
				}
				m_activeThreadCount--; // 线程完成任务后就不再处于活跃状态，而是进入空闲状态，因此需要将活跃线程计数减一。
				task.reset();
			}
			else if(task.cb) // 执行回调函数（封装为临时协程）
			{ // 上面解释过对于函数也应该被调度，具体做法就封装成协程加入调度。
				std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task.cb);
				{
					std::lock_guard<std::mutex> lock(cb_fiber->m_mutex);
					cb_fiber->resume();
				}
				m_activeThreadCount--;
				task.reset();
			}
			// 4 无任务 -> 执行空闲协程
			else
			{
				// 系统关闭 -> idle协程将从死循环跳出并结束 -> 此时的idle协程状态为TERM -> 再次进入将跳出循环并退出run()
	            if (idle_fiber->getState() == Fiber::TERM)
	            {
	            	// 如果调度器没有调度任务，那么idle协程会不断的resume/yield,不会结束进入一个忙等待，如果idle协程结束了
	            	// 一定是调度器停止了，直到有任务才执行上面的if/else，在这里idle_fiber就是不断的和主协程进行交互的子协程
            		if(debug) std::cout << "Schedule::run() ends in thread: " << thread_id << std::endl;
	                break;
	            }
				m_idleThreadCount++;
				idle_fiber->resume(); // 执行idle协程
				m_idleThreadCount--;
			}
		}

	}

	/**
	 * @brief 停止调度器
	 *
	 * 执行流程：
	 * 1. 设置停止标志位
	 * 2. 唤醒所有工作线程
	 * 3. 等待线程池结束
	 */
	void Scheduler::stop()
	{
		if(debug) std::cout << "Schedule::stop() starts in thread: " << Thread::GetThreadId() << std::endl;

		if(stopping())
		{
			return;
		}

		m_stopping = true;

	    if (m_useCaller)
	    {
	        assert(GetThis() == this);
	    }
	    else
	    {
	        assert(GetThis() != this);
	    }

		// 唤醒所有线程
		for (size_t i = 0; i < m_threadCount; i++)
		{
			tickle();
		}

		if (m_schedulerFiber)
		{
			tickle();
		}

		// 恢复调度协程以触发终止
		if(m_schedulerFiber)
		{
			m_schedulerFiber->resume();
			if(debug) std::cout << "m_schedulerFiber ends in thread:" << Thread::GetThreadId() << std::endl;
		}

		// 等待所有工作线程结束
		std::vector<std::shared_ptr<Thread>> thrs;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			thrs.swap(m_threads);
		}

		for(auto &i : thrs)
		{
			i->join();
		}
		if(debug) std::cout << "Schedule::stop() ends in thread:" << Thread::GetThreadId() << std::endl;
	}

	void Scheduler::tickle()
	{
	}

	// 空闲协程函数
	void Scheduler::idle()
	{
		while(!stopping())
		{
			if(debug) std::cout << "Scheduler::idle(), sleeping in thread: " << Thread::GetThreadId() << std::endl;
			sleep(1); // 降低CPU占用
			Fiber::GetThis()->yield(); // 主动让出执行权
		}
	}

	// 使用互斥锁的目的因为m_tasks，m_activeThreadCount会被多线程竞争所以需要互斥锁来保护资源的访问，
	// 此时这个函数的目的就是为了判断调度器是否退出。在stop函数中如果stopping为true代表调度器已经退出直接返回return 不进行任何的操作。
	bool Scheduler::stopping()
	{
	    std::lock_guard<std::mutex> lock(m_mutex);
	    return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
	}


}