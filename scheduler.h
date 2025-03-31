#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

//#include "hook.h"
#include "fiber.h"
#include "thread.h"

#include <mutex>
#include <vector>

namespace sylar {

	/**
	 * @brief 协程调度器，实现多线程协同的任务调度
	 *
	 * 特性：
	 * - 支持混合调度协程(Fiber)和普通函数(Callback)
	 * - 线程池管理，支持任务指定运行线程
	 * - 主线程可选择作为工作线程参与调度
	 */
	class Scheduler
	{
	public:
		/**
		 * @param threads 工作线程数量（不含主线程）
		 * @param use_caller 是否将主线程作为工作线程
		 * @param name 调度器名称（用于调试）
		 */
		Scheduler(size_t threads = 1, bool use_caller = true, const std::string& name="Scheduler");
		virtual ~Scheduler(); // virtual防止出现资源泄露，基类指针删除派生类对象时不完全销毁的问题。

		const std::string& getName() const {return m_name;} // 获取调度器的名字

	public:
		// 获取正在运行的调度器
		static Scheduler* GetThis();

	protected:
		// 设置正在运行的调度器
		void SetThis();

	public:
		/**
		 * @brief 添加任务到队列（线程安全）
		 * @tparam FiberOrCb 支持协程指针或函数对象
		 * @param thread 指定运行线程ID（-1表示任意）
		 *
		 * 设计特点：
		 * - 通过互斥锁保证任务队列线程安全
		 * - 当队列从空变为非空时触发tickle通知
		 */
		// 添加任务到任务队列
		// FiberOrCb 调度任务类型，可以是协程对象或函数指针
	    template <class FiberOrCb> // 这个不需要想那么复杂看成T也行
	    void scheduleLock(FiberOrCb fc, int thread = -1)
	    {
    		bool need_tickle; // 用于标记任务队列是否为空，从而判断是否需要唤醒线程。
    		{
    			std::lock_guard<std::mutex> lock(m_mutex);
    			// empty ->  all thread is idle -> need to be waken up
    			need_tickle = m_tasks.empty();

    			//创建Task的任务对象
		        ScheduleTask task(fc, thread);
		        if (task.fiber || task.cb) // 存在就加入
		        {
		            m_tasks.push_back(task);
		        }
    		}

    		if(need_tickle) // 如果检查出了队列为空，就唤醒线程
    		{
    			tickle();
    		}
	    }

		// 启动线程池，启动调度器
		virtual void start();
		// 关闭线程池，停止调度器，等所有调度任务都执行完后再返回。
		virtual void stop();

	protected:
		// 唤醒线程
		virtual void tickle();

		/**
		 * @brief 工作线程主循环
		 *
		 * 核心逻辑：
		 * 1. 从任务队列获取任务
		 * 2. 执行协程resume()或回调函数
		 * 3. 空闲时执行idle()策略
		 */
		// 线程函数
		virtual void run();

		// 空闲协程函数，无任务调度时执行idle协程。
		virtual void idle();

		// 是否可以关闭
		virtual bool stopping();

		// 返回是否有空闲线程
		// 当调度协程进入idle时空闲线程数+1，从idle协程返回时空闲 线程数减1；
		bool hasIdleThreads() {return m_idleThreadCount>0;}

	private:

		/**
		 * @brief 调度任务封装结构体
		 *
		 * 支持两种任务类型：
		 * - fiber: 协程指针（用于协程调度）
		 * - cb:    函数对象（用于普通异步任务）
		 * thread字段指定目标线程（-1表示不限制）
		 */
		// 任务
		struct ScheduleTask
		{
			std::shared_ptr<Fiber> fiber; // 协程智能指针（自动管理生命周期）
			std::function<void()> cb; // 函数回调
			int thread; // 目标线程ID

			ScheduleTask()
			{
				fiber = nullptr;
				cb = nullptr;
				thread = -1;
			}

			ScheduleTask(std::shared_ptr<Fiber> f, int thr)
			{
				fiber = f;
				thread = thr;
			}

			ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
			{
				fiber.swap(*f); // 将内容转移也就是指针内部的转移。和上面的赋值不同，引用计数不会增加
				thread = thr;
			}

			ScheduleTask(std::function<void()> f, int thr)
			{
				cb = f;
				thread = thr;
			}

			ScheduleTask(std::function<void()>* f, int thr)
			{
				cb.swap(*f); // 同理
				thread = thr;
			}

			void reset() // 重置
			{
				fiber = nullptr;
				cb = nullptr;
				thread = -1;
			}
		};

	private:
		std::string m_name; // 调度器的名称
		// 互斥锁 -> 保护任务队列
		std::mutex m_mutex;
		// 线程池，存初始化好的线程
		std::vector<std::shared_ptr<Thread>> m_threads;
		// 任务队列
		std::vector<ScheduleTask> m_tasks;
		// 存储工作线程的线程id
		std::vector<int> m_threadIds;
		// 需要额外创建的线程数
		size_t m_threadCount = 0;

		// 原子计数器（无锁并发）
		// 活跃线程数
		std::atomic<size_t> m_activeThreadCount = {0};
		// 赋值好可以省略，m_activeThreadCount{0},c++11后叫做统一初始化列表

		// 空闲线程数
		std::atomic<size_t> m_idleThreadCount = {0};

		// 主线程相关配置
		// 主线程是否用作工作线程
		bool m_useCaller;
		// 如果是 -> 需要额外创建调度协程
		std::shared_ptr<Fiber> m_schedulerFiber;
		// 如果是 -> 记录主线程的线程id
		int m_rootThread = -1;
		// 是否正在关闭
		bool m_stopping = false;
	};

}

#endif