// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <linux/cgroup.h>
#include <linux/bpf_verifier.h>
#include <linux/bpf_sched.h>
#include <linux/btf_ids.h>
#include <linux/cpuidle.h>
#include <linux/bpf_topology.h>
#include "sched.h"

DEFINE_STATIC_KEY_FALSE(bpf_sched_enabled_key);

/*
 * For every hook declare a nop function where a BPF program can be attached.
 */
#define BPF_SCHED_HOOK(RET, DEFAULT, NAME, ...)	\
noinline RET bpf_sched_##NAME(__VA_ARGS__)	\
{						\
	return DEFAULT;				\
}

#include <linux/sched_hook_defs.h>
#undef BPF_SCHED_HOOK

#define BPF_SCHED_HOOK(RET, DEFAULT, NAME, ...) BTF_ID(func, bpf_sched_##NAME)
BTF_SET_START(bpf_sched_hooks)
#include <linux/sched_hook_defs.h>
#undef BPF_SCHED_HOOK
BTF_SET_END(bpf_sched_hooks)

const struct bpf_func_proto bpf_init_cpu_topology_proto __weak;
const struct bpf_func_proto bpf_get_cpumask_info_proto __weak;

int bpf_sched_verify_prog(struct bpf_verifier_log *vlog,
			  const struct bpf_prog *prog)
{
	if (!prog->gpl_compatible) {
		bpf_log(vlog,
			"sched programs must have a GPL compatible license\n");
		return -EINVAL;
	}

	if (!btf_id_set_contains(&bpf_sched_hooks, prog->aux->attach_btf_id)) {
		bpf_log(vlog, "attach_btf_id %u points to wrong type name %s\n",
			prog->aux->attach_btf_id, prog->aux->attach_func_name);
		return -EINVAL;
	}

	return 0;
}

BPF_CALL_3(bpf_sched_cpu_stats_of, int, cpu,
	   struct bpf_sched_cpu_stats *, ctx,
	   int, len)
{
	struct cpuidle_state *idle;
	struct rq *rq;

	if (len != sizeof(*ctx))
		return -EINVAL;

	if ((unsigned int)cpu >= nr_cpu_ids)
		return -EINVAL;

	rq = cpu_rq(cpu);
	memset(ctx, 0, sizeof(struct bpf_sched_cpu_stats));

	/* load/util */
#ifdef CONFIG_SMP
	SCHED_WARN_ON(!rcu_read_lock_held());
	ctx->cfs_load_avg = rq->cfs.avg.load_avg;
	ctx->cfs_runnable_avg = rq->cfs.avg.runnable_avg;
	ctx->cfs_util_avg = rq->cfs.avg.util_avg;
	ctx->rt_load_avg = rq->avg_rt.load_avg;
	ctx->rt_runnable_avg = rq->avg_rt.runnable_avg;
	ctx->rt_util_avg = rq->avg_rt.util_avg;
#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	ctx->irq_load_avg = rq->avg_irq.load_avg;
	ctx->irq_runnable_avg = rq->avg_irq.runnable_avg;
	ctx->irq_util_avg = rq->avg_irq.util_avg;
#endif
#endif

	/* nr_running */
	ctx->nr_running = rq->nr_running;
	ctx->cfs_nr_running = rq->cfs.nr_running;
	ctx->cfs_h_nr_running = rq->cfs.h_nr_running;
	ctx->cfs_idle_h_nr_running = rq->cfs.idle_h_nr_running;
	ctx->rt_nr_running = rq->rt.rt_nr_running;
	ctx->rr_nr_running = rq->rt.rr_nr_running;

	/* idle statistics */
	ctx->available_idle = available_idle_cpu(cpu);
	idle = idle_get_state(rq);
	if (idle)
		ctx->exit_latency = idle->exit_latency;
#ifdef CONFIG_SMP
	ctx->idle_stamp = rq->idle_stamp;
	ctx->avg_idle = rq->avg_idle;
#endif

	/* capacity */
#ifdef CONFIG_SMP
	ctx->capacity = rq->cpu_capacity;
	ctx->capacity_orig = rq->cpu_capacity_orig;
#endif

	return 0;
}

static const struct bpf_func_proto bpf_sched_cpu_stats_of_proto = {
	.func		= bpf_sched_cpu_stats_of,
	.gpl_only	= false,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_ANYTHING,
	.arg2_type	= ARG_PTR_TO_UNINIT_MEM,
	.arg3_type	= ARG_CONST_SIZE,
};

BTF_ID_LIST_SINGLE(btf_sched_entity_ids, struct, sched_entity)
BTF_ID_LIST_SINGLE(btf_sched_task_ids, struct, task_struct)
BTF_ID_LIST_SINGLE(btf_sched_tg_ids, struct, task_group)

