/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#include "co_routine.h"
#include "co_routine_inner.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <map>

#include <poll.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <errno.h>

#include <assert.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C"
{
	extern void coctx_swap( coctx_t *,coctx_t* ) asm("coctx_swap");
};
using namespace std;
stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env );
struct stCoEpoll_t;

/* 协程环境类型 - 每个线程有且仅有一个该类型的变量
 *
 * 该结构的作用是什么呢？ - 我们知道, 非对称协程允许嵌套创建子协程, 为了记录这种嵌套创建的协程, 以便子协程退出
 * 时正确恢复到挂起点(挂起点位于父协程中), 我们就需要记录这种嵌套调用过程; 另外, 协程中的套接字向内核注册了事件,
 * 我们必须保存套接字和协程的对应关系, 以便该线程的eventloop中检测到套接字上事件发生时, 能够恢复该套接字对应的
 * 协程来处理事件.
 * */
struct stCoRoutineEnv_t
{
	stCoRoutine_t *pCallStack[ 128 ]; // 该线程内允许嵌套创建128个协程(即协程1内创建协程2, 协程2内创建协程3... 协程127内创建协程128. 该结构虽然是数组, 但将其作为栈来使用, 满足后进先出的特点)
	int iCallStackSize;               // 该线程内嵌套创建的协程数量, 即pCallStack数组中元素的数量
	stCoEpoll_t *pEpoll;              // 该线程内的epoll实例(套接字通过该结构内的epoll句柄向内核注册事件), 也用于该线程的事件循环eventloop中
};

/* co_log_err - 协程日志输出 */
void co_log_err( const char *fmt,... )
{
}

// 辅助功能函数, 就不详细说明了
static unsigned long long counter(void);
static unsigned long long getCpuKhz()
{
	FILE *fp = fopen("/proc/cpuinfo","r");
	if(!fp) return 1;
	char buf[4096] = {0};
	fread(buf,1,sizeof(buf),fp);
	fclose(fp);

	char *lp = strstr(buf,"cpu MHz");
	if(!lp) return 1;
	lp += strlen("cpu MHz");
	while(*lp == ' ' || *lp == '\t' || *lp == ':')
	{
		++lp;
	}

	double mhz = atof(lp);
	unsigned long long u = (unsigned long long)(mhz * 1000);
	return u;
}

static unsigned long long GetTickMS()
{
	static uint32_t khz = getCpuKhz();
	return counter() / khz;
}

static unsigned long long counter(void)
{
	register uint32_t lo, hi;
	register unsigned long long o;
    __asm__ __volatile__ (
        "rdtscp" : "=a"(lo), "=d"(hi)
        );
	o = hi;
	o <<= 32;
	return (o | lo);
}

/*
 * GetPid - 获取当前线程id
 * @return void
 * */
static pid_t GetPid()
{
    static __thread pid_t pid = 0;
    static __thread pid_t tid = 0;
    if( !pid || !tid || pid != getpid() )
    {
        pid = getpid();                 // 获取当前进程id
        tid = syscall( __NR_gettid );   // 获取当前线程id
    }
    return tid;

}
/*
static pid_t GetPid()
{
	char **p = (char**)pthread_self();
	return p ? *(pid_t*)(p + 18) : getpid();
}
*/
template <class T,class TLink>
void RemoveFromLink(T *ap)
{
	TLink *lst = ap->pLink;
	if(!lst) return ;
	assert( lst->head && lst->tail );

	if( ap == lst->head )
	{
		lst->head = ap->pNext;
		if(lst->head)
		{
			lst->head->pPrev = NULL;
		}
	}
	else
	{
		if(ap->pPrev)
		{
			ap->pPrev->pNext = ap->pNext;
		}
	}

	if( ap == lst->tail )
	{
		lst->tail = ap->pPrev;
		if(lst->tail)
		{
			lst->tail->pNext = NULL;
		}
	}
	else
	{
		ap->pNext->pPrev = ap->pPrev;
	}

	ap->pPrev = ap->pNext = NULL;
	ap->pLink = NULL;
}

