/* ********************************
 * Author:       Johan Hanssen Seferidis
 * License:	     MIT
 * Description:  Library providing a threading pool where you can add
 *               work. For usage, check the thpool.h file or README.md
 *
 *//** @file thpool.h *//*
 *
 ********************************/

#if defined(__APPLE__)
#include <AvailabilityMacros.h>
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif
#endif
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#endif

#include "thpool.h"

#ifdef THPOOL_DEBUG
#define THPOOL_DEBUG 1
#else
#define THPOOL_DEBUG 0
#endif

#if !defined(DISABLE_PRINT) || defined(THPOOL_DEBUG)
#define err(str) fprintf(stderr, str)
#else
#define err(str)
#endif

#ifndef THPOOL_THREAD_NAME
#define THPOOL_THREAD_NAME thpool
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

static volatile int threads_keepalive;
static volatile int threads_on_hold;



/* ========================== STRUCTURES ============================ */


/* Binary semaphore */
typedef struct bsem {
	pthread_mutex_t mutex;
	pthread_cond_t   cond;
	int v;
} bsem;


/* Job */
typedef struct job{
	struct job*  prev;                   /* pointer to previous job   */
	void   (*function)(void* arg);       /* function pointer          */
	void*  arg;                          /* function's argument       */
} job;


/* Job queue */
typedef struct jobqueue{
	pthread_mutex_t rwmutex;             /* used for queue r/w access */
	job  *front;                         /* pointer to front of queue */
	job  *rear;                          /* pointer to rear  of queue */
	bsem *has_jobs;                      /* flag as binary semaphore  */
	int   len;                           /* number of jobs in queue   */
} jobqueue;


/* Thread */
typedef struct thread{
	int       id;                        /* friendly id               */
	pthread_t pthread;                   /* pointer to actual thread  */
	struct thpool_* thpool_p;            /* access to thpool          */
} thread;


/* Threadpool */
typedef struct thpool_{
	thread**   threads;                  /* pointer to threads        */
	volatile int num_threads_alive;      /* threads currently alive   */
	volatile int num_threads_working;    /* threads currently working */
	pthread_mutex_t  thcount_lock;       /* used for thread count etc */
	pthread_cond_t  threads_all_idle;    /* signal to thpool_wait     */
	jobqueue  jobqueue;                  /* job queue                 */
} thpool_;





/* ========================== PROTOTYPES ============================ */


static int  thread_init(thpool_* thpool_p, struct thread** thread_p, int id);
static void* thread_do(struct thread* thread_p);
static void  thread_hold(int sig_id);
static void  thread_destroy(struct thread* thread_p);

static int   jobqueue_init(jobqueue* jobqueue_p);
static void  jobqueue_clear(jobqueue* jobqueue_p);
static void  jobqueue_push(jobqueue* jobqueue_p, struct job* newjob_p);
static struct job* jobqueue_pull(jobqueue* jobqueue_p);
static void  jobqueue_destroy(jobqueue* jobqueue_p);

static void  bsem_init(struct bsem *bsem_p, int value);
static void  bsem_reset(struct bsem *bsem_p);
static void  bsem_post(struct bsem *bsem_p);
static void  bsem_post_all(struct bsem *bsem_p);
static void  bsem_wait(struct bsem *bsem_p);





/* ========================== THREADPOOL ============================ */


/* Initialise thread pool */
struct thpool_* thpool_init(int num_threads){
// 【全局状态机初始化】
        // keepalive 设为 1 表示线程池处于运行态。
        // 所有的工作线程在 thread_do 循环中都会持续监听这个标志位，以决定是否继续存活
	threads_on_hold   = 0;
	threads_keepalive = 1;

	if (num_threads < 0){
		num_threads = 0;
	}
        // 【主控结构体内存分配】
        // 在堆区为线程池（thpool_p）申请内存
	/* Make new thread pool */
	thpool_* thpool_p;
	thpool_p = (struct thpool_*)malloc(sizeof(struct thpool_));
	if (thpool_p == NULL){
		err("thpool_init(): Could not allocate memory for thread pool\n");
		return NULL;
	}
	thpool_p->num_threads_alive   = 0;
	thpool_p->num_threads_working = 0;
        // 【任务队列初始化】
        // 队列是生产者（主线程）和消费者（工作线程）交互的唯一缓冲区。
        // jobqueue_init 内部会初始化用于队列互斥访问的锁和用于阻塞挂起的信号
	/* Initialise the job queue */
	if (jobqueue_init(&thpool_p->jobqueue) == -1){
		err("thpool_init(): Could not allocate memory for job queue\n");
	// 【级联回滚】
                // 发生异常时必须回滚之前已分配的主控结构体内存，防止内存泄漏
		free(thpool_p);
		return NULL;
	}

