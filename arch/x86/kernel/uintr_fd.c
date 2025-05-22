// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021, Intel Corporation.
 *
 * Sohil Mehta <sohil.mehta@intel.com>
 */
#define pr_fmt(fmt)	"uintr: " fmt

#include <linux/anon_inodes.h>
#include <linux/fdtable.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <linux/mm.h>         // virt_to_phys, virt_to_page, page_to_phys
#include <linux/fs.h>         // struct file
#include <asm/io.h>

#include <asm/uintr.h>
static bool Debug = false;

struct uintrfd_ctx {
	struct uintr_receiver_info *r_info;
	/* Protect sender_list */
	spinlock_t sender_lock;
	struct list_head sender_list;
};

#ifdef CONFIG_PROC_FS
static void uintrfd_show_fdinfo(struct seq_file *m, struct file *file)
{
	struct uintrfd_ctx *uintrfd_ctx = file->private_data;

	/* Check: Should we print the receiver and sender info here? */
	seq_printf(m, "user_vector:%llu\n", uintrfd_ctx->r_info->uvec);
}
#endif

static int uintrfd_release(struct inode *inode, struct file *file)
{
	struct uintrfd_ctx *uintrfd_ctx = file->private_data;
	struct uintr_sender_info *s_info, *tmp;
	unsigned long flags;
	if(Debug)printk("recv: Release uintrfd for r_task %d uvec %llu\n",
		 uintrfd_ctx->r_info->upid_ctx->task->pid,
		 uintrfd_ctx->r_info->uvec);
	pr_debug("recv: Release uintrfd for r_task %d uvec %llu\n",
		 uintrfd_ctx->r_info->upid_ctx->task->pid,
		 uintrfd_ctx->r_info->uvec);

	spin_lock_irqsave(&uintrfd_ctx->sender_lock, flags);
	list_for_each_entry_safe(s_info, tmp, &uintrfd_ctx->sender_list, node) {
		list_del(&s_info->node);
		do_uintr_unregister_sender(uintrfd_ctx->r_info, s_info);
	}
	spin_unlock_irqrestore(&uintrfd_ctx->sender_lock, flags);

	do_uintr_unregister_vector(uintrfd_ctx->r_info);
	kfree(uintrfd_ctx);

	return 0;
}

#define UINTR_GET_UPID_PHYS_ADDR _IOR('u', 1, u64)

static long uintrfd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct uintrfd_ctx *uintrfd_ctx = file->private_data;
	u64 __user *upid_addr = (u64 __user *)arg;  // 用户空间指针
	u64 virt_addr, phys_addr;
	// struct page *page;
	
    switch (cmd) {
    case UINTR_GET_UPID_PHYS_ADDR: {
        if (!uintrfd_ctx->r_info || !uintrfd_ctx->r_info->upid_ctx)
            return -EINVAL;  // 检查数据结构是否有效
            
		virt_addr = (u64) uintrfd_ctx->r_info->upid_ctx->upid;
		// if (!virt_addr_valid((void *)virt_addr))
        //     return -EFAULT;

		// 翻译为物理地址
		// page = virt_to_page((void *)virt_addr);
        // phys_addr = page_to_phys(page) | (virt_addr & ~PAGE_MASK);

        // if (copy_to_user(upid_addr, &phys_addr, sizeof(phys_addr)))
		phys_addr = virt_addr - uintr_mem_offset();
		if (copy_to_user(upid_addr, &phys_addr, sizeof(phys_addr)))
            return -EFAULT;  // 拷贝失败
        return 0;
    }
    default:
        return -ENOTTY;  // 未知命令
    }
}

static const struct file_operations uintrfd_fops = {
#ifdef CONFIG_PROC_FS
	.show_fdinfo	= uintrfd_show_fdinfo,
#endif
	.release	= uintrfd_release,
	.llseek		= noop_llseek,
	.unlocked_ioctl	= uintrfd_ioctl,
};

/*
 * sys_uintr_create_fd - Create a uintr_fd for the registered interrupt vector.
 */