BPF_CALL_1(bpf_sched_entity_is_task, struct sched_entity *, se)
{
	return entity_is_task(se) ? 1 : 0;
}

static const struct bpf_func_proto bpf_sched_entity_is_task_proto = {
	.func		= bpf_sched_entity_is_task,
	.gpl_only	= false,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_BTF_ID,
	.arg1_btf_id	= &btf_sched_entity_ids[0],
};

BPF_CALL_1(bpf_sched_entity_to_task, struct sched_entity *, se)
{
	if (entity_is_task(se)) {
		struct task_struct *tsk = task_of(se);

		return (unsigned long)tsk;
	}

	return (unsigned long)NULL;
}

static const struct bpf_func_proto bpf_sched_entity_to_task_proto = {
	.func		= bpf_sched_entity_to_task,
	.gpl_only	= false,
	.ret_type	= RET_PTR_TO_BTF_ID_OR_NULL,
	.ret_btf_id	= &btf_sched_task_ids[0],
	.arg1_type	= ARG_PTR_TO_BTF_ID,
	.arg1_btf_id	= &btf_sched_entity_ids[0],
};

BPF_CALL_1(bpf_sched_entity_to_tg, struct sched_entity *, se)
{
#if CONFIG_FAIR_GROUP_SCHED
	if (!entity_is_task(se)) {
		struct task_group *tg = group_cfs_rq(se)->tg;

		return (unsigned long)tg;
	}
#endif

	return (unsigned long)NULL;
}

static const struct bpf_func_proto bpf_sched_entity_to_tg_proto = {
	.func		= bpf_sched_entity_to_tg,
	.gpl_only	= false,
	.ret_type	= RET_PTR_TO_BTF_ID_OR_NULL,
	.ret_btf_id	= &btf_sched_tg_ids[0],
	.arg1_type	= ARG_PTR_TO_BTF_ID,
	.arg1_btf_id	= &btf_sched_entity_ids[0],
};

