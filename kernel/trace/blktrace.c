/*
 * Copyright (C) 2006 Jens Axboe <axboe@kernel.dk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/blktrace_api.h>
#include <linux/percpu.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/time.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/blk-cgroup.h>

#include <trace/events/block.h>

#include "trace_output.h"

#ifdef CONFIG_BLK_DEV_IO_TRACE

static unsigned int blktrace_seq __read_mostly = 1;

static struct trace_array *blk_tr;
static bool blk_tracer_enabled __read_mostly;

static LIST_HEAD(running_trace_list);
static __cacheline_aligned_in_smp DEFINE_SPINLOCK(running_trace_lock);

/* Select an alternative, minimalistic output than the original one */
#define TRACE_BLK_OPT_CLASSIC	0x1
#define TRACE_BLK_OPT_CGROUP	0x2
#define TRACE_BLK_OPT_CGNAME	0x4

static struct tracer_opt blk_tracer_opts[] = {
	/* Default disable the minimalistic output */
	{ TRACER_OPT(blk_classic, TRACE_BLK_OPT_CLASSIC) },
#ifdef CONFIG_BLK_CGROUP
	{ TRACER_OPT(blk_cgroup, TRACE_BLK_OPT_CGROUP) },
	{ TRACER_OPT(blk_cgname, TRACE_BLK_OPT_CGNAME) },
#endif
	{ }
};

static struct tracer_flags blk_tracer_flags = {
	.val  = 0,
	.opts = blk_tracer_opts,
};

/* Global reference count of probes */
static DEFINE_MUTEX(blk_probe_mutex);
static int blk_probes_ref;

static void blk_register_tracepoints(void);
static void blk_unregister_tracepoints(void);

/*
 * Send out a notify message.
 */
static void trace_note(struct blk_trace *bt, pid_t pid, int action,
		       const void *data, size_t len,
		       union kernfs_node_id *cgid)
{
	struct blk_io_trace *t;
	struct ring_buffer_event *event = NULL;
	struct ring_buffer *buffer = NULL;
	int pc = 0;
	int cpu = smp_processor_id();
	bool blk_tracer = blk_tracer_enabled;
	ssize_t cgid_len = cgid ? sizeof(*cgid) : 0;

	if (blk_tracer) {
		buffer = blk_tr->trace_buffer.buffer;
		pc = preempt_count();
		event = trace_buffer_lock_reserve(buffer, TRACE_BLK,
						  sizeof(*t) + len + cgid_len,
						  0, pc);
		if (!event)
			return;
		t = ring_buffer_event_data(event);
		goto record_it;
	}

	if (!bt->rchan)
		return;

	t = relay_reserve(bt->rchan, sizeof(*t) + len + cgid_len);
	if (t) {
		t->magic = BLK_IO_TRACE_MAGIC | BLK_IO_TRACE_VERSION;
		t->time = ktime_to_ns(ktime_get());
record_it:
		t->device = bt->dev;
		t->action = action | (cgid ? __BLK_TN_CGROUP : 0);
		t->pid = pid;
		t->cpu = cpu;
		t->pdu_len = len + cgid_len;
		if (cgid)
			memcpy((void *)t + sizeof(*t), cgid, cgid_len);
		memcpy((void *) t + sizeof(*t) + cgid_len, data, len);

		if (blk_tracer)
			trace_buffer_unlock_commit(blk_tr, buffer, event, 0, pc);
	}
}

/*
 * Send out a notify for this process, if we haven't done so since a trace
 * started
 */
static void trace_note_tsk(struct task_struct *tsk)
{
	unsigned long flags;
	struct blk_trace *bt;

	tsk->btrace_seq = blktrace_seq;
	spin_lock_irqsave(&running_trace_lock, flags);
	list_for_each_entry(bt, &running_trace_list, running_list) {
		trace_note(bt, tsk->pid, BLK_TN_PROCESS, tsk->comm,
			   sizeof(tsk->comm), NULL);
	}
	spin_unlock_irqrestore(&running_trace_lock, flags);
}

static void trace_note_time(struct blk_trace *bt)
{
	struct timespec now;
	unsigned long flags;
	u32 words[2];

	getnstimeofday(&now);
	words[0] = now.tv_sec;
	words[1] = now.tv_nsec;

	local_irq_save(flags);
	trace_note(bt, 0, BLK_TN_TIMESTAMP, words, sizeof(words), NULL);
	local_irq_restore(flags);
}

