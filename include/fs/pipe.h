#ifndef _PIPE_H
#define _PIPE_H
#include <lock/mutex.h>
#include <types.h>

#define PIPE_BUF_SIZE (PAGE_SIZE * 32)
struct thread;

struct Pipe {
	// 对管道进行读写访问操作均需要加锁
	struct mutex lock;

	// 下面的引用数是相对于kernfd来说的
	// 该值单调递减，所以为1时表示管道关闭，无需分别维护读者、写者
	u32 count;		   // kernFd引用数
	u64 pipeReadPos;	   // read position
	u64 pipeWritePos;	   // write position
	void * pipeBuf; // data buffer
	struct thread *waitProc;
};

int pipe(int fd[2]);
int pipe_check_read(struct Pipe *p);
int pipe_check_write(struct Pipe *p);

#endif
