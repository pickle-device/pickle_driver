// Copyright (c) 2025 The Regents of University of California
// All rights reserved.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cacheflush.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pgtable.h>
#include <linux/set_memory.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "pickle_driver_internal.h"

static struct mmap_paddr_tracker mmap_tracker;

const u64 pickle_device_paddr = 0x10110000;
const u64 pickle_device_data_paddr = 0x10110008;
const u64 pickle_device_spec_paddr = 0x10110010;
void __iomem *pickle_device_ptr;
void __iomem *pickle_device_data_ptr;
void __iomem *pickle_device_spec_ptr;

u64 read_spec_from_device(u64 field);

u64 read_spec_from_device(u64 field) {
  u64 *ptr = (u64 *)pickle_device_spec_ptr;
  u64 spec_val = readq(ptr + field);
  pr_info("%s: read from addr 0x%llx, data 0x%llx\n", __func__,
          (uint64_t)(ptr + field), spec_val);
  return spec_val;
}

static long pickle_driver_ioctl(struct file *file, unsigned ioc,
                                unsigned long arg) {
  int err = 0;

  pr_info("%s: ioc = %d, %d\n", __func__, ioc,
          ioc == IOC_PICKLE_DRIVER_GET_DEVICE_SPECS);

  if (ioc == ARM64_IOC_PICKLE_DRIVER_MMAP_PADDR) {
    struct mmap_paddr_params __user *u_params;
    struct mmap_paddr_params k_params;

    u_params = (struct mmap_paddr_params *)arg;
    // copy from user space
    err = copy_from_user(&k_params, u_params, sizeof(k_params));
    if (err) {
      pr_info("%s: copy_from_user failed, errno = %d\n", __func__, err);
      return err;
    }

    k_params.paddr = mmap_tracker.paddrs[0];

    err = copy_to_user(&(u_params->paddr), &(k_params.paddr),
                       sizeof(k_params.paddr));
    if (err) {
      pr_info("%s: copy_to_user failed, errno = %d\n", __func__, err);
      return err;
    }

    pr_info("%s: ioc = ARM64_IOC_PICKLE_DRIVER_MMAP_PADDR\n", __func__);
    return 0;
  } else if (ioc == ARM64_IOC_PICKLE_DRIVER_GET_PROCESS_PAGETABLE_PADDR) {
    struct process_pagetable_params __user *u_params;
    struct process_pagetable_params k_params;
    struct task_struct *task;
    struct mm_struct *mm;

    pr_info("%s: ioc = ARM64_IOC_PICKLE_DRIVER_GET_PROCESS_PAGETABLE_PADDR\n",
            __func__);

    u_params = (struct process_pagetable_params *)arg;
    // copy from user space
    err = copy_from_user(&k_params, u_params, sizeof(k_params));
    if (err) {
      pr_info("%s: copy_from_user failed, errno = %d\n", __func__, err);
      return err;
    }

    // find the task from pid
    rcu_read_lock();
    task = pid_task(find_vpid(k_params.pid), PIDTYPE_PID);
    rcu_read_unlock();

    if (task == NULL) {
      pr_info(
          "%s: failed to find task_struct associated with pid %lld, err %d\n",
          __func__, k_params.pid, err);
      return err;
    }

    // find the memory manager
    mm = task->mm;
    if (mm == NULL) {
      mm = task->active_mm;
    }
    if (mm == NULL) {
      pr_info("%s: failed to find mm_struct associated with pid %lld, err %d\n",
              __func__, k_params.pid, err);
      return err;
    }

    // figure out the root of the pagetable
    // https://elinux.org/Tims_Notes_on_ARM_memory_allocation
    k_params.pagetable_paddr = 0;
    k_params.pagetable_paddr = (uint64_t)virt_to_phys(((void *)(mm->pgd)));
    //  copy back to userspace
    err =
        copy_to_user(&(u_params->pagetable_paddr), &(k_params.pagetable_paddr),
                     sizeof(k_params.pagetable_paddr));
    if (err) {
      pr_info("%s: copy_to_user failed, errno = %d\n", __func__, err);
      return err;
    }
  } else if (ioc == IOC_PICKLE_DRIVER_GET_DEVICE_SPECS) {
    struct device_specs __user *u_params;
    struct device_specs k_params;

    pr_info("%s: ioc = IOC_PICKLE_DRIVER_GET_DEVICE_SPECS\n", __func__);
    u_params = (struct device_specs *)arg;
    k_params.availability =
        read_spec_from_device(DEVICE_AVAILABILITY_PADDR_OFFSET);
    k_params.prefetch_distance =
        read_spec_from_device(DEVICE_PREFETCH_DISTANCE_PADDR_OFFSET);

    err = copy_to_user(u_params, &k_params, sizeof(k_params));
    if (err) {
      pr_info("%s: copy_to_user failed, errono = %d\n", __func__, err);
      return err;
    }
  }
  pr_info("%s: DONE\n", __func__);
  return 0;
}

static int pickle_driver_open(struct inode *inode, struct file *file) {
  pr_info("%s: DONE\n", __func__);
  return 0;
}

