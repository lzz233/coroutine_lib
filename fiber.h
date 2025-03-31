#ifndef _COROUTINE_H_
#define _COROUTINE_H_

#include <iostream>     
#include <memory>       
#include <atomic>       
#include <functional>   
#include <cassert>      
#include <ucontext.h>   
#include <unistd.h>
#include <mutex>

namespace sylar {

class Fiber : public std::enable_shared_from_this<Fiber>
{
public:
	// 协程状态
	enum State // 定义协程的状态，属于协程的上下文切换的时候，需要被保存
	{
		READY, 
		RUNNING, 
		TERM 
	};

private:
	// 仅由GetThis()调用 -> 私有 -> 创建主协程  
	Fiber(); // 细节1 Fiber()是私有的，只能被GetThis()方法调用，用于创建主协程。

public:
	Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true);
	// 带参的构造函数用于构造子协程，初始化子协程的ucontext_t上下文和栈空间，要求传入协程的入口函数，以及可选协程栈大小
	// 重载构造函数。用于创建指定回调函数、栈大小和 run_in_scheduler 本协程是否参与调度器调度，默认为true
	~Fiber();

	// 重用一个协程
	void reset(std::function<void()> cb); // 重置协程状态和入口函数，复用栈空间，不重新创建栈

	// 任务线程恢复执行
	void resume();
	// 任务线程让出执行权
	void yield();

	uint64_t getId() const {return m_id;} // 获取唯一标识
	State getState() const {return m_state;} // 获取协程状态

public:
	// 设置当前运行的协程
	static void SetThis(Fiber *f);

	// 得到当前运行的协程 
	static std::shared_ptr<Fiber> GetThis();

	// 设置调度协程（默认为主协程）
	static void SetSchedulerFiber(Fiber* f);
	
	// 得到当前运行的协程id
	static uint64_t GetFiberId();

	// 协程的主函数，入口点
	static void MainFunc();	

private:
	// id，协程唯一标识符
	uint64_t m_id = 0;
	// 栈大小
	uint32_t m_stacksize = 0;
	// 协程的初始状态是ready
	State m_state = READY;
	// 协程上下文
	ucontext_t m_ctx; // ucontext_t 是一个用于保存用户协程执行上下文的结构体，通常用于实现用户级线程或协程。
	// 协程栈指针
	void* m_stack = nullptr;
	// 协程的回调函数
	std::function<void()> m_cb;
	// 是否让出执行权交给调度协程
	bool m_runInScheduler;

public:
	std::mutex m_mutex;
};

}

#endif

