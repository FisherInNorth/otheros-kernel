#include <dev/sbi.h>
#include <dev/timer.h>
#include <lib/log.h>
#include <lib/string.h>
#include <lock/mutex.h>
#include <proc/cpu.h>
#include <proc/thread.h>
#include <riscv.h>

struct cpu cpus[NCPU];

/**
 * @brief 获取当前CPU的ID，调用时必须在关中断的情况下
 */
register_t cpu_this_id() {
	return r_tp();
}

/**
 * @brief 获取当前CPU的结构体，调用时必须在关中断的情况下
 */
cpu_t *cpu_this() {
	return &cpus[cpu_this_id()];
}

extern threadq_t thread_sleepq;
u64 last_idle[NCPU];
void cpu_idle() {

// DEBUG：发现长时间睡眠的线程
#ifdef SLEEP_DEBUG
	// 距离上次打印的时间差大于1s，打印一次
	if (time_mono_us() - last_idle[cpu_this_id()] > 20000000ul) {
		last_idle[cpu_this_id()] = time_mono_us();

		// 检查睡眠队列是否有线程睡眠时间过长
		tdq_critical_enter(&thread_sleepq);

		thread_t *td = NULL;
		if (TAILQ_EMPTY(&thread_sleepq.tq_head)) {
			log(999,"\nsleepq is empty.\n");
		} else {
			log(999,"\n");
		}

		TAILQ_FOREACH (td, &thread_sleepq.tq_head, td_sleepq) {
			log(999,"sleepq: thread %s is sleeping on \"%s\"\n", td->td_name,
			     td->td_wmesg);
		}

		extern mutex_t mtx_file;
		if (mtx_file.mtx_owner) {
			log(999,"file mtx is hold by thread %s\n",
			     mtx_file.mtx_owner->td_name);
		}

		tdq_critical_exit(&thread_sleepq);
	}
#endif

	intr_on();
	for (int i = 0; i < 1000000; i++)
		;
	intr_off();
}

void cpu_halt() {
	intr_off();
	SBI_SYSTEM_RESET(0, 0);
	// 关机后不会执行到这里
	while (1)
		;
}

bool cpu_allidle() {
	for (int i = 0; i < NCPU; i++) {
		if (!cpus[i].cpu_idle) {
			return false;
		}
	}
	return true;
}
