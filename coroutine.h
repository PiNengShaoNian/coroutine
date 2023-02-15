#ifndef C_COROUTINE_H
#define C_COROUTINE_H

// 一个协程执行完毕后的状态
#define COROUTINE_DEAD 0
// 新创建的协程会处于READY
#define COROUTINE_READY 1
// 一个正在执行中协程的状态
#define COROUTINE_RUNNING 2
// 执行到一半主动yield后的协程处于的状态
#define COROUTINE_SUSPEND 3

// 协程调度器
// 为了ABI兼容，这里故意没有具体实现，因为不同编译器和平台结构体的内存布局可能不同
// 故意不实现可以避免用户对结构体中的内容access造成潜在的错误，当然如果直接从同一个
// 编译器和平台编译不会有这样的问题.
struct schedule;

// 一个要被协程执行的函数，作为创建协程时的参数传入
typedef void (*coroutine_func)(struct schedule *, void *ud);

// 创建一个协程调度器
struct schedule *coroutine_open(void);

// 关闭一个协程调度器
void coroutine_close(struct schedule *);

// 创建一个协程
int coroutine_new(struct schedule *, coroutine_func, void *ud);

// 切换到对应协程中运行
void coroutine_resume(struct schedule *, int id);

// 返回协程状态
int coroutine_status(struct schedule *, int id);

// 协程是否正在运行
int coroutine_running(struct schedule *);

// 切出协程
void coroutine_yield(struct schedule *);

#endif