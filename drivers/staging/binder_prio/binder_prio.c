#include <linux/swap.h>
#include <linux/module.h>
#include <trace/hooks/binder.h>
#include <uapi/linux/android/binder.h>
#include <uapi/linux/sched/types.h>
#include <linux/sched/prio.h>
#include <linux/namei.h>
#include <../../android/binder_internal.h>
#include <../../../kernel/sched/sched.h>
#include <linux/string.h>

#ifdef CONFIG_BINDER_PRIO_DEBUG
#include <linux/module.h>

static uint __read_mostly debug = 0;
module_param(debug, uint, 0644);
#endif

static bool __read_mostly is_miui_rom = false;
static const char *miui_framework = "/system/framework/MiuiBooster.jar";

static const char *task_name[] = {
	"droid.launcher3",  // com.android.launcher3
	"ndroid.systemui",  // com.android.systemui
	// "surfaceflinger",
	"cameraserver",
};

static const char *task_name_miui[] = {
	"com.miui.home",
	".globallauncher",  // com.mi.android.globallauncher
	"rsonalassistant",  // com.miui.personalassistant
};

static int to_userspace_prio(int policy, int kernel_priority) {
	if (fair_policy(policy))
		return PRIO_TO_NICE(kernel_priority);
	else
		return MAX_USER_RT_PRIO - 1 - kernel_priority;
}

static bool set_binder_rt_task(struct binder_transaction *t) {
	int i;

	if (t && t->from && t->from->task && t->to_proc && t->to_proc->tsk && (!(t->flags & TF_ONE_WAY)) &&
	    rt_policy(t->from->task->policy)) {
		#define from_task_comm    t->from->task->comm
		#define from_task_gl_comm t->from->task->group_leader->comm

		if (is_miui_rom) {
			if (!strncmp(from_task_gl_comm, "com.miui.home", strlen("com.miui.home")) &&
			    !strncmp(from_task_comm, "RenderThread", strlen("RenderThread")) &&
			    !strncmp(t->to_proc->tsk->comm, "surfaceflinger", strlen("surfaceflinger")))
				goto yes_and_exit;
			if (!strncmp(from_task_gl_comm, "surfaceflinger", strlen("surfaceflinger")) &&
			    !strncmp(from_task_comm, "passBlur", strlen("passBlur")))
				goto yes_and_exit;
		}
		if (!strncmp(from_task_gl_comm, "cameraserver", strlen("cameraserver")) &&
		    !strncmp(from_task_comm, "C3Dev-", strlen("C3Dev-")) &&
		    strstr(from_task_comm, "-ReqQ"))
			goto yes_and_exit;
		/*
		 * `wmshell.main` and `wmshell.splashscreen` threads are defined in
		 * `com.android.wm.shell.dagger.WMShellConcurrencyModule` in the Android source code.
		 */
		if (!strncmp(from_task_comm, "wmshell.main", strlen("wmshell.main")) ||
		    !strncmp(from_task_comm, "ll.splashscreen", strlen("ll.splashscreen")))
			goto yes_and_exit;
		if (t->from->task->pid == t->from->task->tgid) {
			for (i = 0; i < ARRAY_SIZE(task_name); i++)
				if (strncmp(from_task_comm, task_name[i], strlen(task_name[i])) == 0)
					goto yes_and_exit;
			if (is_miui_rom) {
				for (i = 0; i < ARRAY_SIZE(task_name_miui); i++)
					if (strncmp(from_task_comm, task_name_miui[i], strlen(task_name_miui[i])) == 0)
						goto yes_and_exit;
			}
		}

		return false;

yes_and_exit:
#ifdef CONFIG_BINDER_PRIO_DEBUG
		if (debug)
			pr_info("binder_prio: %s: tid: %d, from_task: %s, from_task_gl: %s; to_task: %s\n",
				__func__, t->from->task->pid,
				t->from->task->comm, t->from->task->group_leader->comm, t->to_proc->tsk->comm);
#endif
		return true;

		#undef from_task_comm
		#undef from_task_gl_comm
	}
	return false;
}

static void extend_surfacefinger_binder_set_priority_handler(void *data, struct binder_transaction *t, struct task_struct *task) {
	struct sched_param params;
	struct binder_priority desired;
	unsigned int policy;
	struct binder_node *target_node = t->buffer->target_node;

	desired.prio = target_node->min_priority;
	desired.sched_policy = target_node->sched_policy;
	policy = desired.sched_policy;
	if (set_binder_rt_task(t)) {
		desired.sched_policy = SCHED_FIFO;
		desired.prio = 98;
		policy = desired.sched_policy;
	}
	if (rt_policy(policy) && task->policy != policy) {
		params.sched_priority = to_userspace_prio(policy, desired.prio);
		sched_setscheduler_nocheck(task, policy | SCHED_RESET_ON_FORK, &params);
	}
}

static void extend_surfacefinger_binder_trans_handler(void *data, struct binder_proc *target_proc,
    struct binder_proc *proc,struct binder_thread *thread, struct binder_transaction_data *tr) {
	if (target_proc && target_proc->tsk && strncmp(target_proc->tsk->comm, "surfaceflinger",
		strlen("surfaceflinger")) == 0) {
		if (thread && proc && tr && thread->transaction_stack
			&& (!(thread->transaction_stack->flags & TF_ONE_WAY))) {
			target_proc->default_priority.sched_policy = SCHED_FIFO;
			target_proc->default_priority.prio = 98;
		}
	}
}

static void extend_skip_binder_thread_priority_from_rt_to_normal_handler(void *data, struct task_struct *task, bool *skip) {
	if (task && rt_policy(task->policy)) {
		*skip = true;
	}
}

int __init binder_prio_init(void)
{
    struct path path;

    pr_info("binder_prio: module init!");

    if (kern_path(miui_framework, LOOKUP_FOLLOW, &path) == 0) {
        pr_info("binder_prio: Miui/HyperOS rom detected!\n");
        is_miui_rom = true;
    } else {
        pr_info("binder_prio: AOSP rom detected!\n");
        is_miui_rom = false;
    }
    path_put(&path);

    register_trace_android_vh_binder_set_priority(extend_surfacefinger_binder_set_priority_handler, NULL);
    register_trace_android_vh_binder_trans(extend_surfacefinger_binder_trans_handler, NULL);
    register_trace_android_vh_binder_priority_skip(extend_skip_binder_thread_priority_from_rt_to_normal_handler, NULL);

    return 0;
}

void __exit binder_prio_exit(void)
{
    unregister_trace_android_vh_binder_set_priority(extend_surfacefinger_binder_set_priority_handler, NULL);
    unregister_trace_android_vh_binder_trans(extend_surfacefinger_binder_trans_handler, NULL);
    unregister_trace_android_vh_binder_priority_skip(extend_skip_binder_thread_priority_from_rt_to_normal_handler, NULL);

    pr_info("binder_prio: module exit!");
}

module_init(binder_prio_init);
module_exit(binder_prio_exit);
MODULE_LICENSE("GPL");