	/* Make threads in pool */
	thpool_p->threads = (struct thread**)malloc(num_threads * sizeof(struct thread *));
	if (thpool_p->threads == NULL){
		err("thpool_init(): Could not allocate memory for threads\n");
		jobqueue_destroy(&thpool_p->jobqueue);
		free(thpool_p);
		return NULL;
	}
// 【同步原语初始化】
        // thcount_lock: 用于保护 num_threads_alive 和 working 这两个统计变量的互斥锁。
        // threads_all_idle: 条件变量，当所有线程都处于空闲状态时，可用来唤醒等待销毁的主线程
	pthread_mutex_init(&(thpool_p->thcount_lock), NULL);
	pthread_cond_init(&thpool_p->threads_all_idle, NULL);

	/* Thread init */
// 【内核线程派生】
        // 依次实例化每一个工作线程。thread_init 内部会真正执行 pthread_create 陷入内核态
	int n;
	for (n=0; n<num_threads; n++){
		thread_init(thpool_p, &thpool_p->threads[n], n);
#if THPOOL_DEBUG
			printf("THPOOL_DEBUG: Created thread %d in pool \n", n);
#endif
	}

	/* Wait for threads to initialize */
// 【自旋等待】
        // 极其关键的一步。主线程在此处通过死循环轻量级阻塞，直到所有的子线程
        // 都成功在自己的执行流中将 num_threads_alive 自增到 num_threads。
        // 这确保了线程池对象返回给调用者时，所有的 Worker 都已 100% 准备就绪，避免了时序竞态条件
	while (thpool_p->num_threads_alive != num_threads) {}

	return thpool_p;
}


/* Add work to the thread pool */
// 【生产者对外网关接口】
// 这是主线程（生产者）将外部 socket 连接投递给线程池的唯一入口，负责将离散的函数和参数打包成标准任务节点。
int thpool_add_work(thpool_* thpool_p, void (*function_p)(void*), void* arg_p){
	job* newjob;
        // 必须通过 malloc 在堆区动态开辟任务节点。因为此函数极快执行完毕后就会弹栈，
        // 若使用局部栈变量，Worker 线程还没来得及取任务，内存就已经失效，会导致致命的段错误。
	newjob=(struct job*)malloc(sizeof(struct job));
        // 极限高并发压测下，系统可用内存可能被瞬间耗尽（Out of Memory）。
        // 此处严格拦截内存分配失败，防止后续空指针解引用导致整个 Web 服务器闪退。
	if (newjob==NULL){
		err("thpool_add_work(): Could not allocate memory for new job\n");
		return -1;
	}

	/* add function and argument */
        // 利用 C 语言的函数指针机制，将特定业务逻辑（如 accept_request）及其参数地址
        // 无缝挂载到抽象的 job 结构体上，实现了框架层与业务层的彻底解耦。
	newjob->function=function_p;
	newjob->arg=arg_p;

	/* add job to queue */
        // 复杂的互斥锁 (Mutex) 竞争、多线程链表指针安全插入、以及对 Worker 线程的重新启用
        // 被封装在了内部的 push 函数中，极大降低了外层 API 调用的负担。
	jobqueue_push(&thpool_p->jobqueue, newjob);

	return 0;
}


/* Wait until all jobs have finished */
void thpool_wait(thpool_* thpool_p){
	pthread_mutex_lock(&thpool_p->thcount_lock);
	while (thpool_p->jobqueue.len || thpool_p->num_threads_working) {
		pthread_cond_wait(&thpool_p->threads_all_idle, &thpool_p->thcount_lock);
	}
	pthread_mutex_unlock(&thpool_p->thcount_lock);
}