static int pickle_driver_data_transfer_mmap(struct file *file,
                                            struct vm_area_struct *vma) {
  // In this function, we allocate a new physical page, then assign the vma
  // virtual range to it.
  int err = 0;
  char *new_page_ptr =
      (char *)__get_free_page(GFP_USER);  // allocate a new page in kernel space
  uint64_t page_vaddr = (uint64_t)new_page_ptr;
  phys_addr_t page_paddr = virt_to_phys(
      (void *)
          page_vaddr);  // virt_to_phys works for virt address in kernel space
  uint64_t pfn = page_paddr >> PAGE_SHIFT;

  flush_cache_range(vma, vma->vm_start, vma->vm_end);
  pr_info("%s: created a kernel space page at paddr 0x%llx, vaddr 0x%llx\n",
          __func__, page_paddr, page_vaddr);
  pr_info("%s: vm_start 0x%llx, vm_end 0x%llx, vm_page_prot 0x%llx\n", __func__,
          (u64)vma->vm_start, (u64)vma->vm_end, vma->vm_page_prot.pgprot);
  SetPageReserved(virt_to_page(page_vaddr));
  vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
  mmap_tracker.paddrs[0] = page_paddr;
  err = remap_pfn_range(vma, vma->vm_start, pfn, vma->vm_end - vma->vm_start,
                        vma->vm_page_prot);
  // flush_tlb_range(vma, vma->vm_start, vma->vm_end);
  flush_tlb_all();
  if (err) {
    pr_info(
        "%s: failed to remap_pfn_range vm_start 0x%llx, vm_end 0x%llx, "
        "vm_page_prot 0x%llx, errno %d\n",
        __func__, (u64)vma->vm_start, (u64)vma->vm_end,
        vma->vm_page_prot.pgprot, err);
    return err;
  }
  pr_info(
      "%s: remap_pfn_range vm_start 0x%llx, vm_end 0x%llx, vm_page_prot "
      "0x%llx\n",
      __func__, (u64)vma->vm_start, (u64)vma->vm_end, vma->vm_page_prot.pgprot);
  pr_info("%s: DONE\n", __func__);

  return 0;
}

static ssize_t device_read(struct file *file, char __user *buf, size_t count,
                           loff_t *ppos) {
  pr_info("%s: nothing to do\n", __func__);
  pr_info("%s: DONE\n", __func__);

  return 0;
}

static ssize_t device_write(struct file *file, const char __user *buf,
                            size_t count, loff_t *ppos) {
  int err = 0;
  int i;
  ssize_t num_bytes_written = 0;

  const u8 __user *buf8 = (const u8 __user *)buf;
  u8 data[512];
  u64 op = *ppos;

  err = copy_from_user(data, buf8, count);
  if (err) {
    pr_info("%s: error when calling copy_from_user(): errno = %d", __func__,
            err);
    return err;
  }

  if (op == 0) {  // read (uncacheable_page_start_paddr,
                  // uncacheable_page_end_paddr) and send to device
    for (i = 0; i < 16; i++) {
      writeb(data[i], pickle_device_ptr);
      wmb();
      // pr_info("%s: writing 0x%x to pickle device op 0\n", __func__, data[i]);
    }
    num_bytes_written = 16;
  } else if (op == 1) {
    for (i = 0; i < count; i++) {
      writeb(data[i], pickle_device_data_ptr);
      wmb();
      // pr_info("%s: writing 0x%x to pickle device op 1\n", __func__, data[i]);
    }
    num_bytes_written = count;
  } else {
    pr_info("%s: invalid op: op = 0x%llx\n", __func__, op);
    return -EINVAL;
  }

  pr_info("%s: DONE\n", __func__);

  return num_bytes_written;
}

static const struct file_operations pickle_driver_fops = {
    .owner = THIS_MODULE,
    .open = pickle_driver_open,
    .unlocked_ioctl = pickle_driver_ioctl,
    .compat_ioctl = pickle_driver_ioctl,
    .mmap = pickle_driver_data_transfer_mmap,
    .read = device_read,
    .write = device_write,
};

static struct miscdevice pickle_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "hey_pickle",
    .mode = (umode_t)0666,
    .fops = &pickle_driver_fops,
};

static int __init pickle_driver_init(void) {
  int err = 0;

  mmap_paddr_tracker_init(&mmap_tracker);

  err = misc_register(&pickle_device);
  if (err != 0) {
    pr_err("%s: failed to register pickle_device, err: %d\n", __func__, err);
    return -ENODEV;
  }
  pickle_device_ptr = ioremap(pickle_device_paddr, 8);
  pickle_device_data_ptr = ioremap(pickle_device_data_paddr, 8);
  pickle_device_spec_ptr = ioremap(pickle_device_spec_paddr, NUM_SPECS * 8);
  pr_info("%s: DONE\n", __func__);
  return 0;
}
module_init(pickle_driver_init);

static void __exit pickle_driver_exit(void) {
  iounmap(pickle_device_spec_ptr);
  iounmap(pickle_device_data_ptr);
  iounmap(pickle_device_ptr);
  misc_deregister(&pickle_device);
  mmap_paddr_tracker_free(&mmap_tracker);
  pr_info("%s: DONE\n", __func__);
}
module_exit(pickle_driver_exit);

MODULE_DESCRIPTION("Pickle driver");
MODULE_LICENSE("GPL");  // we are using GPL functions