SYSCALL_DEFINE2(uintr_create_fd, u64, vector, unsigned int, flags)
{
	if (Debug)printk("uintr_create_fd called\n");
	struct uintrfd_ctx *uintrfd_ctx;
	int uintrfd;
	int ret;

	if (!uintr_arch_enabled())
		return -EOPNOTSUPP;

	if (flags)
		return -EINVAL;

	uintrfd_ctx = kzalloc(sizeof(*uintrfd_ctx), GFP_KERNEL);
	if (!uintrfd_ctx)
		return -ENOMEM;

	uintrfd_ctx->r_info = kzalloc(sizeof(*uintrfd_ctx->r_info), GFP_KERNEL);
	if (!uintrfd_ctx->r_info) {
		ret = -ENOMEM;
		goto out_free_ctx;
	}

	uintrfd_ctx->r_info->uvec = vector;
	ret = do_uintr_register_vector(uintrfd_ctx->r_info);
	if (ret) {
		kfree(uintrfd_ctx->r_info);
		goto out_free_ctx;
	}

	INIT_LIST_HEAD(&uintrfd_ctx->sender_list);
	spin_lock_init(&uintrfd_ctx->sender_lock);

	/* TODO: Get user input for flags - UFD_CLOEXEC */
	/* Check: Do we need O_NONBLOCK? */
	uintrfd = anon_inode_getfd("[uintrfd]", &uintrfd_fops, uintrfd_ctx,
				   O_RDONLY | O_CLOEXEC | O_NONBLOCK);

	if (uintrfd < 0) {
		ret = uintrfd;
		goto out_free_uvec;
	}
	if(Debug)printk("recv: Alloc vector success uintrfd %d uvec %llu for task=%d\n",
		 uintrfd, uintrfd_ctx->r_info->uvec, current->pid);

	pr_debug("recv: Alloc vector success uintrfd %d uvec %llu for task=%d\n",
		 uintrfd, uintrfd_ctx->r_info->uvec, current->pid);

	return uintrfd;

out_free_uvec:
	do_uintr_unregister_vector(uintrfd_ctx->r_info);
out_free_ctx:
	kfree(uintrfd_ctx);
	if(Debug)printk("recv: Alloc vector failed for task=%d ret %d\n",
		 current->pid, ret);
	pr_debug("recv: Alloc vector failed for task=%d ret %d\n",
		 current->pid, ret);
	return ret;
}

/*
 * sys_uintr_register_handler - setup user interrupt handler for receiver.
 */
SYSCALL_DEFINE2(uintr_register_handler, u64 __user *, handler, unsigned int, flags)
{	
	if (Debug) printk("uintr_register_handler called\n");
	int ret;

	if (!uintr_arch_enabled())
		return -EOPNOTSUPP;

	if (flags)
		return -EINVAL;

	/* TODO: Validate the handler address */
	if (!handler)
		return -EFAULT;

	ret = do_uintr_register_handler((u64)handler);
	if(Debug) printk("recv: register handler task=%d flags %d handler %lx ret %d\n",current->pid, flags, (unsigned long)handler, ret);
	pr_debug("recv: register handler task=%d flags %d handler %lx ret %d\n",
		 current->pid, flags, (unsigned long)handler, ret);

	return ret;
}

/*
 * sys_uintr_unregister_handler - Teardown user interrupt handler for receiver.
 */
SYSCALL_DEFINE1(uintr_unregister_handler, unsigned int, flags)
{
	if (Debug) printk("uintr_unregister_handler called\n");
	int ret;

	if (!uintr_arch_enabled())
		return -EOPNOTSUPP;

	if (flags)
		return -EINVAL;

	ret = do_uintr_unregister_handler();
	if(Debug) printk("recv: unregister handler task=%d flags %d ret %d\n",
		 current->pid, flags, ret);
	pr_debug("recv: unregister handler task=%d flags %d ret %d\n",
		 current->pid, flags, ret);

	return ret;
}

/*
 * sys_uintr_register_sender - setup user inter-processor interrupt sender.
 */