template <class TNode,class TLink>
void inline AddTail(TLink*apLink,TNode *ap)
{
	if( ap->pLink )
	{
		return ;
	}
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)ap;
		ap->pNext = NULL;
		ap->pPrev = apLink->tail;
		apLink->tail = ap;
	}
	else
	{
		apLink->head = apLink->tail = ap;
		ap->pNext = ap->pPrev = NULL;
	}
	ap->pLink = apLink;
}
template <class TNode,class TLink>
void inline PopHead( TLink*apLink )
{
	if( !apLink->head ) 
	{
		return ;
	}
	TNode *lp = apLink->head;
	if( apLink->head == apLink->tail )
	{
		apLink->head = apLink->tail = NULL;
	}
	else
	{
		apLink->head = apLink->head->pNext;
	}

	lp->pPrev = lp->pNext = NULL;
	lp->pLink = NULL;

	if( apLink->head )
	{
		apLink->head->pPrev = NULL;
	}
}

template <class TNode,class TLink>
void inline Join( TLink*apLink,TLink *apOther )
{
	//printf("apOther %p\n",apOther);
	if( !apOther->head )
	{
		return ;
	}
	TNode *lp = apOther->head;
	while( lp )
	{
		lp->pLink = apLink;
		lp = lp->pNext;
	}
	lp = apOther->head;
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)lp;
		lp->pPrev = apLink->tail;
		apLink->tail = apOther->tail;
	}
	else
	{
		apLink->head = apOther->head;
		apLink->tail = apOther->tail;
	}

	apOther->head = apOther->tail = NULL;
}


// ----------------------------------------------------------------------------
struct stTimeoutItemLink_t;
struct stTimeoutItem_t;

/* 线程epoll实例 - 该结构存在于stCoRoutineEnv_t结构中
 *
 * 同一线程内所有的套接字都通过iEpollFd文件描述符向内核注册事件
 * */
struct stCoEpoll_t
{
	int iEpollFd;                               // 由epoll_create函数创建的epoll句柄
	static const int _EPOLL_SIZE = 1024 * 10;
	struct stTimeout_t *pTimeout;
	struct stTimeoutItemLink_t *pstTimeoutList;
	struct stTimeoutItemLink_t *pstActiveList;

};
typedef void (*OnPreparePfn_t)( stTimeoutItem_t *,struct epoll_event &ev, stTimeoutItemLink_t *active );
typedef void (*OnProcessPfn_t)( stTimeoutItem_t *);

/* 详见stPoll_t结构说明 */
struct stTimeoutItem_t
{

	enum
	{
		eMaxTimeout = 20 * 1000 //20s
	};
	stTimeoutItem_t *pPrev;
	stTimeoutItem_t *pNext;
	stTimeoutItemLink_t *pLink;

	unsigned long long ullExpireTime;

	OnPreparePfn_t pfnPrepare;
	OnProcessPfn_t pfnProcess;

	void *pArg; // routine
	bool bTimeout;
};
struct stTimeoutItemLink_t
{
	stTimeoutItem_t *head;
	stTimeoutItem_t *tail;

};
struct stTimeout_t
{
	stTimeoutItemLink_t *pItems;
	int iItemSize;