void __trace_note_message(struct blk_trace *bt, struct blkcg *blkcg,
	const char *fmt, ...)
{
	int n;
	va_list args;
	unsigned long flags;
	char *buf;

	if (unlikely(bt->trace_state != Blktrace_running &&
		     !blk_tracer_enabled))
		return;

	/*
	 * If the BLK_TC_NOTIFY action mask isn't set, don't send any note
	 * message to the trace.
	 */
	if (!(bt->act_mask & BLK_TC_NOTIFY))
		return;

	local_irq_save(flags);
	buf = this_cpu_ptr(bt->msg_data);
	va_start(args, fmt);
	n = vscnprintf(buf, BLK_TN_MAX_MSG, fmt, args);
	va_end(args);

	if (!(blk_tracer_flags.val & TRACE_BLK_OPT_CGROUP))
		blkcg = NULL;
#ifdef CONFIG_BLK_CGROUP
	trace_note(bt, 0, BLK_TN_MESSAGE, buf, n,
		blkcg ? cgroup_get_kernfs_id(blkcg->css.cgroup) : NULL);
#else
	trace_note(bt, 0, BLK_TN_MESSAGE, buf, n, NULL);
#endif
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(__trace_note_message);

static int act_log_check(struct blk_trace *bt, u32 what, sector_t sector,
			 pid_t pid)
{
	if (((bt->act_mask << BLK_TC_SHIFT) & what) == 0)
		return 1;
	if (sector && (sector < bt->start_lba || sector > bt->end_lba))
		return 1;
	if (bt->pid && pid != bt->pid)
		return 1;

	return 0;
}

/*
 * Data direction bit lookup
 */
static const u32 ddir_act[2] = { BLK_TC_ACT(BLK_TC_READ),
				 BLK_TC_ACT(BLK_TC_WRITE) };

#define BLK_TC_RAHEAD		BLK_TC_AHEAD

/* The ilog2() calls fall out because they're constant */
#define MASK_TC_BIT(rw, __name) ((rw & REQ_ ## __name) << \
	  (ilog2(BLK_TC_ ## __name) + BLK_TC_SHIFT - __REQ_ ## __name))

/*
 * The worker for the various blk_add_trace*() types. Fills out a
 * blk_io_trace structure and places it in a per-cpu subbuffer.
 */
static void __blk_add_trace(struct blk_trace *bt, sector_t sector, int bytes,
				int rw, u32 what, int error, int pdu_len,
				void *pdu_data, struct task_struct *tsk,
				union kernfs_node_id *cgid)
{
	struct ring_buffer_event *event = NULL;
	struct ring_buffer *buffer = NULL;
	struct blk_io_trace *t;
	unsigned long flags = 0;
	unsigned long *sequence;
	pid_t pid;
	int cpu, pc = 0;
	bool blk_tracer = blk_tracer_enabled;
	ssize_t cgid_len = cgid ? sizeof(*cgid) : 0;

	if (unlikely(bt->trace_state != Blktrace_running && !blk_tracer))
		return;

	what |= ddir_act[rw & WRITE];
	what |= MASK_TC_BIT(rw, SYNC);
	what |= MASK_TC_BIT(rw, RAHEAD);
	what |= MASK_TC_BIT(rw, META);
	what |= MASK_TC_BIT(rw, DISCARD);
	what |= MASK_TC_BIT(rw, FLUSH);
	what |= MASK_TC_BIT(rw, FUA);
	if (cgid)
		what |= __BLK_TA_CGROUP;

	pid = tsk->pid;
	if (act_log_check(bt, what, sector, pid))
		return;
	cpu = raw_smp_processor_id();

	if (blk_tracer) {
		tracing_record_cmdline(current);

		buffer = blk_tr->trace_buffer.buffer;
		pc = preempt_count();
		event = trace_buffer_lock_reserve(buffer, TRACE_BLK,
						  sizeof(*t) + pdu_len + cgid_len,
						  0, pc);
		if (!event)
			return;
		t = ring_buffer_event_data(event);
		goto record_it;
	}

	if (unlikely(tsk->btrace_seq != blktrace_seq))
		trace_note_tsk(tsk);

	/*
	 * A word about the locking here - we disable interrupts to reserve
	 * some space in the relay per-cpu buffer, to prevent an irq
	 * from coming in and stepping on our toes.
	 */
	local_irq_save(flags);
	t = relay_reserve(bt->rchan, sizeof(*t) + pdu_len + cgid_len);
	if (t) {
		sequence = per_cpu_ptr(bt->sequence, cpu);

		t->magic = BLK_IO_TRACE_MAGIC | BLK_IO_TRACE_VERSION;
		t->sequence = ++(*sequence);
		t->time = ktime_to_ns(ktime_get());
record_it:
		/*
		 * These two are not needed in ftrace as they are in the
		 * generic trace_entry, filled by tracing_generic_entry_update,
		 * but for the trace_event->bin() synthesizer benefit we do it
		 * here too.
		 */
		t->cpu = cpu;
		t->pid = pid;

		t->sector = sector;
		t->bytes = bytes;
		t->action = what;
		t->device = bt->dev;
		t->error = error;
		t->pdu_len = pdu_len + cgid_len;

		if (cgid_len)
			memcpy((void *)t + sizeof(*t), cgid, cgid_len);
		if (pdu_len)
			memcpy((void *)t + sizeof(*t) + cgid_len, pdu_data, pdu_len);

		if (blk_tracer) {
			trace_buffer_unlock_commit(blk_tr, buffer, event, 0, pc);
			return;
		}
	}

	local_irq_restore(flags);
}

static struct dentry *blk_tree_root;
static DEFINE_MUTEX(blk_tree_mutex);

static void blk_trace_free(struct blk_trace *bt)
{
	debugfs_remove(bt->msg_file);
	debugfs_remove(bt->dropped_file);
	relay_close(bt->rchan);
	debugfs_remove(bt->dir);
	free_percpu(bt->sequence);
	free_percpu(bt->msg_data);
	kfree(bt);
}

static void get_probe_ref(void)
{
	mutex_lock(&blk_probe_mutex);
	if (++blk_probes_ref == 1)
		blk_register_tracepoints();
	mutex_unlock(&blk_probe_mutex);
}

static void put_probe_ref(void)
{
	mutex_lock(&blk_probe_mutex);
	if (!--blk_probes_ref)
		blk_unregister_tracepoints();
	mutex_unlock(&blk_probe_mutex);
}

static void blk_trace_cleanup(struct blk_trace *bt)
{
	synchronize_rcu();
	blk_trace_free(bt);
	put_probe_ref();
}

static int __blk_trace_remove(struct request_queue *q)
{
	struct blk_trace *bt;

	bt = xchg(&q->blk_trace, NULL);
	if (!bt)
		return -EINVAL;

	if (bt->trace_state != Blktrace_running)
		blk_trace_cleanup(bt);

	return 0;
}

int blk_trace_remove(struct request_queue *q)
{
	int ret;

	mutex_lock(&q->blk_trace_mutex);
	ret = __blk_trace_remove(q);
	mutex_unlock(&q->blk_trace_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(blk_trace_remove);

static ssize_t blk_dropped_read(struct file *filp, char __user *buffer,
				size_t count, loff_t *ppos)
{
	struct blk_trace *bt = filp->private_data;
	char buf[16];

	snprintf(buf, sizeof(buf), "%u\n", atomic_read(&bt->dropped));

	return simple_read_from_buffer(buffer, count, ppos, buf, strlen(buf));
}

static const struct file_operations blk_dropped_fops = {
	.owner =	THIS_MODULE,
	.open =		simple_open,
	.read =		blk_dropped_read,
	.llseek =	default_llseek,
};

static ssize_t blk_msg_write(struct file *filp, const char __user *buffer,
				size_t count, loff_t *ppos)
{
	char *msg;
	struct blk_trace *bt;

	if (count >= BLK_TN_MAX_MSG)
		return -EINVAL;

	msg = kmalloc(count + 1, GFP_KERNEL);
	if (msg == NULL)
		return -ENOMEM;

	if (copy_from_user(msg, buffer, count)) {
		kfree(msg);
		return -EFAULT;
	}

	msg[count] = '\0';
	bt = filp->private_data;
	__trace_note_message(bt, NULL, "%s", msg);
	kfree(msg);

	return count;
}

static const struct file_operations blk_msg_fops = {
	.owner =	THIS_MODULE,
	.open =		simple_open,
	.write =	blk_msg_write,
	.llseek =	noop_llseek,
};

/*
 * Keep track of how many times we encountered a full subbuffer, to aid
 * the user space app in telling how many lost events there were.
 */
static int blk_subbuf_start_callback(struct rchan_buf *buf, void *subbuf,
				     void *prev_subbuf, size_t prev_padding)
{
	struct blk_trace *bt;

	if (!relay_buf_full(buf))
		return 1;

	bt = buf->chan->private_data;
	atomic_inc(&bt->dropped);
	return 0;
}

static int blk_remove_buf_file_callback(struct dentry *dentry)
{
	debugfs_remove(dentry);

	return 0;
}

static struct dentry *blk_create_buf_file_callback(const char *filename,
						   struct dentry *parent,
						   umode_t mode,
						   struct rchan_buf *buf,
						   int *is_global)
{
	return debugfs_create_file(filename, mode, parent, buf,
					&relay_file_operations);
}

static struct rchan_callbacks blk_relay_callbacks = {
	.subbuf_start		= blk_subbuf_start_callback,
	.create_buf_file	= blk_create_buf_file_callback,
	.remove_buf_file	= blk_remove_buf_file_callback,
};

static void blk_trace_setup_lba(struct blk_trace *bt,
				struct block_device *bdev)
{
	struct hd_struct *part = NULL;

	if (bdev)
		part = bdev->bd_part;

	if (part) {
		bt->start_lba = part->start_sect;
		bt->end_lba = part->start_sect + part->nr_sects;
	} else {
		bt->start_lba = 0;
		bt->end_lba = -1ULL;
	}
}

/*
 * Setup everything required to start tracing
 */
int do_blk_trace_setup(struct request_queue *q, char *name, dev_t dev,
		       struct block_device *bdev,
		       struct blk_user_trace_setup *buts)
{
	struct blk_trace *bt = NULL;
	struct dentry *dir = NULL;
	int ret;

	if (!buts->buf_size || !buts->buf_nr)
		return -EINVAL;

	strncpy(buts->name, name, BLKTRACE_BDEV_SIZE);
	buts->name[BLKTRACE_BDEV_SIZE - 1] = '\0';

	/*
	 * some device names have larger paths - convert the slashes
	 * to underscores for this to work as expected
	 */
	strreplace(buts->name, '/', '_');

	/*
	 * bdev can be NULL, as with scsi-generic, this is a helpful as
	 * we can be.
	 */
	if (q->blk_trace) {
		pr_warn("Concurrent blktraces are not allowed on %s\n",
			buts->name);
		return -EBUSY;
	}

	bt = kzalloc(sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;

	ret = -ENOMEM;
	bt->sequence = alloc_percpu(unsigned long);
	if (!bt->sequence)
		goto err;

	bt->msg_data = __alloc_percpu(BLK_TN_MAX_MSG, __alignof__(char));
	if (!bt->msg_data)
		goto err;

	ret = -ENOENT;

	mutex_lock(&blk_tree_mutex);
	if (!blk_tree_root) {
		blk_tree_root = debugfs_create_dir("block", NULL);
		if (!blk_tree_root) {
			mutex_unlock(&blk_tree_mutex);
			goto err;
		}
	}
	mutex_unlock(&blk_tree_mutex);

	dir = debugfs_create_dir(buts->name, blk_tree_root);

	if (!dir)
		goto err;

	bt->dir = dir;
	bt->dev = dev;
	atomic_set(&bt->dropped, 0);
	INIT_LIST_HEAD(&bt->running_list);

	ret = -EIO;
	bt->dropped_file = debugfs_create_file("dropped", 0444, dir, bt,
					       &blk_dropped_fops);
	if (!bt->dropped_file)
		goto err;

	bt->msg_file = debugfs_create_file("msg", 0222, dir, bt, &blk_msg_fops);
	if (!bt->msg_file)
		goto err;

	bt->rchan = relay_open("trace", dir, buts->buf_size,
				buts->buf_nr, &blk_relay_callbacks, bt);
	if (!bt->rchan)
		goto err;

	bt->act_mask = buts->act_mask;
	if (!bt->act_mask)
		bt->act_mask = (u16) -1;

	blk_trace_setup_lba(bt, bdev);

	/* overwrite with user settings */
	if (buts->start_lba)
		bt->start_lba = buts->start_lba;
	if (buts->end_lba)
		bt->end_lba = buts->end_lba;

	bt->pid = buts->pid;
	bt->trace_state = Blktrace_setup;

	ret = -EBUSY;
	if (cmpxchg(&q->blk_trace, NULL, bt))
		goto err;

	get_probe_ref();

	return 0;
err:
	blk_trace_free(bt);
	return ret;
}

static int __blk_trace_setup(struct request_queue *q, char *name, dev_t dev,
			     struct block_device *bdev, char __user *arg)
{
	struct blk_user_trace_setup buts;
	int ret;

	ret = copy_from_user(&buts, arg, sizeof(buts));
	if (ret)
		return -EFAULT;

	ret = do_blk_trace_setup(q, name, dev, bdev, &buts);
	if (ret)
		return ret;

	if (copy_to_user(arg, &buts, sizeof(buts))) {
		__blk_trace_remove(q);
		return -EFAULT;
	}
	return 0;
}

int blk_trace_setup(struct request_queue *q, char *name, dev_t dev,
		    struct block_device *bdev,
		    char __user *arg)
{
	int ret;

	mutex_lock(&q->blk_trace_mutex);
	ret = __blk_trace_setup(q, name, dev, bdev, arg);
	mutex_unlock(&q->blk_trace_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(blk_trace_setup);

#if defined(CONFIG_COMPAT) && defined(CONFIG_X86_64)
static int compat_blk_trace_setup(struct request_queue *q, char *name,
				  dev_t dev, struct block_device *bdev,
				  char __user *arg)
{
	struct blk_user_trace_setup buts;
	struct compat_blk_user_trace_setup cbuts;
	int ret;

	if (copy_from_user(&cbuts, arg, sizeof(cbuts)))
		return -EFAULT;

	buts = (struct blk_user_trace_setup) {
		.act_mask = cbuts.act_mask,
		.buf_size = cbuts.buf_size,
		.buf_nr = cbuts.buf_nr,
		.start_lba = cbuts.start_lba,
		.end_lba = cbuts.end_lba,
		.pid = cbuts.pid,
	};

	ret = do_blk_trace_setup(q, name, dev, bdev, &buts);
	if (ret)
		return ret;

	if (copy_to_user(arg, &buts.name, ARRAY_SIZE(buts.name))) {
		__blk_trace_remove(q);
		return -EFAULT;
	}

	return 0;
}
#endif

static int __blk_trace_startstop(struct request_queue *q, int start)
{
	int ret;
	struct blk_trace *bt;

	bt = rcu_dereference_protected(q->blk_trace,
				       lockdep_is_held(&q->blk_trace_mutex));
	if (bt == NULL)
		return -EINVAL;

	/*
	 * For starting a trace, we can transition from a setup or stopped
	 * trace. For stopping a trace, the state must be running
	 */
	ret = -EINVAL;
	if (start) {
		if (bt->trace_state == Blktrace_setup ||
		    bt->trace_state == Blktrace_stopped) {
			blktrace_seq++;
			smp_mb();
			bt->trace_state = Blktrace_running;
			spin_lock_irq(&running_trace_lock);
			list_add(&bt->running_list, &running_trace_list);
			spin_unlock_irq(&running_trace_lock);

			trace_note_time(bt);
			ret = 0;
		}
	} else {
		if (bt->trace_state == Blktrace_running) {
			bt->trace_state = Blktrace_stopped;
			spin_lock_irq(&running_trace_lock);
			list_del_init(&bt->running_list);
			spin_unlock_irq(&running_trace_lock);
			relay_flush(bt->rchan);
			ret = 0;
		}
	}

	return ret;
}

int blk_trace_startstop(struct request_queue *q, int start)
{
	int ret;

	mutex_lock(&q->blk_trace_mutex);
	ret = __blk_trace_startstop(q, start);
	mutex_unlock(&q->blk_trace_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(blk_trace_startstop);

/*
 * When reading or writing the blktrace sysfs files, the references to the
 * opened sysfs or device files should prevent the underlying block device
 * from being removed. So no further delete protection is really needed.
 */

/**
 * blk_trace_ioctl: - handle the ioctls associated with tracing
 * @bdev:	the block device
 * @cmd:	the ioctl cmd
 * @arg:	the argument data, if any
 *
 **/
int blk_trace_ioctl(struct block_device *bdev, unsigned cmd, char __user *arg)
{
	struct request_queue *q;
	int ret, start = 0;
	char b[BDEVNAME_SIZE];

	q = bdev_get_queue(bdev);
	if (!q)
		return -ENXIO;

	mutex_lock(&q->blk_trace_mutex);

	switch (cmd) {
	case BLKTRACESETUP:
		bdevname(bdev, b);
		ret = __blk_trace_setup(q, b, bdev->bd_dev, bdev, arg);
		break;
#if defined(CONFIG_COMPAT) && defined(CONFIG_X86_64)
	case BLKTRACESETUP32:
		bdevname(bdev, b);
		ret = compat_blk_trace_setup(q, b, bdev->bd_dev, bdev, arg);
		break;
#endif
	case BLKTRACESTART:
		start = 1;
	case BLKTRACESTOP:
		ret = __blk_trace_startstop(q, start);
		break;
	case BLKTRACETEARDOWN:
		ret = __blk_trace_remove(q);
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&q->blk_trace_mutex);
	return ret;
}

/**
 * blk_trace_shutdown: - stop and cleanup trace structures
 * @q:    the request queue associated with the device
 *
 **/
void blk_trace_shutdown(struct request_queue *q)
{
	mutex_lock(&q->blk_trace_mutex);
	if (rcu_dereference_protected(q->blk_trace,
				      lockdep_is_held(&q->blk_trace_mutex))) {
		__blk_trace_startstop(q, 0);
		__blk_trace_remove(q);
	}

	mutex_unlock(&q->blk_trace_mutex);
}

#ifdef CONFIG_BLK_CGROUP
static union kernfs_node_id *
blk_trace_bio_get_cgid(struct request_queue *q, struct bio *bio)
{
	struct blk_trace *bt = q->blk_trace;

	if (!bt || !(blk_tracer_flags.val & TRACE_BLK_OPT_CGROUP))
		return NULL;

	if (!bio->bi_css)
		return NULL;
	return cgroup_get_kernfs_id(bio->bi_css->cgroup);
}
#else
static union kernfs_node_id *
blk_trace_bio_get_cgid(struct request_queue *q, struct bio *bio)
{
	return NULL;
}
#endif

static union kernfs_node_id *
blk_trace_request_get_cgid(struct request_queue *q, struct request *rq)
{
	if (!rq->bio)
		return NULL;
	/* Use the first bio */
	return blk_trace_bio_get_cgid(q, rq->bio);
}

/*
 * blktrace probes
 */

/**
 * blk_add_trace_rq - Add a trace for a request oriented action
 * @q:		queue the io is for
 * @rq:		the source request
 * @nr_bytes:	number of completed bytes
 * @what:	the action
 * @cgid:	the cgroup info
 *
 * Description:
 *     Records an action against a request. Will log the bio offset + size.
 *
 **/
static void blk_add_trace_rq(struct request_queue *q, struct request *rq,
			     unsigned int nr_bytes, u32 what,
			     union kernfs_node_id *cgid)
{
	struct blk_trace *bt;
	struct task_struct *tsk = current;

	rcu_read_lock();
	bt = rcu_dereference(q->blk_trace);
	if (likely(!bt)) {
		rcu_read_unlock();
		return;
	}

	/*
	 * Use the bio context for all events except ISSUE and
	 * COMPLETE events.
	 *
	 * Not all the pages in the bio are dirtied by the same task but
	 * most likely it will be, since the sectors accessed on the device
	 * must be adjacent.
	 */
	if (!((what == BLK_TA_ISSUE) || (what == BLK_TA_COMPLETE)) &&
	    bio_has_data(rq->bio) && rq->bio->bi_io_vec &&
	    rq->bio->bi_io_vec->bv_page &&
	    rq->bio->bi_io_vec->bv_page->tsk_dirty)
		tsk = rq->bio->bi_io_vec->bv_page->tsk_dirty;

	if (rq->cmd_type == REQ_TYPE_BLOCK_PC) {
		what |= BLK_TC_ACT(BLK_TC_PC);
		__blk_add_trace(bt, 0, nr_bytes, rq->cmd_flags,
				what, rq->errors, rq->cmd_len, rq->cmd, tsk);
	} else  {
		what |= BLK_TC_ACT(BLK_TC_FS);
		__blk_add_trace(bt, blk_rq_pos(rq), nr_bytes,
				rq->cmd_flags, what, rq->errors, 0, NULL, tsk);
	}
	rcu_read_unlock();
}

static void blk_add_trace_rq_abort(void *ignore,
				   struct request_queue *q, struct request *rq)
{
	blk_add_trace_rq(q, rq, blk_rq_bytes(rq), BLK_TA_ABORT,
			 blk_trace_request_get_cgid(q, rq));
}

static void blk_add_trace_rq_insert(void *ignore,
				    struct request_queue *q, struct request *rq)
{
	blk_add_trace_rq(q, rq, blk_rq_bytes(rq), BLK_TA_INSERT,
			 blk_trace_request_get_cgid(q, rq));
}

static void blk_add_trace_rq_issue(void *ignore,
				   struct request_queue *q, struct request *rq)
{
	blk_add_trace_rq(q, rq, blk_rq_bytes(rq), BLK_TA_ISSUE,
			 blk_trace_request_get_cgid(q, rq));
}

static void blk_add_trace_rq_requeue(void *ignore,
				     struct request_queue *q,
				     struct request *rq)
{
	blk_add_trace_rq(q, rq, blk_rq_bytes(rq), BLK_TA_REQUEUE,
			 blk_trace_request_get_cgid(q, rq));
}

static void blk_add_trace_rq_complete(void *ignore,
				      struct request_queue *q,
				      struct request *rq,
				      unsigned int nr_bytes)
{
	blk_add_trace_rq(q, rq, nr_bytes, BLK_TA_COMPLETE,
			 blk_trace_request_get_cgid(q, rq));
}

/**
 * blk_add_trace_bio - Add a trace for a bio oriented action
 * @q:		queue the io is for
 * @bio:	the source bio
 * @what:	the action
 * @error:	error, if any
 *
 * Description:
 *     Records an action against a bio. Will log the bio offset + size.
 *
 **/
static void blk_add_trace_bio(struct request_queue *q, struct bio *bio,
			      u32 what, int error, union kernfs_node_id *cgid)
{
	struct blk_trace *bt;
	struct task_struct *tsk = current;

	rcu_read_lock();
	bt = rcu_dereference(q->blk_trace);
	if (likely(!bt)) {
		rcu_read_unlock();
		return;
	}

	/*
	 * Not all the pages in the bio are dirtied by the same task but
	 * most likely it will be, since the sectors accessed on the device
	 * must be adjacent.
	 */
	if (bio_has_data(bio) && bio->bi_io_vec && bio->bi_io_vec->bv_page &&
	    bio->bi_io_vec->bv_page->tsk_dirty)
		tsk = bio->bi_io_vec->bv_page->tsk_dirty;

	__blk_add_trace(bt, bio->bi_iter.bi_sector, bio->bi_iter.bi_size,
			bio->bi_rw, what, error, 0, NULL, tsk, cgid);
	rcu_read_unlock();
}

static void blk_add_trace_bio_bounce(void *ignore,
				     struct request_queue *q, struct bio *bio)
{
	blk_add_trace_bio(q, bio, BLK_TA_BOUNCE, 0,
			  blk_trace_bio_get_cgid(q, bio));
}

static void blk_add_trace_bio_complete(void *ignore,
				       struct request_queue *q, struct bio *bio,
				       int error)
{
	blk_add_trace_bio(q, bio, BLK_TA_COMPLETE, error,
			  blk_trace_bio_get_cgid(q, bio));
}

static void blk_add_trace_bio_backmerge(void *ignore,
					struct request_queue *q,
					struct request *rq,
					struct bio *bio)
{
	blk_add_trace_bio(q, bio, BLK_TA_BACKMERGE, 0,
			 blk_trace_bio_get_cgid(q, bio));
}

static void blk_add_trace_bio_frontmerge(void *ignore,
					 struct request_queue *q,
					 struct request *rq,
					 struct bio *bio)
{
	blk_add_trace_bio(q, bio, BLK_TA_FRONTMERGE, 0,
			  blk_trace_bio_get_cgid(q, bio));
}

static void blk_add_trace_bio_queue(void *ignore,
				    struct request_queue *q, struct bio *bio)
{
	blk_add_trace_bio(q, bio, BLK_TA_QUEUE, 0,
			  blk_trace_bio_get_cgid(q, bio));
}

static void blk_add_trace_getrq(void *ignore,
				struct request_queue *q,
				struct bio *bio, int rw)
{
	if (bio)
		blk_add_trace_bio(q, bio, BLK_TA_GETRQ, 0,
				  blk_trace_bio_get_cgid(q, bio));
	else {
		struct blk_trace *bt;

		rcu_read_lock();
		bt = rcu_dereference(q->blk_trace);
		if (bt)
			__blk_add_trace(bt, 0, 0, rw, BLK_TA_GETRQ, 0, 0,
					NULL, current, NULL);
		rcu_read_unlock();
	}
}


static void blk_add_trace_sleeprq(void *ignore,
				  struct request_queue *q,
				  struct bio *bio, int rw)
{
	if (bio)
		blk_add_trace_bio(q, bio, BLK_TA_SLEEPRQ, 0,
				  blk_trace_bio_get_cgid(q, bio));
	else {
		struct blk_trace *bt;

		rcu_read_lock();
		bt = rcu_dereference(q->blk_trace);
		if (bt)
			__blk_add_trace(bt, 0, 0, rw, BLK_TA_SLEEPRQ,
					0, 0, NULL, current, NULL);
		rcu_read_unlock();
	}
}

static void blk_add_trace_plug(void *ignore, struct request_queue *q)
{
	struct blk_trace *bt;

	rcu_read_lock();
	bt = rcu_dereference(q->blk_trace);
	if (bt)
		__blk_add_trace(bt, 0, 0, 0, BLK_TA_PLUG, 0, 0, NULL,
				current, NULL);
	rcu_read_unlock();
}

static void blk_add_trace_unplug(void *ignore, struct request_queue *q,
				    unsigned int depth, bool explicit)
{
	struct blk_trace *bt;

	rcu_read_lock();
	bt = rcu_dereference(q->blk_trace);
	if (bt) {
		__be64 rpdu = cpu_to_be64(depth);
		u32 what;

		if (explicit)
			what = BLK_TA_UNPLUG_IO;
		else
			what = BLK_TA_UNPLUG_TIMER;

		__blk_add_trace(bt, 0, 0, 0, what, 0, sizeof(rpdu), &rpdu,
				current, NULL);
	}
	rcu_read_unlock();
}

static void blk_add_trace_split(void *ignore,
				struct request_queue *q, struct bio *bio,
				unsigned int pdu)
{
	struct blk_trace *bt;
	struct task_struct *tsk = current;

	rcu_read_lock();
	bt = rcu_dereference(q->blk_trace);
	if (bt) {
		__be64 rpdu = cpu_to_be64(pdu);

		if (bio_has_data(bio) && bio->bi_io_vec &&
		    bio->bi_io_vec->bv_page &&
		    bio->bi_io_vec->bv_page->tsk_dirty)
			tsk = bio->bi_io_vec->bv_page->tsk_dirty;

		__blk_add_trace(bt, bio->bi_iter.bi_sector,
				bio->bi_iter.bi_size, bio->bi_rw, BLK_TA_SPLIT,
				bio->bi_error, sizeof(rpdu), &rpdu, tsk,
				blk_trace_bio_get_cgid(q, bio));
	}
	rcu_read_unlock();
}

/**
 * blk_add_trace_bio_remap - Add a trace for a bio-remap operation
 * @ignore:	trace callback data parameter (not used)
 * @q:		queue the io is for
 * @bio:	the source bio
 * @dev:	target device
 * @from:	source sector
 *
 * Description:
 *     Device mapper or raid target sometimes need to split a bio because
 *     it spans a stripe (or similar). Add a trace for that action.
 *
 **/
static void blk_add_trace_bio_remap(void *ignore,
				    struct request_queue *q, struct bio *bio,
				    dev_t dev, sector_t from)
{
	struct blk_trace *bt;
	struct blk_io_trace_remap r;
	struct task_struct *tsk = current;

	rcu_read_lock();
	bt = rcu_dereference(q->blk_trace);
	if (likely(!bt)) {
		rcu_read_unlock();
		return;
	}

	r.device_from = cpu_to_be32(dev);
	r.device_to   = cpu_to_be32(bio->bi_bdev->bd_dev);
	r.sector_from = cpu_to_be64(from);

	if (bio_has_data(bio) && bio->bi_io_vec &&
	    bio->bi_io_vec->bv_page &&
	    bio->bi_io_vec->bv_page->tsk_dirty)
		tsk = bio->bi_io_vec->bv_page->tsk_dirty;

	__blk_add_trace(bt, bio->bi_iter.bi_sector, bio->bi_iter.bi_size,
			bio->bi_rw, BLK_TA_REMAP, bio->bi_error,
			sizeof(r), &r, tsk, blk_trace_bio_get_cgid(q, bio));
	rcu_read_unlock();
}

/**
 * blk_add_trace_rq_remap - Add a trace for a request-remap operation
 * @ignore:	trace callback data parameter (not used)
 * @q:		queue the io is for
 * @rq:		the source request
 * @dev:	target device
 * @from:	source sector
 *
 * Description:
 *     Device mapper remaps request to other devices.
 *     Add a trace for that action.
 *
 **/
static void blk_add_trace_rq_remap(void *ignore,
				   struct request_queue *q,
				   struct request *rq, dev_t dev,
				   sector_t from)
{
	struct blk_trace *bt;
	struct blk_io_trace_remap r;
	struct task_struct *tsk = current;

	rcu_read_lock();
	bt = rcu_dereference(q->blk_trace);
	if (likely(!bt)) {
		rcu_read_unlock();
		return;
	}

	r.device_from = cpu_to_be32(dev);
	r.device_to   = cpu_to_be32(disk_devt(rq->rq_disk));
	r.sector_from = cpu_to_be64(from);

	if (bio_has_data(rq->bio) && rq->bio->bi_io_vec &&
	    rq->bio->bi_io_vec->bv_page &&
	    rq->bio->bi_io_vec->bv_page->tsk_dirty)
		tsk = rq->bio->bi_io_vec->bv_page->tsk_dirty;

	__blk_add_trace(bt, blk_rq_pos(rq), blk_rq_bytes(rq),
			rq_data_dir(rq), BLK_TA_REMAP, !!rq->errors,
			sizeof(r), &r, tsk, blk_trace_request_get_cgid(q, rq));
	rcu_read_unlock();
}

/**
 * blk_add_driver_data - Add binary message with driver-specific data
 * @q:		queue the io is for
 * @rq:		io request
 * @data:	driver-specific data
 * @len:	length of driver-specific data
 *
 * Description:
 *     Some drivers might want to write driver-specific data per request.
 *
 **/
void blk_add_driver_data(struct request_queue *q,
			 struct request *rq,
			 void *data, size_t len)
{
	struct blk_trace *bt;
	struct task_struct *tsk = current;

	rcu_read_lock();
	bt = rcu_dereference(q->blk_trace);
	if (likely(!bt)) {
		rcu_read_unlock();
		return;
	}

	if (bio_has_data(rq->bio) && rq->bio->bi_io_vec &&
	    rq->bio->bi_io_vec->bv_page &&
	    rq->bio->bi_io_vec->bv_page->tsk_dirty)
		tsk = rq->bio->bi_io_vec->bv_page->tsk_dirty;

	if (rq->cmd_type == REQ_TYPE_BLOCK_PC)
		__blk_add_trace(bt, 0, blk_rq_bytes(rq), 0,
				BLK_TA_DRV_DATA, rq->errors, len, data, tsk,
				blk_trace_request_get_cgid(q, rq));
	else
		__blk_add_trace(bt, blk_rq_pos(rq), blk_rq_bytes(rq), 0,
				BLK_TA_DRV_DATA, rq->errors, len, data, tsk,
				blk_trace_request_get_cgid(q, rq));
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(blk_add_driver_data);

static void blk_register_tracepoints(void)
{
	int ret;

	ret = register_trace_block_rq_abort(blk_add_trace_rq_abort, NULL);
	WARN_ON(ret);
	ret = register_trace_block_rq_insert(blk_add_trace_rq_insert, NULL);
	WARN_ON(ret);
	ret = register_trace_block_rq_issue(blk_add_trace_rq_issue, NULL);
	WARN_ON(ret);
	ret = register_trace_block_rq_requeue(blk_add_trace_rq_requeue, NULL);
	WARN_ON(ret);
	ret = register_trace_block_rq_complete(blk_add_trace_rq_complete, NULL);
	WARN_ON(ret);
	ret = register_trace_block_bio_bounce(blk_add_trace_bio_bounce, NULL);
	WARN_ON(ret);
	ret = register_trace_block_bio_complete(blk_add_trace_bio_complete, NULL);
	WARN_ON(ret);
	ret = register_trace_block_bio_backmerge(blk_add_trace_bio_backmerge, NULL);
	WARN_ON(ret);
	ret = register_trace_block_bio_frontmerge(blk_add_trace_bio_frontmerge, NULL);
	WARN_ON(ret);
	ret = register_trace_block_bio_queue(blk_add_trace_bio_queue, NULL);
	WARN_ON(ret);
	ret = register_trace_block_getrq(blk_add_trace_getrq, NULL);
	WARN_ON(ret);
	ret = register_trace_block_sleeprq(blk_add_trace_sleeprq, NULL);
	WARN_ON(ret);
	ret = register_trace_block_plug(blk_add_trace_plug, NULL);
	WARN_ON(ret);
	ret = register_trace_block_unplug(blk_add_trace_unplug, NULL);
	WARN_ON(ret);
	ret = register_trace_block_split(blk_add_trace_split, NULL);
	WARN_ON(ret);
	ret = register_trace_block_bio_remap(blk_add_trace_bio_remap, NULL);
	WARN_ON(ret);
	ret = register_trace_block_rq_remap(blk_add_trace_rq_remap, NULL);
	WARN_ON(ret);
}

static void blk_unregister_tracepoints(void)
{
	unregister_trace_block_rq_remap(blk_add_trace_rq_remap, NULL);
	unregister_trace_block_bio_remap(blk_add_trace_bio_remap, NULL);
	unregister_trace_block_split(blk_add_trace_split, NULL);
	unregister_trace_block_unplug(blk_add_trace_unplug, NULL);
	unregister_trace_block_plug(blk_add_trace_plug, NULL);
	unregister_trace_block_sleeprq(blk_add_trace_sleeprq, NULL);
	unregister_trace_block_getrq(blk_add_trace_getrq, NULL);
	unregister_trace_block_bio_queue(blk_add_trace_bio_queue, NULL);
	unregister_trace_block_bio_frontmerge(blk_add_trace_bio_frontmerge, NULL);
	unregister_trace_block_bio_backmerge(blk_add_trace_bio_backmerge, NULL);
	unregister_trace_block_bio_complete(blk_add_trace_bio_complete, NULL);
	unregister_trace_block_bio_bounce(blk_add_trace_bio_bounce, NULL);
	unregister_trace_block_rq_complete(blk_add_trace_rq_complete, NULL);
	unregister_trace_block_rq_requeue(blk_add_trace_rq_requeue, NULL);
	unregister_trace_block_rq_issue(blk_add_trace_rq_issue, NULL);
	unregister_trace_block_rq_insert(blk_add_trace_rq_insert, NULL);
	unregister_trace_block_rq_abort(blk_add_trace_rq_abort, NULL);

	tracepoint_synchronize_unregister();
}

/*
 * struct blk_io_tracer formatting routines
 */

static void fill_rwbs(char *rwbs, const struct blk_io_trace *t)
{
	int i = 0;
	int tc = t->action >> BLK_TC_SHIFT;

	if ((t->action & ~__BLK_TN_CGROUP) == BLK_TN_MESSAGE) {
		rwbs[i++] = 'N';
		goto out;
	}

	if (tc & BLK_TC_FLUSH)
		rwbs[i++] = 'F';

	if (tc & BLK_TC_DISCARD)
		rwbs[i++] = 'D';
	else if (tc & BLK_TC_WRITE)
		rwbs[i++] = 'W';
	else if (t->bytes)
		rwbs[i++] = 'R';
	else
		rwbs[i++] = 'N';

	if (tc & BLK_TC_FUA)
		rwbs[i++] = 'F';
	if (tc & BLK_TC_AHEAD)
		rwbs[i++] = 'A';
	if (tc & BLK_TC_SYNC)
		rwbs[i++] = 'S';
	if (tc & BLK_TC_META)
		rwbs[i++] = 'M';
out:
	rwbs[i] = '\0';
}

static inline
const struct blk_io_trace *te_blk_io_trace(const struct trace_entry *ent)
{
	return (const struct blk_io_trace *)ent;
}

static inline const void *pdu_start(const struct trace_entry *ent, bool has_cg)
{
	return (void *)(te_blk_io_trace(ent) + 1) +
		(has_cg ? sizeof(union kernfs_node_id) : 0);
}

static inline const void *cgid_start(const struct trace_entry *ent)
{
	return (void *)(te_blk_io_trace(ent) + 1);
}

static inline int pdu_real_len(const struct trace_entry *ent, bool has_cg)
{
	return te_blk_io_trace(ent)->pdu_len -
			(has_cg ? sizeof(union kernfs_node_id) : 0);
}

static inline u32 t_action(const struct trace_entry *ent)
{
	return te_blk_io_trace(ent)->action;
}

static inline u32 t_bytes(const struct trace_entry *ent)
{
	return te_blk_io_trace(ent)->bytes;
}

static inline u32 t_sec(const struct trace_entry *ent)
{
	return te_blk_io_trace(ent)->bytes >> 9;
}

static inline unsigned long long t_sector(const struct trace_entry *ent)
{
	return te_blk_io_trace(ent)->sector;
}

static inline __u16 t_error(const struct trace_entry *ent)
{
	return te_blk_io_trace(ent)->error;
}

static __u64 get_pdu_int(const struct trace_entry *ent, bool has_cg)
{
	const __u64 *val = pdu_start(ent, has_cg);
	return be64_to_cpu(*val);
}

static void get_pdu_remap(const struct trace_entry *ent,
			  struct blk_io_trace_remap *r, bool has_cg)
{
	const struct blk_io_trace_remap *__r = pdu_start(ent, has_cg);
	__u64 sector_from = __r->sector_from;

	r->device_from = be32_to_cpu(__r->device_from);
	r->device_to   = be32_to_cpu(__r->device_to);
	r->sector_from = be64_to_cpu(sector_from);
}

typedef void (blk_log_action_t) (struct trace_iterator *iter, const char *act,
	bool has_cg);

static void blk_log_action_classic(struct trace_iterator *iter, const char *act,
	bool has_cg)
{
	char rwbs[RWBS_LEN];
	unsigned long long ts  = iter->ts;
	unsigned long nsec_rem = do_div(ts, NSEC_PER_SEC);
	unsigned secs	       = (unsigned long)ts;
	const struct blk_io_trace *t = te_blk_io_trace(iter->ent);

	fill_rwbs(rwbs, t);

	trace_seq_printf(&iter->seq,
			 "%3d,%-3d %2d %5d.%09lu %5u %2s %3s ",
			 MAJOR(t->device), MINOR(t->device), iter->cpu,
			 secs, nsec_rem, iter->ent->pid, act, rwbs);
}

static void blk_log_action(struct trace_iterator *iter, const char *act,
	bool has_cg)
{
	char rwbs[RWBS_LEN];
	const struct blk_io_trace *t = te_blk_io_trace(iter->ent);

	fill_rwbs(rwbs, t);
	if (has_cg) {
		const union kernfs_node_id *id = cgid_start(iter->ent);

		if (blk_tracer_flags.val & TRACE_BLK_OPT_CGNAME) {
			char blkcg_name_buf[NAME_MAX + 1] = "<...>";

			cgroup_path_from_kernfs_id(id, blkcg_name_buf,
				sizeof(blkcg_name_buf));
			trace_seq_printf(&iter->seq, "%3d,%-3d %s %2s %3s ",
				 MAJOR(t->device), MINOR(t->device),
				 blkcg_name_buf, act, rwbs);
		} else
			trace_seq_printf(&iter->seq,
				 "%3d,%-3d %x,%-x %2s %3s ",
				 MAJOR(t->device), MINOR(t->device),
				 id->ino, id->generation, act, rwbs);
	} else
		trace_seq_printf(&iter->seq, "%3d,%-3d %2s %3s ",
				 MAJOR(t->device), MINOR(t->device), act, rwbs);
}

static void blk_log_dump_pdu(struct trace_seq *s,
	const struct trace_entry *ent, bool has_cg)
{
	const unsigned char *pdu_buf;
	int pdu_len;
	int i, end;

	pdu_buf = pdu_start(ent, has_cg);
	pdu_len = pdu_real_len(ent, has_cg);

	if (!pdu_len)
		return;

	/* find the last zero that needs to be printed */
	for (end = pdu_len - 1; end >= 0; end--)
		if (pdu_buf[end])
			break;
	end++;

	trace_seq_putc(s, '(');

	for (i = 0; i < pdu_len; i++) {

		trace_seq_printf(s, "%s%02x",
				 i == 0 ? "" : " ", pdu_buf[i]);

		/*
		 * stop when the rest is just zeroes and indicate so
		 * with a ".." appended
		 */
		if (i == end && end != pdu_len - 1) {
			trace_seq_puts(s, " ..) ");
			return;
		}
	}

	trace_seq_puts(s, ") ");
}

static void blk_log_generic(struct trace_seq *s, const struct trace_entry *ent, bool has_cg)
{
	char cmd[TASK_COMM_LEN];

	trace_find_cmdline(ent->pid, cmd);

	if (t_action(ent) & BLK_TC_ACT(BLK_TC_PC)) {
		trace_seq_printf(s, "%u ", t_bytes(ent));
		blk_log_dump_pdu(s, ent, has_cg);
		trace_seq_printf(s, "[%s]\n", cmd);
	} else {
		if (t_sec(ent))
			trace_seq_printf(s, "%llu + %u [%s]\n",
						t_sector(ent), t_sec(ent), cmd);
		else
			trace_seq_printf(s, "[%s]\n", cmd);
	}
}

static void blk_log_with_error(struct trace_seq *s,
			      const struct trace_entry *ent, bool has_cg)
{
	if (t_action(ent) & BLK_TC_ACT(BLK_TC_PC)) {
		blk_log_dump_pdu(s, ent, has_cg);
		trace_seq_printf(s, "[%d]\n", t_error(ent));
	} else {
		if (t_sec(ent))
			trace_seq_printf(s, "%llu + %u [%d]\n",
					 t_sector(ent),
					 t_sec(ent), t_error(ent));
		else
			trace_seq_printf(s, "%llu [%d]\n",
					 t_sector(ent), t_error(ent));
	}
}

static void blk_log_remap(struct trace_seq *s, const struct trace_entry *ent, bool has_cg)
{
	struct blk_io_trace_remap r = { .device_from = 0, };

	get_pdu_remap(ent, &r, has_cg);
	trace_seq_printf(s, "%llu + %u <- (%d,%d) %llu\n",
			 t_sector(ent), t_sec(ent),
			 MAJOR(r.device_from), MINOR(r.device_from),
			 (unsigned long long)r.sector_from);
}

static void blk_log_plug(struct trace_seq *s, const struct trace_entry *ent, bool has_cg)
{
	char cmd[TASK_COMM_LEN];

	trace_find_cmdline(ent->pid, cmd);

	trace_seq_printf(s, "[%s]\n", cmd);
}

static void blk_log_unplug(struct trace_seq *s, const struct trace_entry *ent, bool has_cg)
{
	char cmd[TASK_COMM_LEN];

	trace_find_cmdline(ent->pid, cmd);

	trace_seq_printf(s, "[%s] %llu\n", cmd, get_pdu_int(ent, has_cg));
}

static void blk_log_split(struct trace_seq *s, const struct trace_entry *ent, bool has_cg)
{
	char cmd[TASK_COMM_LEN];

	trace_find_cmdline(ent->pid, cmd);

	trace_seq_printf(s, "%llu / %llu [%s]\n", t_sector(ent),
			 get_pdu_int(ent, has_cg), cmd);
}

static void blk_log_msg(struct trace_seq *s, const struct trace_entry *ent,
			bool has_cg)
{

	trace_seq_putmem(s, pdu_start(ent, has_cg),
		pdu_real_len(ent, has_cg));
	trace_seq_putc(s, '\n');
}

/*
 * struct tracer operations
 */

static void blk_tracer_print_header(struct seq_file *m)
{
	if (!(blk_tracer_flags.val & TRACE_BLK_OPT_CLASSIC))
		return;
	seq_puts(m, "# DEV   CPU TIMESTAMP     PID ACT FLG\n"
		    "#  |     |     |           |   |   |\n");
}

static void blk_tracer_start(struct trace_array *tr)
{
	blk_tracer_enabled = true;
}

static int blk_tracer_init(struct trace_array *tr)
{
	blk_tr = tr;
	blk_tracer_start(tr);
	return 0;
}

static void blk_tracer_stop(struct trace_array *tr)
{
	blk_tracer_enabled = false;
}

static void blk_tracer_reset(struct trace_array *tr)
{
	blk_tracer_stop(tr);
}

static const struct {
	const char *act[2];
	void	   (*print)(struct trace_seq *s, const struct trace_entry *ent,
			    bool has_cg);
} what2act[] = {
	[__BLK_TA_QUEUE]	= {{  "Q", "queue" },	   blk_log_generic },
	[__BLK_TA_BACKMERGE]	= {{  "M", "backmerge" },  blk_log_generic },
	[__BLK_TA_FRONTMERGE]	= {{  "F", "frontmerge" }, blk_log_generic },
	[__BLK_TA_GETRQ]	= {{  "G", "getrq" },	   blk_log_generic },
	[__BLK_TA_SLEEPRQ]	= {{  "S", "sleeprq" },	   blk_log_generic },
	[__BLK_TA_REQUEUE]	= {{  "R", "requeue" },	   blk_log_with_error },
	[__BLK_TA_ISSUE]	= {{  "D", "issue" },	   blk_log_generic },
	[__BLK_TA_COMPLETE]	= {{  "C", "complete" },   blk_log_with_error },
	[__BLK_TA_PLUG]		= {{  "P", "plug" },	   blk_log_plug },
	[__BLK_TA_UNPLUG_IO]	= {{  "U", "unplug_io" },  blk_log_unplug },
	[__BLK_TA_UNPLUG_TIMER]	= {{ "UT", "unplug_timer" }, blk_log_unplug },
	[__BLK_TA_INSERT]	= {{  "I", "insert" },	   blk_log_generic },
	[__BLK_TA_SPLIT]	= {{  "X", "split" },	   blk_log_split },
	[__BLK_TA_BOUNCE]	= {{  "B", "bounce" },	   blk_log_generic },
	[__BLK_TA_REMAP]	= {{  "A", "remap" },	   blk_log_remap },
};

static enum print_line_t print_one_line(struct trace_iterator *iter,
					bool classic)
{
	struct trace_array *tr = iter->tr;
	struct trace_seq *s = &iter->seq;
	const struct blk_io_trace *t;
	u16 what;
	bool long_act;
	blk_log_action_t *log_action;
	bool has_cg;

	t	   = te_blk_io_trace(iter->ent);
	what	   = (t->action & ((1 << BLK_TC_SHIFT) - 1)) & ~__BLK_TA_CGROUP;
	long_act   = !!(tr->trace_flags & TRACE_ITER_VERBOSE);
	log_action = classic ? &blk_log_action_classic : &blk_log_action;
	has_cg	   = t->action & __BLK_TA_CGROUP;

	if ((t->action & ~__BLK_TN_CGROUP) == BLK_TN_MESSAGE) {
		log_action(iter, long_act ? "message" : "m", has_cg);
		blk_log_msg(s, iter->ent, has_cg);
	}

	if (unlikely(what == 0 || what >= ARRAY_SIZE(what2act)))
		trace_seq_printf(s, "Unknown action %x\n", what);
	else {
		log_action(iter, what2act[what].act[long_act], has_cg);
		what2act[what].print(s, iter->ent, has_cg);
	}

	return trace_handle_return(s);
}

static enum print_line_t blk_trace_event_print(struct trace_iterator *iter,
					       int flags, struct trace_event *event)
{
	return print_one_line(iter, false);
}

static void blk_trace_synthesize_old_trace(struct trace_iterator *iter)
{
	struct trace_seq *s = &iter->seq;
	struct blk_io_trace *t = (struct blk_io_trace *)iter->ent;
	const int offset = offsetof(struct blk_io_trace, sector);
	struct blk_io_trace old = {
		.magic	  = BLK_IO_TRACE_MAGIC | BLK_IO_TRACE_VERSION,
		.time     = iter->ts,
	};

	trace_seq_putmem(s, &old, offset);
	trace_seq_putmem(s, &t->sector,
			 sizeof(old) - offset + t->pdu_len);
}

static enum print_line_t
blk_trace_event_print_binary(struct trace_iterator *iter, int flags,
			     struct trace_event *event)
{
	blk_trace_synthesize_old_trace(iter);

	return trace_handle_return(&iter->seq);
}

static enum print_line_t blk_tracer_print_line(struct trace_iterator *iter)
{
	if ((iter->ent->type != TRACE_BLK) ||
	    !(blk_tracer_flags.val & TRACE_BLK_OPT_CLASSIC))
		return TRACE_TYPE_UNHANDLED;

	return print_one_line(iter, true);
}

static int
blk_tracer_set_flag(struct trace_array *tr, u32 old_flags, u32 bit, int set)
{
	/* don't output context-info for blk_classic output */
	if (bit == TRACE_BLK_OPT_CLASSIC) {
		if (set)
			tr->trace_flags &= ~TRACE_ITER_CONTEXT_INFO;
		else
			tr->trace_flags |= TRACE_ITER_CONTEXT_INFO;
	}
	return 0;
}

static struct tracer blk_tracer __read_mostly = {
	.name		= "blk",
	.init		= blk_tracer_init,
	.reset		= blk_tracer_reset,
	.start		= blk_tracer_start,
	.stop		= blk_tracer_stop,
	.print_header	= blk_tracer_print_header,
	.print_line	= blk_tracer_print_line,
	.flags		= &blk_tracer_flags,
	.set_flag	= blk_tracer_set_flag,
};

static struct trace_event_functions trace_blk_event_funcs = {
	.trace		= blk_trace_event_print,
	.binary		= blk_trace_event_print_binary,
};

static struct trace_event trace_blk_event = {
	.type		= TRACE_BLK,
	.funcs		= &trace_blk_event_funcs,
};

static int __init init_blk_tracer(void)
{
	if (!register_trace_event(&trace_blk_event)) {
		pr_warning("Warning: could not register block events\n");
		return 1;
	}

	if (register_tracer(&blk_tracer) != 0) {
		pr_warning("Warning: could not register the block tracer\n");
		unregister_trace_event(&trace_blk_event);
		return 1;
	}

	return 0;
}

device_initcall(init_blk_tracer);

static int blk_trace_remove_queue(struct request_queue *q)
{
	struct blk_trace *bt;

	bt = xchg(&q->blk_trace, NULL);
	if (bt == NULL)
		return -EINVAL;

	if (bt->trace_state == Blktrace_running) {
		bt->trace_state = Blktrace_stopped;
		spin_lock_irq(&running_trace_lock);
		list_del_init(&bt->running_list);
		spin_unlock_irq(&running_trace_lock);
		relay_flush(bt->rchan);
	}

	put_probe_ref();
	synchronize_rcu();
	blk_trace_free(bt);
	return 0;
}

/*
 * Setup everything required to start tracing
 */
static int blk_trace_setup_queue(struct request_queue *q,
				 struct block_device *bdev)
{
	struct blk_trace *bt = NULL;
	int ret = -ENOMEM;

	bt = kzalloc(sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;

	bt->msg_data = __alloc_percpu(BLK_TN_MAX_MSG, __alignof__(char));
	if (!bt->msg_data)
		goto free_bt;

	bt->dev = bdev->bd_dev;
	bt->act_mask = (u16)-1;

	blk_trace_setup_lba(bt, bdev);

	ret = -EBUSY;
	if (cmpxchg(&q->blk_trace, NULL, bt))
		goto free_bt;

	get_probe_ref();
	return 0;

free_bt:
	blk_trace_free(bt);
	return ret;
}

/*
 * sysfs interface to enable and configure tracing
 */

static ssize_t sysfs_blk_trace_attr_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf);
static ssize_t sysfs_blk_trace_attr_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count);
#define BLK_TRACE_DEVICE_ATTR(_name) \
	DEVICE_ATTR(_name, S_IRUGO | S_IWUSR, \
		    sysfs_blk_trace_attr_show, \
		    sysfs_blk_trace_attr_store)

static BLK_TRACE_DEVICE_ATTR(enable);
static BLK_TRACE_DEVICE_ATTR(act_mask);
static BLK_TRACE_DEVICE_ATTR(pid);
static BLK_TRACE_DEVICE_ATTR(start_lba);
static BLK_TRACE_DEVICE_ATTR(end_lba);

static struct attribute *blk_trace_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_act_mask.attr,
	&dev_attr_pid.attr,
	&dev_attr_start_lba.attr,
	&dev_attr_end_lba.attr,
	NULL
};

struct attribute_group blk_trace_attr_group = {
	.name  = "trace",
	.attrs = blk_trace_attrs,
};

static const struct {
	int mask;
	const char *str;
} mask_maps[] = {
	{ BLK_TC_READ,		"read"		},
	{ BLK_TC_WRITE,		"write"		},
	{ BLK_TC_FLUSH,		"flush"		},
	{ BLK_TC_SYNC,		"sync"		},
	{ BLK_TC_QUEUE,		"queue"		},
	{ BLK_TC_REQUEUE,	"requeue"	},
	{ BLK_TC_ISSUE,		"issue"		},
	{ BLK_TC_COMPLETE,	"complete"	},
	{ BLK_TC_FS,		"fs"		},
	{ BLK_TC_PC,		"pc"		},
	{ BLK_TC_AHEAD,		"ahead"		},
	{ BLK_TC_META,		"meta"		},
	{ BLK_TC_DISCARD,	"discard"	},
	{ BLK_TC_DRV_DATA,	"drv_data"	},
	{ BLK_TC_FUA,		"fua"		},
};

static int blk_trace_str2mask(const char *str)
{
	int i;
	int mask = 0;
	char *buf, *s, *token;

	buf = kstrdup(str, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;
	s = strstrip(buf);

	while (1) {
		token = strsep(&s, ",");
		if (token == NULL)
			break;

		if (*token == '\0')
			continue;

		for (i = 0; i < ARRAY_SIZE(mask_maps); i++) {
			if (strcasecmp(token, mask_maps[i].str) == 0) {
				mask |= mask_maps[i].mask;
				break;
			}
		}
		if (i == ARRAY_SIZE(mask_maps)) {
			mask = -EINVAL;
			break;
		}
	}
	kfree(buf);

	return mask;
}

static ssize_t blk_trace_mask2str(char *buf, int mask)
{
	int i;
	char *p = buf;

	for (i = 0; i < ARRAY_SIZE(mask_maps); i++) {
		if (mask & mask_maps[i].mask) {
			p += sprintf(p, "%s%s",
				    (p == buf) ? "" : ",", mask_maps[i].str);
		}
	}
	*p++ = '\n';

	return p - buf;
}

static struct request_queue *blk_trace_get_queue(struct block_device *bdev)
{
	if (bdev->bd_disk == NULL)
		return NULL;

	return bdev_get_queue(bdev);
}

static ssize_t sysfs_blk_trace_attr_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct hd_struct *p = dev_to_part(dev);
	struct request_queue *q;
	struct block_device *bdev;
	struct blk_trace *bt;
	ssize_t ret = -ENXIO;

	bdev = bdget(part_devt(p));
	if (bdev == NULL)
		goto out;

	q = blk_trace_get_queue(bdev);
	if (q == NULL)
		goto out_bdput;

	mutex_lock(&q->blk_trace_mutex);

	bt = rcu_dereference_protected(q->blk_trace,
				       lockdep_is_held(&q->blk_trace_mutex));
	if (attr == &dev_attr_enable) {
		ret = sprintf(buf, "%u\n", !!bt);
		goto out_unlock_bdev;
	}

	if (bt == NULL)
		ret = sprintf(buf, "disabled\n");
	else if (attr == &dev_attr_act_mask)
		ret = blk_trace_mask2str(buf, bt->act_mask);
	else if (attr == &dev_attr_pid)
		ret = sprintf(buf, "%u\n", bt->pid);
	else if (attr == &dev_attr_start_lba)
		ret = sprintf(buf, "%llu\n", bt->start_lba);
	else if (attr == &dev_attr_end_lba)
		ret = sprintf(buf, "%llu\n", bt->end_lba);

out_unlock_bdev:
	mutex_unlock(&q->blk_trace_mutex);
out_bdput:
	bdput(bdev);
out:
	return ret;
}

static ssize_t sysfs_blk_trace_attr_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct block_device *bdev;
	struct request_queue *q;
	struct hd_struct *p;
	struct blk_trace *bt;
	u64 value;
	ssize_t ret = -EINVAL;

	if (count == 0)
		goto out;

	if (attr == &dev_attr_act_mask) {
		if (sscanf(buf, "%llx", &value) != 1) {
			/* Assume it is a list of trace category names */
			ret = blk_trace_str2mask(buf);
			if (ret < 0)
				goto out;
			value = ret;
		}
	} else if (sscanf(buf, "%llu", &value) != 1)
		goto out;

	ret = -ENXIO;

	p = dev_to_part(dev);
	bdev = bdget(part_devt(p));
	if (bdev == NULL)
		goto out;

	q = blk_trace_get_queue(bdev);
	if (q == NULL)
		goto out_bdput;

	mutex_lock(&q->blk_trace_mutex);

	bt = rcu_dereference_protected(q->blk_trace,
				       lockdep_is_held(&q->blk_trace_mutex));
	if (attr == &dev_attr_enable) {
		if (!!value == !!bt) {
			ret = 0;
			goto out_unlock_bdev;
		}
		if (value)
			ret = blk_trace_setup_queue(q, bdev);
		else
			ret = blk_trace_remove_queue(q);
		goto out_unlock_bdev;
	}

	ret = 0;
	if (bt == NULL) {
		ret = blk_trace_setup_queue(q, bdev);
		bt = rcu_dereference_protected(q->blk_trace,
				lockdep_is_held(&q->blk_trace_mutex));
	}

	if (ret == 0) {
		if (attr == &dev_attr_act_mask)
			bt->act_mask = value;
		else if (attr == &dev_attr_pid)
			bt->pid = value;
		else if (attr == &dev_attr_start_lba)
			bt->start_lba = value;
		else if (attr == &dev_attr_end_lba)
			bt->end_lba = value;
	}

out_unlock_bdev:
	mutex_unlock(&q->blk_trace_mutex);
out_bdput:
	bdput(bdev);
out:
	return ret ? ret : count;
}

int blk_trace_init_sysfs(struct device *dev)
{
	return sysfs_create_group(&dev->kobj, &blk_trace_attr_group);
}

void blk_trace_remove_sysfs(struct device *dev)
{
	sysfs_remove_group(&dev->kobj, &blk_trace_attr_group);
}

#endif /* CONFIG_BLK_DEV_IO_TRACE */

#ifdef CONFIG_EVENT_TRACING

void blk_dump_cmd(char *buf, struct request *rq)
{
	int i, end;
	int len = rq->cmd_len;
	unsigned char *cmd = rq->cmd;

	if (rq->cmd_type != REQ_TYPE_BLOCK_PC) {
		buf[0] = '\0';
		return;
	}

	for (end = len - 1; end >= 0; end--)
		if (cmd[end])
			break;
	end++;

	for (i = 0; i < len; i++) {
		buf += sprintf(buf, "%s%02x", i == 0 ? "" : " ", cmd[i]);
		if (i == end && end != len - 1) {
			sprintf(buf, " ..");
			break;
		}
	}
}

void blk_fill_rwbs(char *rwbs, u32 rw, int bytes)
{
	int i = 0;

	if (rw & REQ_FLUSH)
		rwbs[i++] = 'F';

	if (rw & WRITE)
		rwbs[i++] = 'W';
	else if (rw & REQ_DISCARD)
		rwbs[i++] = 'D';
	else if (bytes)
		rwbs[i++] = 'R';
	else
		rwbs[i++] = 'N';

	if (rw & REQ_FUA)
		rwbs[i++] = 'F';
	if (rw & REQ_RAHEAD)
		rwbs[i++] = 'A';
	if (rw & REQ_SYNC)
		rwbs[i++] = 'S';
	if (rw & REQ_META)
		rwbs[i++] = 'M';
	if (rw & REQ_SECURE)
		rwbs[i++] = 'E';

	rwbs[i] = '\0';
}
EXPORT_SYMBOL_GPL(blk_fill_rwbs);

#endif /* CONFIG_EVENT_TRACING */