/* Destroy the threadpool */
void thpool_destroy(thpool_* thpool_p){
	/* No need to destroy if it's NULL */
	if (thpool_p == NULL) return ;
// 使用 volatile 关键字修饰，强制每次从内存中读取该变量，
        // 防止编译器优化导致在多核多线程环境下读到 CPU 寄存器里的旧缓存值
	volatile int threads_total = thpool_p->num_threads_alive;

	/* End each thread 's infinite loop */
// 将全局运行标志位清零。
        // 此时，所有在 thread_do 中执行的 while循环，
        // 在下一次条件判断时都将判定为假，从而跳出死循环，准备退出
	threads_keepalive = 0;

	/* Give one second to kill idle threads */
	double TIMEOUT = 1.0;
	time_t start, end;
	double tpassed = 0.0;
	time (&start);
	while (tpassed < TIMEOUT && thpool_p->num_threads_alive){
// 持续释放信号量，强制唤醒所有阻塞的空闲线程。
                // 它们醒来后会检查到 keepalive == 0，从而主动结束运行
		bsem_post_all(thpool_p->jobqueue.has_jobs);
		time (&end);
		tpassed = difftime(end,start);
	}

	/* Poll remaining threads */
	while (thpool_p->num_threads_alive){
		bsem_post_all(thpool_p->jobqueue.has_jobs);
		sleep(1);
	}

	/* Job queue cleanup */
	jobqueue_destroy(&thpool_p->jobqueue);
// 严格按照与 init 初始化相反的顺序（自底向上）进行 free 操作
	/* Deallocs */
	int n;
	for (n=0; n < threads_total; n++){
		thread_destroy(thpool_p->threads[n]);
	}
	free(thpool_p->threads);
	free(thpool_p);
}


/* Pause all threads in threadpool */
void thpool_pause(thpool_* thpool_p) {
	int n;
	for (n=0; n < thpool_p->num_threads_alive; n++){
		pthread_kill(thpool_p->threads[n]->pthread, SIGUSR1);
	}
}


/* Resume all threads in threadpool */
void thpool_resume(thpool_* thpool_p) {
    // resuming a single threadpool hasn't been
    // implemented yet, meanwhile this suppresses
    // the warnings
    (void)thpool_p;

	threads_on_hold = 0;
}


int thpool_num_threads_working(thpool_* thpool_p){
	return thpool_p->num_threads_working;
}





/* ============================ THREAD ============================== */


/* Initialize a thread in the thread pool
 *
 * @param thread        address to the pointer of the thread to be created
 * @param id            id to be given to the thread
 * @return 0 on success, -1 otherwise.
 */
static int thread_init (thpool_* thpool_p, struct thread** thread_p, int id){
// 【元数据存储】
        // 在堆区申请一块内存，用于存放单个线程的属性
        // 这块内存是纯用户态的，用于构建我们的“面向对象”线程管理模型
	*thread_p = (struct thread*)malloc(sizeof(struct thread));
	if (*thread_p == NULL){
		err("thread_init(): Could not allocate memory for thread\n");
		return -1;
	}
// 将全局线程池指针绑定到该子线程上。
        // 这至关重要，因为子线程在执行 thread_do 时，必须通过这个指针去访问
        // 全局共享的 jobqueue以及互斥锁和信号量
	(*thread_p)->thpool_p = thpool_p;
	(*thread_p)->id       = id;

	pthread_create(&(*thread_p)->pthread, NULL, (void * (*)(void *)) thread_do, (*thread_p));
        // 默认情况下，线程是“可连接的”，其终止后内核仍会保留部分数据结构，直到其他线程调用 pthread_join。
        // 调用 pthread_detach 后，该线程与主线程“脱钩”。
        // 当它执行完毕（即跳出 thread_do 死循环）退出时，操作系统会自动且立刻回收其占用的所有内核资源。
        // 这避免了主线程必须时刻阻塞等待子线程结束的劣势，实现了真正的异步管理
	pthread_detach((*thread_p)->pthread);
	return 0;
}


/* Sets the calling thread on hold */
static void thread_hold(int sig_id) {
    (void)sig_id;
	threads_on_hold = 1;
	while (threads_on_hold){
		sleep(1);
	}
}