	unsigned long long ullStart;
	long long llStartIdx;
};
stTimeout_t *AllocTimeout( int iSize )
{
	stTimeout_t *lp = (stTimeout_t*)calloc( 1,sizeof(stTimeout_t) );	

	lp->iItemSize = iSize;
	lp->pItems = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) * lp->iItemSize );

	lp->ullStart = GetTickMS();
	lp->llStartIdx = 0;

	return lp;
}
void FreeTimeout( stTimeout_t *apTimeout )
{
	free( apTimeout->pItems );
	free ( apTimeout );
}
int AddTimeout( stTimeout_t *apTimeout,stTimeoutItem_t *apItem ,unsigned long long allNow )
{
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}
	if( allNow < apTimeout->ullStart )
	{
		co_log_err("CO_ERR: AddTimeout line %d allNow %llu apTimeout->ullStart %llu",
					__LINE__,allNow,apTimeout->ullStart);

		return __LINE__;
	}
	if( apItem->ullExpireTime < allNow )
	{
		co_log_err("CO_ERR: AddTimeout line %d apItem->ullExpireTime %llu allNow %llu apTimeout->ullStart %llu",
					__LINE__,apItem->ullExpireTime,allNow,apTimeout->ullStart);

		return __LINE__;
	}
	int diff = apItem->ullExpireTime - apTimeout->ullStart;

	if( diff >= apTimeout->iItemSize )
	{
		co_log_err("CO_ERR: AddTimeout line %d diff %d",
					__LINE__,diff);

		return __LINE__;
	}
	AddTail( apTimeout->pItems + ( apTimeout->llStartIdx + diff ) % apTimeout->iItemSize , apItem );

	return 0;
}
inline void TakeAllTimeout( stTimeout_t *apTimeout,unsigned long long allNow,stTimeoutItemLink_t *apResult )
{
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}

	if( allNow < apTimeout->ullStart )
	{
		return ;
	}
	int cnt = allNow - apTimeout->ullStart + 1;
	if( cnt > apTimeout->iItemSize )
	{
		cnt = apTimeout->iItemSize;
	}
	if( cnt < 0 )
	{
		return;
	}
	for( int i = 0;i<cnt;i++)
	{
		int idx = ( apTimeout->llStartIdx + i) % apTimeout->iItemSize;
		Join<stTimeoutItem_t,stTimeoutItemLink_t>( apResult,apTimeout->pItems + idx  );
	}
	apTimeout->ullStart = allNow;
	apTimeout->llStartIdx += cnt - 1;


}

/*
 * CoRoutineFunc - 所有新协程第一次被调度执行时的入口函数, 新协程在该入口函数中被执行
 * @param co -        (input) 第一次被调度的协程
 * @param 未命名指针 - (input) 用于兼容函数类型
 * */
static int CoRoutineFunc( stCoRoutine_t *co,void * )
{
	if( co->pfn ) // 执行协程函数
	{
		co->pfn( co->arg );
	}
	co->cEnd = 1; //执行结束将cEnd置1

	stCoRoutineEnv_t *env = co->env;  // 获取当前线程的调度器

	co_yield_env( env );              // 删除调度器的协程数组中最后一个协程

	return 0;
}

/*
 * co_create_env - 分配协程存储空间(stCoRoutine_t)并初始化其中的部分成员变量
 * @param env - (input) 当前线程环境,用于初始化协程存储结构stCoRoutine_t
 * @param pfn - (input) 协程函数,用于初始化协程存储结构stCoRoutine_t
 * @param arg - (input) 协程函数的参数,用于初始化协程存储结构stCoRoutine_t
 * @return stCoRoutine_t类型的指针
 * */
struct stCoRoutine_t *co_create_env( stCoRoutineEnv_t * env,pfn_co_routine_t pfn,void *arg )
{
	stCoRoutine_t *lp = (stCoRoutine_t*)malloc( sizeof(stCoRoutine_t) ); // 分配写成存储空间

	memset( lp,0,(long)((stCoRoutine_t*)0)->sRunStack );                 // 初始化除sRunStack外的其他内存空间

	lp->env = env;                            // 初始化协程中的线程环境
	lp->pfn = pfn;                            // 初始化协程函数
	lp->arg = arg;                            // 初始化协程函数的参数

	lp->ctx.ss_sp = lp->sRunStack ;           // 初始化协程栈
	lp->ctx.ss_size = sizeof(lp->sRunStack) ; // 初始化协程栈大小

	return lp;
}

/*
 * co_create - 创建协程
 * @param ppco - (output) 协程指针的地址(传入前未分配内存空间,即未初始化),在函数体中将为协程申请内存空间, 且该内存空间的地址将为ppco赋值
 * @param attr - (input)  协程属性
 * @param pfn  - (input)  协程函数
 * @param arg  - (input)  协程函数的参数
 * @return 成功返回0.
 * */
int co_create( stCoRoutine_t **ppco,const stCoRoutineAttr_t *attr,pfn_co_routine_t pfn,void *arg )
{
	if( !co_get_curr_thread_env() ) 
	{
		co_init_curr_thread_env();      // 初始化协程环境(协程环境其实就是调度器)
	}
	stCoRoutine_t *co = co_create_env( co_get_curr_thread_env(),pfn,arg );
	*ppco = co;
	return 0;
}