SYSCALL_DEFINE2(uintr_register_sender, int64_t, uintrfd, unsigned int, flags)
{
	if (Debug) printk("uintr_register_sender called\n");
	struct uintr_sender_info *s_info;
	struct uintrfd_ctx *uintrfd_ctx;
	unsigned long lock_flags;
	struct file *uintr_f;
	struct fd f;
	int ret = 0;

	if (!uintr_arch_enabled())
		return -EOPNOTSUPP;

	if (flags) {
		printk(KERN_WARNING "uintr_register_sender: flags %x\n", flags);
		if ((~flags) & (1<<9)) {
			return -EINVAL;
		}
		flags &= ~(1<<9);
		if ((flags & 0x3f) != flags) {
			return -EINVAL;
		}
		return raw_uintr_register_sender(uintrfd, flags);
	}

	f = fdget(uintrfd);
	uintr_f = f.file;
	if (!uintr_f)
		return -EBADF;

	if (uintr_f->f_op != &uintrfd_fops) {
		ret = -EOPNOTSUPP;
		goto out_fdput;
	}

	uintrfd_ctx = (struct uintrfd_ctx *)uintr_f->private_data;

	spin_lock_irqsave(&uintrfd_ctx->sender_lock, lock_flags);
	list_for_each_entry(s_info, &uintrfd_ctx->sender_list, node) {
		if (s_info->task == current) {
			ret = -EISCONN;
			break;
		}
	}
	spin_unlock_irqrestore(&uintrfd_ctx->sender_lock, lock_flags);

	if (ret)
		goto out_fdput;

	s_info = kzalloc(sizeof(*s_info), GFP_KERNEL);
	if (!s_info) {
		ret = -ENOMEM;
		goto out_fdput;
	}

	ret = do_uintr_register_sender(uintrfd_ctx->r_info, s_info);
	if (ret) {
		kfree(s_info);
		goto out_fdput;
	}

	spin_lock_irqsave(&uintrfd_ctx->sender_lock, lock_flags);
	list_add(&s_info->node, &uintrfd_ctx->sender_list);
	spin_unlock_irqrestore(&uintrfd_ctx->sender_lock, lock_flags);

	ret = s_info->uitt_index;

out_fdput:
	if(Debug)printk("send: register sender task=%d flags %d ret(uipi_id)=%d\n",
		 current->pid, flags, ret);
	pr_debug("send: register sender task=%d flags %d ret(uipi_id)=%d\n",
		 current->pid, flags, ret);

	fdput(f);
	return ret;
}

/*
 * sys_uintr_unregister_sender - Unregister user inter-processor interrupt sender.
 */
SYSCALL_DEFINE2(uintr_unregister_sender, int, uintrfd, unsigned int, flags)
{	
	if (Debug) printk("uintr_unregister_sender called\n");
	struct uintr_sender_info *s_info;
	struct uintrfd_ctx *uintrfd_ctx;
	struct file *uintr_f;
	unsigned long lock_flags;
	struct fd f;
	int ret;

	if (!uintr_arch_enabled())
		return -EOPNOTSUPP;

	if (flags)
		return -EINVAL;

	f = fdget(uintrfd);
	uintr_f = f.file;
	if (!uintr_f)
		return -EBADF;

	if (uintr_f->f_op != &uintrfd_fops) {
		ret = -EOPNOTSUPP;
		goto out_fdput;
	}

	uintrfd_ctx = (struct uintrfd_ctx *)uintr_f->private_data;

	ret = -EINVAL;
	spin_lock_irqsave(&uintrfd_ctx->sender_lock, lock_flags);
	list_for_each_entry(s_info, &uintrfd_ctx->sender_list, node) {
		if (s_info->task == current) {
			ret = 0;
			list_del(&s_info->node);
			do_uintr_unregister_sender(uintrfd_ctx->r_info, s_info);
			break;
		}
	}
	spin_unlock_irqrestore(&uintrfd_ctx->sender_lock, lock_flags);
	if(Debug)printk("send: unregister sender uintrfd %d for task=%d ret %d\n",
		 uintrfd, current->pid, ret);
	pr_debug("send: unregister sender uintrfd %d for task=%d ret %d\n",
		 uintrfd, current->pid, ret);

out_fdput:
	fdput(f);
	return ret;
}

/*
 * sys_uintr_wait - Wait for a user interrupt
 */
SYSCALL_DEFINE1(uintr_wait, unsigned int, flags)
{
	if (!uintr_arch_enabled())
		return -EOPNOTSUPP;

	if (flags)
		return -EINVAL;

	/* TODO: Add a timeout option */
	return uintr_receiver_wait();
}
