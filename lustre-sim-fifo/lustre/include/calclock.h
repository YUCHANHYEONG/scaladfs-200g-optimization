/*
 * calclock.h - Per-CPU Calclock
 *
 * A simple function profiling tool utilizing per-CPU variables
 */

#ifndef __CALCLOCK_H
#define __CALCLOCK_H

#include <linux/ktime.h>
#include <linux/percpu.h>
#include "lockfree_list.h"

//#define CONFIG_CALCLOCK

struct calclock {
	ktime_t time;
	unsigned long long count;
};

#define KTDEF(funcname)	\
	DEFINE_PER_CPU(struct calclock, funcname##_clock) = {0, 0}
#define KTDEC(funcname)	\
	DECLARE_PER_CPU(struct calclock, funcname##_clock)

#ifdef CONFIG_CALCLOCK

static inline void ktget(ktime_t *clock)
{
	*clock = ktime_get_raw();
}

static inline void __ktput(ktime_t localclocks[], struct calclock *clock)
{
	ktime_t diff;

	WARN_ONCE(ktime_after(localclocks[0], localclocks[1]),
			"Calclock: The order of two clocks is reversed!");
	diff = ktime_sub(localclocks[1], localclocks[0]);
	clock->time = ktime_add_safe(clock->time, diff);
	clock->count++;
}

#define ktput(localclocks, funcname)				\
do {								\
	struct calclock *clock;					\
								\
	clock = get_cpu_ptr(&(funcname##_clock));		\
	__ktput(localclocks, clock);				\
	put_cpu_ptr(&(funcname##_clock));			\
} while (0)

void __ktprint(int depth, char *fn_name, ktime_t time, 
		uint64_t count, size_t nr_threads);

#define ktprint(depth, funcname)				\
do {								\
	int cpu;						\
	struct calclock *clock;					\
	ktime_t timesum = 0;					\
	uint64_t countsum = 0;					\
	size_t threads = 0;					\
								\
	for_each_online_cpu(cpu) {				\
		clock = per_cpu_ptr(&funcname##_clock, cpu);	\
		timesum += clock->time;				\
		countsum += clock->count;			\
		threads += !!clock->count;			\
	}							\
	__ktprint(depth, #funcname, timesum, countsum, threads);\
} while (0)

#define ktreset(funcname)				\
do {								\
	int cpu;						\
	struct calclock *clock;					\
	for_each_online_cpu(cpu) {				\
		clock = per_cpu_ptr(&funcname##_clock, cpu);	\
		clock->time = 0;				\
		clock->count = 0;				\
	}							\
} while (0)

#else /* !CONFIG_CALCLOCK */
#define ktget(clock) do { (void)(clock); } while (0)
#define ktput(localclock, funcname) do { } while (0)
#define ktprint(depth, funcname) do { } while (0)
#endif /* CONFIG_CALCLOCK */

#endif /* __CALCLOCK_H */