/*
 * co_free - 无论协程处于什么状态, 释放协程co占用的内存空间
 * @param co - (input) 待释放空间的协程
 * @return void
 * */
void co_free( stCoRoutine_t *co )
{
	free( co );
}

/*
 * co_release - 协程处于执行结束状态, 释放协程co占用的内存空间
 * @param co - (input) 待释放空间的协程
 * @return void
 * */
void co_release( stCoRoutine_t *co )
{
	if( co->cEnd )
	{
		free( co );
	}
}

/*
 * co_resume - 执行协程
 * @param co - (input) 待切换的协程
 * @return void
 * */
void co_resume( stCoRoutine_t *co )
{
	stCoRoutineEnv_t *env = co->env;                                            // 获取协程co的调度器
	stCoRoutine_t *lpCurrRoutine = env->pCallStack[ env->iCallStackSize - 1 ];  // 在协程co的协程环境的协程数组末尾获取当前正在执行的协程lpCurrRoutine
	if( !co->cStart )
	{
		// 如果当前协程是第一次被调度,则通过入口函数CoRoutineFunc来为其构造上下文
		coctx_make( &co->ctx,(coctx_pfn_t)CoRoutineFunc,co,0 );
		co->cStart = 1;
	}
	env->pCallStack[ env->iCallStackSize++ ] = co;                              // 将协程co加入到协程环境的协程数组末尾
	coctx_swap( &(lpCurrRoutine->ctx),&(co->ctx) );                             // 保存当前上下文到lpCurrRoutine->ctx, 并切换到新的上下文co->ctx
}

/*
 * co_yield_env - 删除协程环境的协程数组中最后一个协程(即当前正在执行的协程)
 * @param env - (input) 当前线程的调度器
 * @return void
 * */
void co_yield_env( stCoRoutineEnv_t *env )
{
	
	stCoRoutine_t *last = env->pCallStack[ env->iCallStackSize - 2 ];   // 上次切换协程时, 被当前协程切换出去的协程
	stCoRoutine_t *curr = env->pCallStack[ env->iCallStackSize - 1 ];   // 当前协程

	env->iCallStackSize--;                                              // 删除当前协程

	coctx_swap( &curr->ctx, &last->ctx );                               // 切换到上次被切换出去的协程last
}

/*
 * co_yield_ct - 删除协程环境的协程数组中最后一个协程(即当前正在执行的协程)
 * @return void
 * */
void co_yield_ct()
{

	co_yield_env( co_get_curr_thread_env() );
}

/*
 * co_yield - 删除协程环境的协程数组中最后一个协程(即当前正在执行的协程)
 * @param co - (input) 用于获取调度器
 * @return void
 * */
void co_yield( stCoRoutine_t *co )
{
	co_yield_env( co->env );
}



//int poll(struct pollfd fds[], nfds_t nfds, int timeout);
// { fd,events,revents }
struct stPollItem_t ;
struct stPoll_t : public stTimeoutItem_t 
{
	struct pollfd *fds;                           // 待检测的套接字描述符集合
	nfds_t nfds;                                  // 待检测的套接字描述符个数
	stPollItem_t *pPollItems;                     // (重点)存储了待检测的每个文件描述符的信息(详见下面注释)
//	struct stPollItem_t : public stTimeoutItem_t
//	{
//		struct pollfd *pSelf;                     // 待检测的套接字描述符集合
//		stPoll_t *pPoll;                          // 指向存储该stPollItem_t结构的stPoll_t类型变量地址
//		struct epoll_event stEvent;               // 待检测的套接字描述符的事件
//	};