/* What each thread is doing
*
* In principle this is an endless loop. The only time this loop gets interrupted is once
* thpool_destroy() is invoked or the program exits.
*
* @param  thread        thread that will run this function
* @return nothing
*/
static void* thread_do(struct thread* thread_p){

	/* Set thread name for profiling and debugging */
	char thread_name[16] = {0};

	snprintf(thread_name, 16, TOSTRING(THPOOL_THREAD_NAME) "-%d", thread_p->id);

#if defined(__linux__)
	/* Use prctl instead to prevent using _GNU_SOURCE flag and implicit declaration */
// 【可观测性设计】
        // 调用内核 prctl 接口重命名子线程。
        // 这样在 Linux 系统下执行 top -H 或 htop 时，可以直接看到每个 Worker 线程的名称而非通用的进程名，
        // 极大地方便了高并发环境下的 CPU 性能调优和死锁追踪。
	prctl(PR_SET_NAME, thread_name);
#elif defined(__APPLE__) && defined(__MACH__)
	pthread_setname_np(thread_name);
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(thread_p->pthread, thread_name);
#else
	err("thread_do(): pthread_setname_np is not supported on this system");
#endif

	/* Assure all threads have been created before starting serving */
	thpool_* thpool_p = thread_p->thpool_p;

	/* Register signal handler */
	struct sigaction act;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_ONSTACK;
	act.sa_handler = thread_hold;
// 【线程运行时控制】
        // 为子线程注册 SIGUSR1 信号。这允许主线程通过信号交互手段（如 thread_hold），
        // 在不销毁线程的情况下暂时挂起某个 Worker，实现了对线程池状态的动态精细化控制。
	if (sigaction(SIGUSR1, &act, NULL) == -1) {
		err("thread_do(): cannot handle SIGUSR1");
	}

	/* Mark thread as alive (initialized) */
	pthread_mutex_lock(&thpool_p->thcount_lock);
	thpool_p->num_threads_alive += 1;
	pthread_mutex_unlock(&thpool_p->thcount_lock);
// 【消除创建开销】
        // 线程池的核心：利用死循环维持线程生命。
        // 通过这种方式，Worker 线程在处理完一个 HTTP 请求后不会退出，而是循环等待下一个任务。
        // 这规避了原生 TinyHTTPd 频繁调用 pthread_create 所产生的内核态上下文切换负载。
	while(threads_keepalive){
// 【非忙等待】
                // 任务队列为空时，线程会在此处触发信号量等待并交出 CPU 执行权。
                // 此时线程处于“休眠态”，直到生产者投递新任务并发送唤醒信号。
                // 这种机制确保了服务器在空闲时 CPU 占用率接近 0%。
		bsem_wait(thpool_p->jobqueue.has_jobs);

		if (threads_keepalive){

			pthread_mutex_lock(&thpool_p->thcount_lock);
			thpool_p->num_threads_working++;
			pthread_mutex_unlock(&thpool_p->thcount_lock);

			/* Read job from queue and execute it */
			void (*func_buff)(void*);
			void*  arg_buff;
// 【临界区保护】
                        // 唤醒后立即尝试从链表中提取任务节点。
                        // 函数内部封装了互斥锁操作，确保在多个 Worker 同时被唤醒时，
                        // 只有一个线程能抢到任务指针。
			job* job_p = jobqueue_pull(&thpool_p->jobqueue);
			if (job_p) {
				func_buff = job_p->function;
				arg_buff  = job_p->arg;
				// 【执行具体业务】
                                // 解构出回调函数地址及其参数，正式切入 Web 服务器逻辑（如 accept_request）。
                                // 此时线程池框架仅作为载体，具体的协议解析和文件传输在这一行被触发。
				func_buff(arg_buff);
			// 【释放任务载体】
                                // 任务执行完毕，立即释放堆区的 job 结构体内存。
                                // 配合 httpd.c 里的内存管理，形成了从任务创建到销毁的完整闭环。
				free(job_p);
			}

			pthread_mutex_lock(&thpool_p->thcount_lock);
			thpool_p->num_threads_working--;
			if (!thpool_p->num_threads_working) {
				pthread_cond_signal(&thpool_p->threads_all_idle);
			}
			pthread_mutex_unlock(&thpool_p->thcount_lock);

		}
	}
	pthread_mutex_lock(&thpool_p->thcount_lock);
	thpool_p->num_threads_alive --;
	pthread_mutex_unlock(&thpool_p->thcount_lock);

	return NULL;
}


/* Frees a thread  */
static void thread_destroy (thread* thread_p){
	free(thread_p);
}





/* ============================ JOB QUEUE =========================== */


/* Initialize queue */
static int jobqueue_init(jobqueue* jobqueue_p){
	jobqueue_p->len = 0;
	jobqueue_p->front = NULL;
	jobqueue_p->rear  = NULL;

	jobqueue_p->has_jobs = (struct bsem*)malloc(sizeof(struct bsem));
	if (jobqueue_p->has_jobs == NULL){
		return -1;
	}

	pthread_mutex_init(&(jobqueue_p->rwmutex), NULL);
	bsem_init(jobqueue_p->has_jobs, 0);

	return 0;
}


/* Clear the queue */
static void jobqueue_clear(jobqueue* jobqueue_p){

	while(jobqueue_p->len){
		free(jobqueue_pull(jobqueue_p));
	}

	jobqueue_p->front = NULL;
	jobqueue_p->rear  = NULL;
	bsem_reset(jobqueue_p->has_jobs);
	jobqueue_p->len = 0;

}


/* Add (allocated) job to queue
 */
