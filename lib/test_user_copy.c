/*
 * Kernel module for testing copy_to/from_user infrastructure.
 *
 * Copyright 2013 Google Inc. All Rights Reserved
 *
 * Authors:
 *      Kees Cook       <keescook@chromium.org>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mman.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

/*
 * Several 32-bit architectures support 64-bit {get,put}_user() calls.
 * As there doesn't appear to be anything that can safely determine
 * their capability at compile-time, we just have to opt-out certain archs.
 */
#if BITS_PER_LONG == 64 || (!(defined(CONFIG_ARM) && !defined(MMU)) && \
			    !defined(CONFIG_AVR32) &&		\
			    !defined(CONFIG_BLACKFIN) &&	\
			    !defined(CONFIG_M32R) &&		\
			    !defined(CONFIG_M68K) &&		\
			    !defined(CONFIG_MICROBLAZE) &&	\
			    !defined(CONFIG_MN10300) &&		\
			    !defined(CONFIG_NIOS2) &&		\
			    !defined(CONFIG_PPC32) &&		\
			    !defined(CONFIG_SUPERH))
# define TEST_U64
#endif

#define test(exp, val, msg, ...)			\
({							\
	int cond = (exp) != (val);			\
	if (cond)					\
		pr_warn(msg "\n", ##__VA_ARGS__);	\
	cond;						\
})

/* Fill a buffer with increasing values */
static void *memfill(void *dst, u8 val, size_t len)
{
	u8 *ptr = dst;

	for (; len > 0; --len)
		*(ptr++) = 0x80 | val++;
	return dst;
}

static int __init test_user_copy_init(void)
{
	int ret = 0;
	char *kmem;
	char __user *usermem;
	char *bad_usermem;
	unsigned long user_addr, n;
	long offs, koffs, len, index;
	u8 val_u8;
	u16 val_u16;
	u32 val_u32;
#ifdef TEST_U64
	u64 val_u64;
#endif

	kmem = kmalloc(PAGE_SIZE * 2, GFP_KERNEL);
	if (!kmem)
		return -ENOMEM;

	user_addr = vm_mmap(NULL, 0, PAGE_SIZE * 4,
			    PROT_READ | PROT_WRITE | PROT_EXEC,
			    MAP_ANONYMOUS | MAP_PRIVATE, 0);
	if (user_addr >= (unsigned long)(TASK_SIZE)) {
		pr_warn("Failed to allocate user memory\n");
		kfree(kmem);
		return -ENOMEM;
	}

	vm_munmap(user_addr, PAGE_SIZE);
	vm_munmap(user_addr + PAGE_SIZE*3, PAGE_SIZE);
	user_addr += PAGE_SIZE;

	usermem = (char __user *)user_addr;
	bad_usermem = (char *)user_addr;

	/*
	 * Legitimate usage: none of these copies should fail.
	 */
	memfill(kmem, 0, PAGE_SIZE * 2);
	ret |= test(0, n = copy_to_user(usermem, kmem, PAGE_SIZE*2),
		    "legitimate copy_to_user failed to write %lu bytes", n);
	memset(kmem, 0x7f, PAGE_SIZE);
	ret |= test(0, n = copy_from_user(kmem, usermem, PAGE_SIZE),
		    "legitimate copy_from_user failed to read %lu bytes", n);
	ret |= test(0, memcmp(kmem, kmem + PAGE_SIZE, PAGE_SIZE),
		    "legitimate usercopy failed to copy data");

	/*
	 * Partially valid mappings: should only perform a partial copy.
	 */
	for (koffs = 0; koffs <= 7; ++koffs) {
		memset(kmem + PAGE_SIZE, 0x7f, koffs);
		for (offs = 1; offs <= 256; ++offs) {
			memset(kmem + PAGE_SIZE + koffs, 0x7f,
			       PAGE_SIZE - koffs);
			for (len = 0; len <= 256; ++len) {
				index = koffs + PAGE_SIZE + len - 1;
				if (len > offs)
					kmem[index] = 0;
				else if (len)
					kmem[index] = 0x80 | (-offs + len - 1);
				memset(kmem, 0x7f, PAGE_SIZE);
				/*
				 * kmem:
				 *  0x7f * PAGE_SIZE
				 *
				 *  0x7f * koffs
				 *  0x80|i * offs
				 *  0x00 * len - offs
				 *  0x7f * PAGE_SIZE - len - koffs
				 */
				ret |= test(max_t(long, len - offs, 0),
					n = copy_from_user(kmem + koffs,
							   usermem + PAGE_SIZE*2
								- offs,
							   len),
					"partial copy_from_user (offs %ld, koffs %ld, len %ld) failed to read %lu bytes instead of %ld",
					offs, koffs, len, n, len - offs);
				ret |= test(0,
					memcmp(kmem, kmem + PAGE_SIZE, koffs),
					"partial copy_from_user (offs %ld, koffs %ld, len %ld, ret %lu) overwrote poison before kmem",
					offs, koffs, len, n);
				ret |= test(0,
					memcmp(kmem + koffs,
					       kmem + PAGE_SIZE + koffs,
					       len - n),
					"partial copy_from_user (offs %ld, koffs %ld, len %ld, ret %lu) failed to copy data",
					offs, koffs, len, n);
				ret |= test(0,
					memcmp(kmem + koffs + offs,
					       kmem + PAGE_SIZE + koffs + offs,
					       n),
					"partial copy_from_user (offs %ld, koffs %ld, len %ld, ret %lu) failed to zero data",
					offs, koffs, len, n);
				ret |= test(0,
					memcmp(kmem + koffs + len,
					       kmem + PAGE_SIZE + koffs + len,
					       PAGE_SIZE - koffs - len),
					"partial copy_from_user (offs %ld, koffs %ld, len %ld, ret %lu) overwrote poison after kmem",
					offs, koffs, len, n);
			}
			if (need_resched())
				schedule();
		}
	}
	memfill(kmem, 0, PAGE_SIZE);
	for (koffs = 0; koffs <= 7; ++koffs) {
		for (offs = 1; offs <= 256; ++offs) {
			for (len = 0; len <= 256; ++len) {
				ret |= test(max_t(long, len - offs, 0),
					n = copy_to_user(usermem + PAGE_SIZE*2
								- offs,
							 kmem + koffs,
							 len),
					"partial copy_to_user (offs %ld, koffs %ld, len %ld) failed to write %lu bytes instead of %ld",
					offs, koffs, len, n, len - offs);
				/* Read back what was written to check */
				ret |= test(max_t(long, len - offs, 0),
					n = copy_from_user(kmem + PAGE_SIZE,
							   usermem + PAGE_SIZE*2
								- offs,
							   len),
					"partial checking copy_from_user (offs %ld, koffs %ld, len %ld) failed to read %lu bytes instead of %ld",
					offs, koffs, len, n, len - offs);
				ret |= test(0,
					memcmp(kmem + koffs,
					       kmem + PAGE_SIZE,
					       len - n),
					"partial copy_to_user or copy_from_user (offs %ld, koffs %ld, len %ld, ret %lu) mismatch",
					offs, koffs, len, n);
			}
			if (need_resched())
				schedule();
		}
	}

	/*
	 * Partially valid mappings starting invalid: shouldn't copy anything.
	 */
	memset(kmem + PAGE_SIZE, 0x0, PAGE_SIZE);
	for (koffs = 0; koffs <= 7; ++koffs) {
		for (offs = 1; offs <= 256; ++offs) {
			for (len = 0; len <= 256; ++len) {
				ret |= test(len,
					    n = copy_from_user(kmem + koffs,
							       usermem - offs,
							       len),
					    "early copy_from_user (offs %ld, koffs %ld, len %ld) failed to read %lu bytes instead of %lu",
					    offs, koffs, len, n, len);
				ret |= test(0, memcmp(kmem + koffs,
						      kmem + PAGE_SIZE,
						      n),
					    "early copy_from_user (offs %lu, koffs %ld, len %ld, ret %lu) failed to zero data",
					    offs, offs, len, n);
				ret |= test(len,
					    n = copy_to_user(usermem - offs,
							     kmem + koffs,
							     len),
					    "early copy_to_user (offs %ld, koffs %ld, len %ld) failed to read %lu bytes instead of %lu",
					    offs, koffs, len, n, len);
			}
			if (need_resched())
				schedule();
		}
	}

#define test_legit(size, check)						  \
	do {								  \
		val_##size = check;					  \
		ret |= test(0, put_user(val_##size, (size __user *)usermem), \
		    "legitimate put_user (" #size ") failed");		  \
		val_##size = 0;						  \
		ret |= test(0, get_user(val_##size, (size __user *)usermem), \
		    "legitimate get_user (" #size ") failed");		  \
		ret |= test(0, val_##size != check,			  \
		    "legitimate get_user (" #size ") failed to do copy"); \
		if (val_##size != check) {				  \
			pr_info("0x%llx != 0x%llx\n",			  \
				(unsigned long long)val_##size,		  \
				(unsigned long long)check);		  \
		}							  \
	} while (0)

	test_legit(u8,  0x5a);
	test_legit(u16, 0x5a5b);
	test_legit(u32, 0x5a5b5c5d);
#ifdef TEST_U64
	test_legit(u64, 0x5a5b5c5d6a6b6c6d);
#endif
#undef test_legit

	/*
	 * Invalid usage: none of these copies should succeed.
	 */

	/* Prepare kernel memory with check values. */
	memset(kmem, 0x5a, PAGE_SIZE);
	memset(kmem + PAGE_SIZE, 0, PAGE_SIZE);

	/* Reject kernel-to-kernel copies through copy_from_user(). */
	ret |= test(PAGE_SIZE,
		    n = copy_from_user(kmem, (char __user *)(kmem + PAGE_SIZE),
				       PAGE_SIZE),
		    "illegal all-kernel copy_from_user failed to read %lu bytes instead of %lu",
		    n, PAGE_SIZE);

	/* Destination half of buffer should have been zeroed. */
	ret |= test(0, memcmp(kmem + PAGE_SIZE, kmem, PAGE_SIZE),
		    "zeroing failure for illegal all-kernel copy_from_user");

#if 0
	/*
	 * When running with SMAP/PAN/etc, this will Oops the kernel
	 * due to the zeroing of userspace memory on failure. This needs
	 * to be tested in LKDTM instead, since this test module does not
	 * expect to explode.
	 */
	ret |= test(PAGE_SIZE,
		    n = copy_from_user(bad_usermem, (char __user *)kmem,
				       PAGE_SIZE),
		    "illegal reversed copy_from_user failed to read %lu bytes instead of %lu",
		    n, PAGE_SIZE);
#endif
	ret |= test(PAGE_SIZE,
		    n = copy_to_user((char __user *)kmem, kmem + PAGE_SIZE,
				     PAGE_SIZE),
		    "illegal all-kernel copy_to_user failed to read %lu bytes instead of %lu",
		    n, PAGE_SIZE);
	ret |= test(PAGE_SIZE,
		    n = copy_to_user((char __user *)kmem, bad_usermem,
				     PAGE_SIZE),
		    "illegal reversed copy_to_user failed to read %lu bytes instead of %lu",
		    n, PAGE_SIZE);

#define test_illegal(size, check)					    \
	do {								    \
		val_##size = (check);					    \
		ret |= test(0, !get_user(val_##size, (size __user *)kmem),  \
		    "illegal get_user (" #size ") passed");		    \
		ret |= test((size)0, val_##size,			    \
		    "zeroing failure for illegal get_user (" #size ")");    \
		if (val_##size != (size)0) {				    \
			pr_info("0x%llx != 0\n",			    \
				(unsigned long long)val_##size);	    \
		}							    \
		ret |= test(0, !put_user(val_##size, (size __user *)kmem),  \
		    "illegal put_user (" #size ") passed");		    \
	} while (0)

	test_illegal(u8,  0x5a);
	test_illegal(u16, 0x5a5b);
	test_illegal(u32, 0x5a5b5c5d);
#ifdef TEST_U64
	test_illegal(u64, 0x5a5b5c5d6a6b6c6d);
#endif
#undef test_illegal

	vm_munmap(user_addr, PAGE_SIZE * 2);
	kfree(kmem);

	if (ret == 0) {
		pr_info("tests passed.\n");
		return 0;
	}

	return -EINVAL;
}

module_init(test_user_copy_init);

static void __exit test_user_copy_exit(void)
{
	pr_info("unloaded.\n");
}

module_exit(test_user_copy_exit);

MODULE_AUTHOR("Kees Cook <keescook@chromium.org>");
MODULE_LICENSE("GPL");