	int iAllEventDetach;
	int iEpollFd;                                 // 由epoll_create函数创建的epoll句柄, 检测事件通过该句柄向内核通知
	int iRaiseCnt;                                // 发生事件的套接字数量

// 	struct stTimeoutItem_t
//  {
//		enum
//		{
//			eMaxTimeout = 20 * 1000 //20s
//		};
//		stTimeoutItem_t *pPrev;
//		stTimeoutItem_t *pNext;
//		stTimeoutItemLink_t *pLink;
//
//		unsigned long long ullExpireTime;         // 超时时的系统时间
//
//		OnPreparePfn_t pfnPrepare;                //
//		OnProcessPfn_t pfnProcess;                // 事件发生时的回调函数, 其主要功能是恢复pArg指向的协程
//
//		void *pArg; // routine                    // 值为协程结构stCoRoutine_t的指针, 指针指向的协程为该待检测套接字所属的协程, 在事件发生时从该值中获得并恢复协程
//		bool bTimeout;                            // 是否超时标志, True表示超时时间内套接字上没有事件发生, False表示超时时间内套接字上有事件发生
//  }
};
/* 详见stPoll_t说明 */
struct stPollItem_t : public stTimeoutItem_t
{
	struct pollfd *pSelf;
	stPoll_t *pPoll;

	struct epoll_event stEvent;
};
/*
 *   EPOLLPRI 		POLLPRI    // There is urgent data to read.  
 *   EPOLLMSG 		POLLMSG
 *
 *   				POLLREMOVE
 *   				POLLRDHUP
 *   				POLLNVAL
 *
 * */
static uint32_t PollEvent2Epoll( short events )
{
	uint32_t e = 0;	
	if( events & POLLIN ) 	e |= EPOLLIN;
	if( events & POLLOUT )  e |= EPOLLOUT;
	if( events & POLLHUP ) 	e |= EPOLLHUP;
	if( events & POLLERR )	e |= EPOLLERR;
	return e;
}
static short EpollEvent2Poll( uint32_t events )
{
	short e = 0;	
	if( events & EPOLLIN ) 	e |= POLLIN;
	if( events & EPOLLOUT ) e |= POLLOUT;
	if( events & EPOLLHUP ) e |= POLLHUP;
	if( events & EPOLLERR ) e |= POLLERR;
	return e;
}

/* 协程环境数组, 数组中元素类型为stCoRoutineEnv_t的指针 */
static stCoRoutineEnv_t* g_arrCoEnvPerThread[ 102400 ] = { 0 };

/*
 * co_init_curr_thread_env - 为当前线程分配协程环境存储空间(stCoRoutineEnv_t)并初始化其中的部分成员变量
 * @return void
 * */
void co_init_curr_thread_env()
{
	// (为当前线程)分配调度器存储空间(stCoRoutineEnv_t)
	pid_t pid = GetPid();	                                     // 获取当前线程id
	g_arrCoEnvPerThread[ pid ] = (stCoRoutineEnv_t*)calloc( 1,sizeof(stCoRoutineEnv_t) ); // 为当前线程分配线程环境的存储空间
	stCoRoutineEnv_t *env = g_arrCoEnvPerThread[ pid ];
	printf("init pid %ld env %p\n",(long)pid,env);               // Always use %p for pointers in printf

	// 初始化协程环境(stCoRoutineEnv_t)中的部分成员变量
	env->iCallStackSize = 0;                                     // 初始化(当前线程)调度器中协程栈大小为0
	struct stCoRoutine_t *self = co_create_env( env,NULL,NULL ); // 将当前线程中的上下文包装成主协程
	self->cIsMain = 1;

	coctx_init( &self->ctx );                                    // 将包装好的主协程中的上下文置零

	env->pCallStack[ env->iCallStackSize++ ] = self;             // 将包装好的主协程加入调度器的协程数组中

	stCoEpoll_t *ev = AllocEpoll();                              // 为调度器创建epoll文件描述符并分配超时链表的存储空间
	SetEpoll( env,ev );                                          // 将ev加入到调度器中
}

/*
 * co_get_curr_thread_env - 获取当前线程的协程环境
 * @return 返回当前线程的调度器指针
 * */
stCoRoutineEnv_t *co_get_curr_thread_env()
{
	return g_arrCoEnvPerThread[ GetPid() ];
}

/*
 * OnPollProcessEvent - 事件发生时的回调函数, 其主要功能是恢复pArg指向的协程
 * */
void OnPollProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}