static void jobqueue_push(jobqueue* jobqueue_p, struct job* newjob){
// 【互斥锁竞争】
        // 必须通过原子操作申请互斥锁（Mutex）。如果此时有 Worker 线程正在队列中取任务，
        // 主线程将在此处阻塞等待，严格防止多个线程同时修改链表指针导致内存写冲突。
	pthread_mutex_lock(&jobqueue_p->rwmutex);
	newjob->prev = NULL;
// 【双向链表边界处理】
        // 依据当前队列长度进行分支处理。线程池的任务调度本质上是一个“先进先出”模型。
        // 通过维护 front 和 rear 指针，实现了在 O(1) 时间复杂度内完成任务的快速入队和出队。
	switch(jobqueue_p->len){

		case 0:  /* if no jobs in queue */
// 针对空队列的特殊初始化：将新任务同时置为队首和队尾。
					jobqueue_p->front = newjob;
					jobqueue_p->rear  = newjob;
					break;

		default: /* if jobs in queue */
					jobqueue_p->rear->prev = newjob;
					jobqueue_p->rear = newjob;

	}
// 【计数器原子更新】
        // 在互斥锁的保护下安全地自增任务计数。这个数字是监控线程池负载、判断系统满载状态的核心指标。
	jobqueue_p->len++;
// 【条件唤醒信号】
        // 任务入队成功后，通过信号量发出唤醒指令。
        // 此时，操作系统内核会将一个原本处于休眠挂起态的 Worker 线程精准地切换回就绪态，
        // 触发其从“阻塞等待”变为“取出执行”。
	bsem_post(jobqueue_p->has_jobs);
// 【锁的归还】
        // 临界区操作完成，主动释放互斥锁。只有这一步执行完，由于竞争而被挂起的其他线程才有机会继续执行。
	pthread_mutex_unlock(&jobqueue_p->rwmutex);
}


/* Get first job from queue(removes it from queue)
 * Notice: Caller MUST hold a mutex
 */
static struct job* jobqueue_pull(jobqueue* jobqueue_p){

	pthread_mutex_lock(&jobqueue_p->rwmutex);
	job* job_p = jobqueue_p->front;

	switch(jobqueue_p->len){

		case 0:  /* if no jobs in queue */
		  			break;

		case 1:  /* if one job in queue */
					jobqueue_p->front = NULL;
					jobqueue_p->rear  = NULL;
					jobqueue_p->len = 0;
					break;

		default: /* if >1 jobs in queue */
					jobqueue_p->front = job_p->prev;
					jobqueue_p->len--;
					/* more than one job in queue -> post it */
					bsem_post(jobqueue_p->has_jobs);

	}

	pthread_mutex_unlock(&jobqueue_p->rwmutex);
	return job_p;
}


/* Free all queue resources back to the system */
static void jobqueue_destroy(jobqueue* jobqueue_p){
	jobqueue_clear(jobqueue_p);
	free(jobqueue_p->has_jobs);
}





/* ======================== SYNCHRONISATION ========================= */


/* Init semaphore to 1 or 0 */
static void bsem_init(bsem *bsem_p, int value) {
	if (value < 0 || value > 1) {
		err("bsem_init(): Binary semaphore can take only values 1 or 0");
		exit(1);
	}
	pthread_mutex_init(&(bsem_p->mutex), NULL);
	pthread_cond_init(&(bsem_p->cond), NULL);
	bsem_p->v = value;
}


/* Reset semaphore to 0 */
static void bsem_reset(bsem *bsem_p) {
	pthread_mutex_destroy(&(bsem_p->mutex));
	pthread_cond_destroy(&(bsem_p->cond));
	bsem_init(bsem_p, 0);
}


/* Post to at least one thread */
static void bsem_post(bsem *bsem_p) {
	pthread_mutex_lock(&bsem_p->mutex);
	bsem_p->v = 1;
	pthread_cond_signal(&bsem_p->cond);
	pthread_mutex_unlock(&bsem_p->mutex);
}


/* Post to all threads */
static void bsem_post_all(bsem *bsem_p) {
	pthread_mutex_lock(&bsem_p->mutex);
	bsem_p->v = 1;
	pthread_cond_broadcast(&bsem_p->cond);
	pthread_mutex_unlock(&bsem_p->mutex);
}


/* Wait on semaphore until semaphore has value 0 */
static void bsem_wait(bsem* bsem_p) {
	pthread_mutex_lock(&bsem_p->mutex);
	while (bsem_p->v != 1) {
		pthread_cond_wait(&bsem_p->cond, &bsem_p->mutex);
	}
	bsem_p->v = 0;
	pthread_mutex_unlock(&bsem_p->mutex);
}
