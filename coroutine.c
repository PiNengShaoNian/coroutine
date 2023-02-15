#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#if __APPLE__ && __MATCH__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#define STACK_SIZE (1024 * 1024)
#define DEFAULT_COROUTINE 16

struct coroutine;

/**
 * 协程调度器
 */
struct schedule
{
  char stack[STACK_SIZE]; // 所有协程的共享栈

  ucontext_t main;       // 主协程的上下文
  int nco;               // 当前存活的线程个数
  int cap;               //  协程管理器的当前最大容量，即可以同时支持多少个协程。如果不够了，则进行扩容
  int running;           // 正在运行的协程id
  struct coroutine **co; // 存放协程的数组
};

/**
 * 协程
 */
struct coroutine
{
  coroutine_func func;  // 协程要执行的函数
  void *ud;             // 协程参数
  ucontext_t ctx;       // 协程上下文
  struct schedule *sch; // 该协程所属的调度器
  ptrdiff_t cap;        // 用于该协程切出后，已经分配的可用于保存运行时栈内存大小
  ptrdiff_t size;       // 该协程当前保存的运行时栈的大小
  int status;           // 协程当前的状态
  char *stack;          // 当前协程的保存起来的运行时栈
};

/**
 * 新建一个协程
 * @param S 协程调度器
 * @param func 协程要执行的函数
 * @param ud 协程参数
 * @return 新创建的协程
 */
struct coroutine *_co_new(struct schedule *S, coroutine_func func, void *ud)
{
  struct coroutine *co = malloc(sizeof(*co));
  co->func = func;
  co->ud = ud;
  co->sch = S;
  co->cap = 0;
  co->size = 0;
  co->status = COROUTINE_READY; // 默认的最初状态都是COROUTINE_READY
  co->stack = NULL;
  return co;
}

/**
 * 删除一个协程
 *
 * @param co 要删除的协程
 */
void _co_delete(struct coroutine *co)
{
  free(co->stack);
  free(co);
}

/**
 * 创建一个协程调度器
 */
struct schedule *coroutine_open(void)
{
  struct schedule *S = malloc(sizeof(*S));
  S->nco = 0;
  S->cap = DEFAULT_COROUTINE;
  S->running = -1;
  S->co = malloc(sizeof(struct coroutine *) * S->cap);
  memset(S->co, 0, sizeof(struct coroutine *) * S->cap);
  return S;
}

/**
 * 关闭一个协程调度器，同时清理掉它的协程
 *
 * @param S 要关闭的调度器
 */
void coroutine_close(struct schedule *S)
{
  int i;
  for (i = 0; i < S->cap; i++)
  {
    struct coroutine *co = S->co[i];

    if (co)
    {
      _co_delete(co);
    }
  }

  free(S->co);
  S->co = NULL;
  free(S);
}

/**
 * 创建一个协程，并将它放入协程调度器
 *
 * @param S 调度器
 * @param func 协程要执行的函数
 * @param ud 协程的参数
 * @return 新创建协程的ID
 */
int coroutine_new(struct schedule *S, coroutine_func func, void *ud)
{
  struct coroutine *co = _co_new(S, func, ud);
  if (S->nco >= S->cap)
  {
    // 如果目前协程的数量已经大于调度器的容量，那么进行扩容
    int id = S->cap; // 新的协程id直接为当前的容量大小
    // 容量扩大为原来的两倍
    S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *));
    // 初始化内存
    memset(S->co + S->cap, 0, sizeof(struct coroutine *) * S->cap);
    // 将协程放入调度器
    S->co[S->cap] = co;
    // 更新容量
    S->cap *= 2;
    // 更新尚未结束的协程个数
    ++S->nco;
    return id;
  }
  else
  {
    // 如果目前协程的数量小于调度器的容量，则取一个为NULL的位置，放入新的协程
    int i;
    for (i = 0; i < S->cap; i++)
    {
      /*
       * 为什么不 i%S->cap,而是要从nco+i开始呢
       * 这其实也算是一种优化策略吧，因为前nco有很大概率都非NULL的，直接跳过去更好
       */
      int id = (i + S->nco) % S->cap;
      if (S->co[id] == NULL)
      {
        S->co[id] = co;
        ++S->nco;
        return id;
      }
    }
  }

  assert(0);
  return -1;
}

/**
 * 这个函数是协程绑定函数的包装函数，在其中我们除了要执行和协程绑定的函数外还会在协程执行完毕后删除该协程
 * 通过low32和hi32 拼出了struct schedule的指针，这里为什么要用这种方式，而不是直接传struct schedule*呢？
 * 因为makecontext的函数指针的参数是int可变列表，在64位下，一个int没法承载一个指针
 * 
 * @param low32 S指针的低32位
 * @param hi32 S指针的高32位
 */