void OnPollPreparePfn( stTimeoutItem_t * ap,struct epoll_event &e,stTimeoutItemLink_t *active )
{
	stPollItem_t *lp = (stPollItem_t *)ap;
	lp->pSelf->revents = EpollEvent2Poll( e.events );


	stPoll_t *pPoll = lp->pPoll;
	pPoll->iRaiseCnt++;

	if( !pPoll->iAllEventDetach )
	{
		pPoll->iAllEventDetach = 1;

		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( pPoll );

		AddTail( active,pPoll );

	}
}

/* co_eventloop - 事件循环, 作用是检测套接字上的事件并恢复相关协程来处理事件 */
void co_eventloop( stCoEpoll_t *ctx,pfn_co_eventloop_t pfn,void *arg )
{
	epoll_event *result = (epoll_event*)calloc(1, sizeof(epoll_event) * stCoEpoll_t::_EPOLL_SIZE );

	for(;;)
	{
		int ret = epoll_wait( ctx->iEpollFd,result,stCoEpoll_t::_EPOLL_SIZE, 1 );

		stTimeoutItemLink_t *active = (ctx->pstActiveList);
		stTimeoutItemLink_t *timeout = (ctx->pstTimeoutList);

		memset( active,0,sizeof(stTimeoutItemLink_t) );
		memset( timeout,0,sizeof(stTimeoutItemLink_t) );

		for(int i=0;i<ret;i++)
		{
			stTimeoutItem_t *item = (stTimeoutItem_t*)result[i].data.ptr;
			if( item->pfnPrepare )
			{
				item->pfnPrepare( item,result[i],active );
			}
			else
			{
				AddTail( active,item );
			}
		}


		unsigned long long now = GetTickMS();
		TakeAllTimeout( ctx->pTimeout,now,timeout );

		stTimeoutItem_t *lp = timeout->head;
		while( lp )
		{
			//printf("raise timeout %p\n",lp);
			lp->bTimeout = true;
			lp = lp->pNext;
		}

		Join<stTimeoutItem_t,stTimeoutItemLink_t>( active,timeout );

		lp = active->head;
		while( lp )
		{

			PopHead<stTimeoutItem_t,stTimeoutItemLink_t>( active );
			if( lp->pfnProcess )
			{
				lp->pfnProcess( lp );    // 恢复协程处理事件
			}

			lp = active->head;
		}
		if( pfn )
		{
			if( -1 == pfn( arg ) )
			{
				break;
			}
		}

	}
	free( result );
	result = 0;
}

void OnCoroutineEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}

/* AllocEpoll - 为当前线程分配stCoEpoll_t类型的存储空间, 并初始化
 * @return 函数中分配的stCoEpoll_t类型空间的地址
 * */
stCoEpoll_t *AllocEpoll()
{
	stCoEpoll_t *ctx = (stCoEpoll_t*)calloc( 1,sizeof(stCoEpoll_t) );

	ctx->iEpollFd = epoll_create( stCoEpoll_t::_EPOLL_SIZE );
	ctx->pTimeout = AllocTimeout( 60 * 1000 );
	
	ctx->pstActiveList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );
	ctx->pstTimeoutList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );


	return ctx;
}

/* FreeEpoll - 释放当前线程中的stCoEpoll_t类型的存储空间
 * @param ctx (input) 待释放的stCoEpoll_t类型存储空间的地址
 * @return void
 * */
void FreeEpoll( stCoEpoll_t *ctx )
{
	if( ctx )
	{
		free( ctx->pstActiveList );
		free( ctx->pstTimeoutList );
		FreeTimeout( ctx->pTimeout );
	}
	free( ctx );
}

/* GetCurrCo - 获取某一协程环境中正在执行的协程
 * @param env (input) 协程环境
 * return 正在执行的协程的地址
 * */
stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env )
{
	return env->pCallStack[ env->iCallStackSize - 1 ];
}

/* GetCurrThreadCo - 获取当前线程中正在执行的协程
 * @param env (input) 协程环境
 * return 正在执行的协程的地址
 * */
stCoRoutine_t *GetCurrThreadCo( )
{
	stCoRoutineEnv_t *env = co_get_curr_thread_env();
	if( !env ) return 0;
	return GetCurrCo(env);
}