BPF_CALL_2(bpf_cpumask_op, struct cpumask_op_args *, op, int, len)
{
	int ret;

	if (len != sizeof(*op) || !op->arg1)
		return -EINVAL;

	switch (op->op_type) {
	case CPUMASK_EMPTY:
		return cpumask_empty((const struct cpumask *)op->arg1);
	case CPUMASK_AND:
		if (!op->arg2 || !op->arg3)
			return -EINVAL;
		return cpumask_and((struct cpumask *)op->arg1,
				   (const struct cpumask *)op->arg2,
				   (const struct cpumask *)op->arg3);
	case CPUMASK_ANDNOT:
		if (!op->arg2 || !op->arg3)
			return -EINVAL;
		cpumask_andnot((struct cpumask *)op->arg1,
			       (const struct cpumask *)op->arg2,
			       (const struct cpumask *)op->arg3);
		break;
	case CPUMASK_SUBSET:
		if (!op->arg2)
			return -EINVAL;
		return cpumask_subset((const struct cpumask *)op->arg1,
				      (const struct cpumask *)op->arg2);
	case CPUMASK_EQUAL:
		if (!op->arg2)
			return -EINVAL;
		return cpumask_equal((const struct cpumask *)op->arg1,
				     (const struct cpumask *)op->arg2);
	case CPUMASK_TEST_CPU:
		if (!op->arg2)
			return -EINVAL;
		return cpumask_test_cpu(*(int *)op->arg1, op->arg2);
	case CPUMASK_COPY:
		if (!op->arg2)
			return -EINVAL;
		cpumask_copy((struct cpumask *)op->arg1,
			     (const struct cpumask *)op->arg2);
		break;
	case CPUMASK_WEIGHT:
		return cpumask_weight((const struct cpumask *)op->arg1);
	case CPUMASK_NEXT:
		if (!op->arg2)
			return -EINVAL;
		return cpumask_next(*(int *)op->arg1,
				    (const struct cpumask *)op->arg2);
	case CPUMASK_NEXT_WRAP:
		if (!op->arg2 || !op->arg3 || !op->arg4)
			return -EINVAL;
		return cpumask_next_wrap(*(int *)op->arg1,
					 (const struct cpumask *)op->arg2,
					 *(int *)op->arg3, *(int *)op->arg4);
	case CPUMASK_NEXT_AND:
		if (!op->arg2 || !op->arg3)
			return -EINVAL;
		return cpumask_next_and(*(int *)op->arg1,
					(const struct cpumask *)op->arg2,
					(const struct cpumask *)op->arg3);
	case CPUMASK_CPULIST_PARSE:
		if (!op->arg2)
			return -EINVAL;

		op->arg1 = (void *)strstrip((void *)op->arg1);
		ret = cpulist_parse((void *)op->arg1,
				    (struct cpumask *)op->arg2);
		return ret;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct bpf_func_proto bpf_cpumask_op_proto = {
	.func		= bpf_cpumask_op,
	.gpl_only	= false,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_MEM,
	.arg2_type	= ARG_CONST_SIZE,
};

BPF_CALL_2(bpf_cpus_share_cache, int, src_cpu, int, dst_cpu)
{
	if ((unsigned int)src_cpu >= nr_cpu_ids ||
	    (unsigned int)dst_cpu >= nr_cpu_ids)
		return 0;

	return cpus_share_cache(src_cpu, dst_cpu);
}

static const struct bpf_func_proto bpf_cpus_share_cache_proto = {
	.func		= bpf_cpus_share_cache,
	.gpl_only	= false,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_ANYTHING,
	.arg2_type	= ARG_ANYTHING,
};

static const struct bpf_func_proto *
bpf_sched_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	switch (func_id) {
	case BPF_FUNC_trace_printk:
		return bpf_get_trace_printk_proto();
	case BPF_FUNC_sched_cpu_stats_of:
		return &bpf_sched_cpu_stats_of_proto;
	case BPF_FUNC_init_cpu_topology:
		return &bpf_init_cpu_topology_proto;
	case BPF_FUNC_get_cpumask_info:
		return &bpf_get_cpumask_info_proto;
	case BPF_FUNC_sched_entity_is_task:
		return &bpf_sched_entity_is_task_proto;
	case BPF_FUNC_sched_entity_to_task:
		return &bpf_sched_entity_to_task_proto;
	case BPF_FUNC_sched_entity_to_tg:
		return &bpf_sched_entity_to_tg_proto;
	case BPF_FUNC_cpumask_op:
		return &bpf_cpumask_op_proto;
	case BPF_FUNC_cpus_share_cache:
		return &bpf_cpus_share_cache_proto;
	default:
		return bpf_base_func_proto(func_id);
	}
}

const struct bpf_prog_ops bpf_sched_prog_ops = {
};

const struct bpf_verifier_ops bpf_sched_verifier_ops = {
	.get_func_proto = bpf_sched_func_proto,
	.is_valid_access = btf_ctx_access,
};

BPF_CALL_1(bpf_sched_tg_tag_of, struct task_group *, tg)
{
	int ret = 0;

#ifdef CONFIG_CGROUP_SCHED
	if (tg == NULL)
		return -EINVAL;
	ret = tg->tag;
#endif

	return ret;
}

const struct bpf_func_proto bpf_sched_tg_tag_of_proto = {
	.func		= bpf_sched_tg_tag_of,
	.gpl_only	= false,
	.ret_type	= RET_INTEGER,
	.arg1_type	= PTR_MAYBE_NULL | ARG_PTR_TO_BTF_ID,
	.arg1_btf_id	= &btf_sched_tg_ids[0],
};

BPF_CALL_1(bpf_sched_task_tag_of, struct task_struct *, tsk)
{
	if (tsk == NULL)
		return -EINVAL;
	return tsk->tag;
}

const struct bpf_func_proto bpf_sched_task_tag_of_proto = {
	.func		= bpf_sched_task_tag_of,
	.gpl_only	= false,
	.ret_type	= RET_INTEGER,
	.arg1_type	= PTR_MAYBE_NULL | ARG_PTR_TO_BTF_ID,
	.arg1_btf_id	= &btf_sched_task_ids[0],
};

BPF_CALL_2(bpf_sched_set_tg_tag, struct task_group *, tg, s64, tag)
{
#if CONFIG_CGROUP_SCHED
	if (tg == NULL || tg == &root_task_group)
		return -EINVAL;

	if (tg->tag == tag)
		return 0;

	rcu_read_lock();
	walk_tg_tree_from(tg, tg_change_tag, tg_nop, (void *)(&tag));
	rcu_read_unlock();

	return 0;
#endif
	return -EPERM;
}

const struct bpf_func_proto bpf_sched_set_tg_tag_proto = {
	.func		= bpf_sched_set_tg_tag,
	.gpl_only	= false,
	.ret_type	= RET_INTEGER,
	.arg1_type	= PTR_MAYBE_NULL | ARG_PTR_TO_BTF_ID,
	.arg1_btf_id	= &btf_sched_tg_ids[0],
	.arg2_type	= ARG_ANYTHING,
};

BPF_CALL_2(bpf_sched_set_task_tag, struct task_struct *, tsk, s64, tag)
{
	if (tsk == NULL)
		return -EINVAL;

	sched_settag(tsk, tag);
	return 0;
}

const struct bpf_func_proto bpf_sched_set_task_tag_proto = {
	.func		= bpf_sched_set_task_tag,
	.gpl_only	= false,
	.ret_type	= RET_INTEGER,
	.arg1_type	= PTR_MAYBE_NULL | ARG_PTR_TO_BTF_ID,
	.arg1_btf_id	= &btf_sched_task_ids[0],
	.arg2_type	= ARG_ANYTHING,
};