static void mainfunc(uint32_t low32, uint32_t hi32)
{
  uintptr_t ptr = (uint32_t)low32 | ((uintptr_t)hi32 << 32);
  struct schedule *S = (struct schedule *)ptr;

  int id = S->running;
  struct coroutine *C = S->co[id];
  C->func(S, C->ud); // 中间有可能会有不断的yield
  _co_delete(C);
  S->co[id] = NULL;
  --S->nco;
  S->running = -1;
}

/**
 * 切换到指定ID的协程开始执行
 *
 * @param S 协程调度器
 * @param id 要被执行的协程
 */
void coroutine_resume(struct schedule *S, int id)
{
  assert(S->running == -1);
  assert(id >= 0 && id < S->cap);

  // 取出协程
  struct coroutine *C = S->co[id];
  if (C == NULL)
    return;

  int status = C->status;
  switch (status)
  {
  case COROUTINE_READY:
    // 初始化ucontext_t结构体, 将当前的上下文放到C->ctx里面
    getcontext(&C->ctx);
    // 将当前协程的运行时栈设置为S->stack
    // 所有的协程共享该栈
    C->ctx.uc_stack.ss_sp = S->stack;
    C->ctx.uc_stack.ss_size = STACK_SIZE;
    C->ctx.uc_link = &S->main; // 如果协程执行完，将切换到主协程中执行
    S->running = id;
    C->status = COROUTINE_RUNNING;

    // 新建协程C的上下文，并将S作为参数传进去
    uintptr_t ptr = (uintptr_t)S;
    // 在这里传入的mainfunc是C->func的包装函数，而不是C->func本身，方便函数执行完毕后做一些清理工作
    makecontext(&C->ctx, (void (*)(void))mainfunc, 2, (uint32_t)ptr, (uint32_t)(ptr >> 32));

    // 将当前的上下文保存到S->main中，并将C->ctx替换到当前上下文
    swapcontext(&S->main, &C->ctx);
    break;
  case COROUTINE_SUSPEND:
    // 将协程保存的栈内容恢复到共享栈内存中
    // 其中C->size在yield时有保存
    // S->stack + STACK_SIZE为栈的最高地址，从这里开始栈向下延展
    // C->stack + C->size是栈底的位置, S->stack + STACK_SIZE是共享栈的栈底位置
    // 所以将C的栈复制到共享栈后它的栈顶位置在共享栈中的位置就应该是 S->stack + STACK_SIZE - C->size
    memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size);
    S->running = id;
    C->status = COROUTINE_RUNNING;
    swapcontext(&S->main, &C->ctx);
    break;
  default:
    assert(0);
  }
}

/**
 * 赋值当前运行时栈到协程的C->stack中
 * @param C 要保存的协程
 * @param top 当前运行时栈的最高地址(也就是栈底)
 */
static void _save_stack(struct coroutine *C, char *top)
{
  /**
   * 考虑到栈是从高地址向低地址拓展，并且此时dummy位置栈顶，
   * 所以内存布局应该值这样的
   * S->stack + STACK_SIZE  ----------------      栈底
   *                        |              |       |
   *                        ----------------       |
   *                                 .             |
   *                                 .             |
   *                                 .             |
   * dummy                  -----------------      ⬇
   *                        |               |      栈顶
   *                        -----------------
   */
  char dummy = 0;
  assert(top - &dummy <= STACK_SIZE);
  // 在这里top - &dummy就是当前协程的运行栈大小
  if (C->cap < top - &dummy)
  {
    // 不够存储当前运行栈，重新分配更大的内存
    free(C->stack);
    C->cap = top - &dummy;
    C->stack = malloc(C->cap);
  }
  // 更新协程栈大小
  C->size = top - &dummy;
  // 将运行时栈复制到C->stack中
  memcpy(C->stack, &dummy, C->size);
}

/**
 * 将当前正在运行的协程让出，切换到主协程上
 * @param S 协程调度器
 */
void coroutine_yield(struct schedule *S)
{
  // 取出当前正在执行的协程
  int id = S->running;
  assert(id >= 0);

  struct coroutine *C = S->co[id];

  // 切出前得把当且协程的栈给保存好
  _save_stack(C, S->stack + STACK_SIZE);

  // 更新当前协程的状态
  C->status = COROUTINE_SUSPEND;
  S->running = -1;

  // 切出到主协程, 所以这里可以看到，只能从协程切换到主协程中
  swapcontext(&C->ctx, &S->main);
}

/**
 * 获取协程的状态
 *
 * @param S 协程调度器
 * @param id 协程ID
 * @return 协程的状态
 */
int coroutine_status(struct schedule *S, int id)
{
  assert(id >= 0 && id < S->cap);
  if (S->co[id] == NULL)
    return COROUTINE_DEAD;
  return S->co[id]->status;
}

/**
 * 获取正在运行的协程的ID
 *
 * @param S 协程调度器
 * @return 协程ID
 */
int coroutine_running(struct schedule *S)
{
  return S->running;
}