/* co_poll - 该函数主要向内核注册套接字上待监听的事件, 然后切换协程, 当该协程被恢复时即说明程序结束, 然后处理善后工作 */
int co_poll( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout )
{
	
	if( timeout > stTimeoutItem_t::eMaxTimeout )
	{
		timeout = stTimeoutItem_t::eMaxTimeout;
	}
	int epfd = ctx->iEpollFd;

	//1.struct change
	stPoll_t arg;
	memset( &arg,0,sizeof(arg) );

	arg.iEpollFd = epfd;
	arg.fds = fds;
	arg.nfds = nfds;

	stPollItem_t arr[2];
	if( nfds < sizeof(arr) / sizeof(arr[0]) )
	{
		arg.pPollItems = arr;
	}	
	else
	{
		arg.pPollItems = (stPollItem_t*)malloc( nfds * sizeof( stPollItem_t ) );
	}
	memset( arg.pPollItems,0,nfds * sizeof(stPollItem_t) );

	arg.pfnProcess = OnPollProcessEvent;
	arg.pArg = GetCurrCo( co_get_curr_thread_env() );
	
	//2.add timeout

	unsigned long long now = GetTickMS();
	arg.ullExpireTime = now + timeout;
	int ret = AddTimeout( ctx->pTimeout,&arg,now );
	if( ret != 0 )
	{
		co_log_err("CO_ERR: AddTimeout ret %d now %lld timeout %d arg.ullExpireTime %lld",
					ret,now,timeout,arg.ullExpireTime);
		errno = EINVAL;
		return -__LINE__;
	}
	//3. add epoll

	for(nfds_t i=0;i<nfds;i++)
	{
		arg.pPollItems[i].pSelf = fds + i;
		arg.pPollItems[i].pPoll = &arg;

		arg.pPollItems[i].pfnPrepare = OnPollPreparePfn;
		struct epoll_event &ev = arg.pPollItems[i].stEvent;

		if( fds[i].fd > -1 )
		{
			ev.data.ptr = arg.pPollItems + i;
			ev.events = PollEvent2Epoll( fds[i].events );

			epoll_ctl( epfd,EPOLL_CTL_ADD, fds[i].fd, &ev );
		}
		//if fail,the timeout would work
		
	}

	co_yield_env( co_get_curr_thread_env() );

	RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &arg );
	for(nfds_t i = 0;i < nfds;i++)
	{
		int fd = fds[i].fd;
		if( fd > -1 )
		{
			epoll_ctl( epfd,EPOLL_CTL_DEL,fd,&arg.pPollItems[i].stEvent );
		}
	}


	if( arg.pPollItems != arr )
	{
		free( arg.pPollItems );
		arg.pPollItems = NULL;
	}
	return arg.iRaiseCnt;
}

void SetEpoll( stCoRoutineEnv_t *env,stCoEpoll_t *ev )
{
	env->pEpoll = ev;
}

/* co_get_epoll_ct - 获取(当前线程中)协程环境中的epoll实例 */
stCoEpoll_t *co_get_epoll_ct()
{
	if( !co_get_curr_thread_env() )
	{
		co_init_curr_thread_env();
	}
	return co_get_curr_thread_env()->pEpoll;
}
struct stHookPThreadSpec_t
{
	stCoRoutine_t *co;
	void *value;

	enum 
	{
		size = 1024
	};
};
void *co_getspecific(pthread_key_t key)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_getspecific( key );
	}
	return co->aSpec[ key ].value;
}
int co_setspecific(pthread_key_t key, const void *value)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_setspecific( key,value );
	}
	co->aSpec[ key ].value = (void*)value;
	return 0;
}


/*
 * co_disable_hook_sys - 禁止hook系统调用
 * return void
 * */
void co_disable_hook_sys()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( co )
	{
		co->cEnableSysHook = 0;
	}
}

/*
 * co_is_enable_sys_hook - 判断协程中的系统调用是否被hook
 * @return hook了系统调用返回true, 否则返回false
 * */
bool co_is_enable_sys_hook()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	return ( co && co->cEnableSysHook );
}


stCoRoutine_t *co_self()
{
	return GetCurrThreadCo();
}
