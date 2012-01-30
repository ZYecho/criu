#include <stdio.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

#include "compiler.h"
#include "types.h"
#include "syscall.h"
#include "parasite.h"
#include "image.h"
#include "util.h"
#include "crtools.h"

#ifdef CONFIG_X86_64

static void *brk_start, *brk_end, *brk_tail;

static struct page_entry page;
static struct vma_entry vma;

static void brk_init(void *brk)
{
	brk_start = brk_tail = brk;
	brk_end = brk_start + PARASITE_BRK_SIZE;
}

static void *brk_alloc(unsigned long bytes)
{
	void *addr = NULL;
	if (brk_end > (brk_tail + bytes)) {
		addr	= brk_tail;
		brk_tail+= bytes;
	}
	return addr;
}

static void brk_free(unsigned long bytes)
{
	if (brk_start >= (brk_tail - bytes))
		brk_tail -= bytes;
}

static unsigned long builtin_strlen(char *str)
{
	unsigned long len = 0;
	while (*str++)
		len++;
	return len;
}

static const unsigned char hex[] = "0123456789abcdef";
static char *long2hex(unsigned long v)
{
	static char buf[32];
	char *p = buf;
	int i;

	for (i = sizeof(long) - 1; i >= 0; i--) {
		*p++ = hex[ ((((unsigned char *)&v)[i]) & 0xf0) >> 4 ];
		*p++ = hex[ ((((unsigned char *)&v)[i]) & 0x0f) >> 0 ];
	}
	*p = 0;

	return buf;
}

static void sys_write_msg(const char *msg)
{
	int size = 0;
	while (msg[size])
		size++;
	sys_write(1, msg, size);
}

static inline int should_dump_page(struct vma_entry *vmae, unsigned char mincore_flags)
{
#ifdef PAGE_ANON
	if (vma_entry_is(vmae, VMA_FILE_PRIVATE))
		return mincore_flags & PAGE_ANON;
	else
		return mincore_flags & PAGE_RSS;
#else
	return (mincore_flags & PAGE_RSS);
#endif
}

static int parasite_open_file(struct parasite_dump_file_args *fa)
{
	int fd;

	fd = sys_open(fa->open_path, fa->open_flags, fa->open_mode);
	if (fd < 0) {
		sys_write_msg("sys_open failed\n");
		SET_PARASITE_STATUS(&fa->status, PARASITE_ERR_OPEN, fd);
		fd = fa->status.ret;
	}

	return fd;
}

/*
 * This is the main page dumping routine, it's executed
 * inside a victim process space.
 */
static int dump_pages(struct parasite_dump_pages_args *args)
{
	parasite_status_t *st = &args->fa.status;
	unsigned long nrpages, pfn, length;
	unsigned long prot_old, prot_new;
	unsigned char *map_brk = NULL;
	unsigned char *map;

	int ret = PARASITE_ERR_FAIL;

	args->nrpages_dumped = 0;
	prot_old = prot_new = 0;

	if (args->fd == -1UL) {
		ret = parasite_open_file(&args->fa);
		if (ret < 0)
			goto err;

		args->fd = ret;
	}

	/* Start from the end of file */
	sys_lseek(args->fd, 0, SEEK_END);

	length	= args->vma_entry.end - args->vma_entry.start;
	nrpages	= length / PAGE_SIZE;

	/*
	 * brk should allow us to handle up to 128M of memory,
	 * otherwise call for mmap.
	 */
	map = brk_alloc(nrpages);
	if (map) {
		map_brk = map;
	} else {
		map = (void *)sys_mmap(NULL, nrpages,
				       PROT_READ   | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS,
				       -1, 0);
		if ((long)map < 0) {
			sys_write_msg("sys_mmap failed\n");
			SET_PARASITE_STATUS(st, PARASITE_ERR_MMAP, (long)map);
			ret = st->ret;
			goto err;
		}
	}

	/*
	 * Try to change page protection if needed so we would
	 * be able to dump contents.
	 */
	if (!(args->vma_entry.prot & PROT_READ)) {
		prot_old = (unsigned long)args->vma_entry.prot;
		prot_new = prot_old | PROT_READ;
		ret = sys_mprotect((unsigned long)args->vma_entry.start,
				   (unsigned long)vma_entry_len(&args->vma_entry),
				   prot_new);
		if (ret) {
			sys_write_msg("sys_mprotect failed\n");
			SET_PARASITE_STATUS(st, PARASITE_ERR_MPROTECT, ret);
			ret = st->ret;
			goto err_free;
		}
	}

	/*
	 * Dumping the whole VMA range is not a common operation
	 * so stick for mincore as a basis.
	 */

	ret = sys_mincore((unsigned long)args->vma_entry.start, length, map);
	if (ret) {
		sys_write_msg("sys_mincore failed\n");
		SET_PARASITE_STATUS(st, PARASITE_ERR_MINCORE, ret);
		ret = st->ret;
		goto err_free;
	}

	ret = 0;
	for (pfn = 0; pfn < nrpages; pfn++) {
		unsigned long vaddr, written;

		if (should_dump_page(&args->vma_entry, map[pfn])) {
			/*
			 * That's the optimized write of
			 * page_entry structure, see image.h
			 */
			vaddr = (unsigned long)args->vma_entry.start + pfn * PAGE_SIZE;
			written = 0;

			written += sys_write(args->fd, &vaddr, sizeof(vaddr));
			written += sys_write(args->fd, (void *)vaddr, PAGE_SIZE);
			if (written != sizeof(vaddr) + PAGE_SIZE) {
				SET_PARASITE_STATUS(st, PARASITE_ERR_WRITE, written);
				ret = st->ret;
				goto err_free;
			}

			args->nrpages_dumped++;
		}
	}

	/*
	 * Don't left pages readable if they were not.
	 */
	if (prot_old != prot_new) {
		ret = sys_mprotect((unsigned long)args->vma_entry.start,
				   (unsigned long)vma_entry_len(&args->vma_entry),
				   prot_old);
		if (ret) {
			sys_write_msg("PANIC: Ouch! sys_mprotect failed on restore\n");
			SET_PARASITE_STATUS(st, PARASITE_ERR_MPROTECT, ret);
			ret = st->ret;
			goto err_free;
		}
	}

	/* on success ret = 0 */
	SET_PARASITE_STATUS(st, ret, ret);

err_free:
	if (map_brk)
		brk_free(nrpages);
	else
		sys_munmap(map, nrpages);
err:
	return ret;
}

static int dump_sigact(struct parasite_dump_file_args *args)
{
	parasite_status_t *st = &args->status;
	rt_sigaction_t act;
	struct sa_entry e;
	int fd, sig;

	int ret = PARASITE_ERR_FAIL;

	fd = parasite_open_file(args);
	if (fd < 0)
		return fd;

	sys_lseek(fd, MAGIC_OFFSET, SEEK_SET);

	for (sig = 1; sig < SIGMAX; sig++) {
		if (sig == SIGKILL || sig == SIGSTOP)
			continue;

		ret = sys_sigaction(sig, NULL, &act);
		if (ret < 0) {
			sys_write_msg("sys_sigaction failed\n");
			SET_PARASITE_STATUS(st, PARASITE_ERR_SIGACTION, ret);
			ret = st->ret;
			goto err_close;
		}

		ASSIGN_TYPED(e.sigaction, act.rt_sa_handler);
		ASSIGN_TYPED(e.flags, act.rt_sa_flags);
		ASSIGN_TYPED(e.restorer, act.rt_sa_restorer);
		ASSIGN_TYPED(e.mask, act.rt_sa_mask.sig[0]);

		ret = sys_write(fd, &e, sizeof(e));
		if (ret != sizeof(e)) {
			sys_write_msg("sys_write failed\n");
			SET_PARASITE_STATUS(st, PARASITE_ERR_WRITE, ret);
			ret = st->ret;
			goto err_close;
		}
	}

	ret = 0;
	SET_PARASITE_STATUS(st, 0, ret);

err_close:
	sys_close(fd);
	return ret;
}

static int dump_itimer(int which, int fd, parasite_status_t *st)
{
	struct itimerval val;
	int ret;
	struct itimer_entry ie;

	ret = sys_getitimer(which, &val);
	if (ret < 0) {
		sys_write_msg("getitimer failed\n");
		SET_PARASITE_STATUS(st, PARASITE_ERR_GETITIMER, ret);
		return st->ret;
	}

	ie.isec = val.it_interval.tv_sec;
	ie.iusec = val.it_interval.tv_usec;
	ie.vsec = val.it_value.tv_sec;
	ie.vusec = val.it_value.tv_sec;

	ret = sys_write(fd, &ie, sizeof(ie));
	if (ret != sizeof(ie)) {
		sys_write_msg("sys_write failed\n");
		SET_PARASITE_STATUS(st, PARASITE_ERR_WRITE, ret);
		return st->ret;
	}

	return 0;
}

static int dump_itimers(struct parasite_dump_file_args *args)
{
	parasite_status_t *st = &args->status;
	rt_sigaction_t act;
	struct sa_entry e;
	int fd, sig;

	int ret = PARASITE_ERR_FAIL;

	fd = parasite_open_file(args);
	if (fd < 0)
		return fd;

	sys_lseek(fd, MAGIC_OFFSET, SEEK_SET);

	ret = dump_itimer(ITIMER_REAL, fd, st);
	if (ret < 0)
		goto err_close;

	ret = dump_itimer(ITIMER_VIRTUAL, fd, st);
	if (ret < 0)
		goto err_close;

	ret = dump_itimer(ITIMER_PROF, fd, st);
	if (ret < 0)
		goto err_close;

	ret = 0;
	SET_PARASITE_STATUS(st, 0, ret);

err_close:
	sys_close(fd);
	return ret;
}

static int dump_misc(struct parasite_dump_misc *args)
{
	parasite_status_t *st = &args->status;

	args->secbits = sys_prctl(PR_GET_SECUREBITS, 0, 0, 0, 0);

	SET_PARASITE_STATUS(st, 0, 0);
	return 0;
}

static int __used parasite_service(unsigned long cmd, void *args, void *brk)
{
	brk_init(brk);

	BUILD_BUG_ON(sizeof(struct parasite_dump_pages_args) > PARASITE_ARG_SIZE);
	BUILD_BUG_ON(sizeof(struct parasite_dump_file_args) > PARASITE_ARG_SIZE);

	switch (cmd) {
	case PARASITE_CMD_PINGME:
		return 0;
	case PARASITE_CMD_DUMPPAGES:
		return dump_pages((struct parasite_dump_pages_args *)args);
	case PARASITE_CMD_DUMP_SIGACTS:
		return dump_sigact((struct parasite_dump_file_args *)args);
	case PARASITE_CMD_DUMP_ITIMERS:
		return dump_itimers((struct parasite_dump_file_args *)args);
	case PARASITE_CMD_DUMP_MISC:
		return dump_misc((struct parasite_dump_misc *)args);
	default:
		sys_write_msg("Unknown command to parasite\n");
		break;
	}

	return -1;
}

static void __parasite_head __used parasite_head(void)
{
	/*
	 * The linker will handle the stack allocation.
	 */
	asm volatile("parasite_head_start:							\n\t"
		     "leaq parasite_stack(%rip), %rsp						\n\t"
		     "pushq $0									\n\t"
		     "movq %rsp, %rbp								\n\t"
		     "movl parasite_cmd(%rip), %edi						\n\t"
		     "leaq parasite_args(%rip), %rsi						\n\t"
		     "leaq parasite_brk(%rip), %rdx						\n\t"
		     "call parasite_service							\n\t"
		     "parasite_service_complete:						\n\t"
		     "int $0x03									\n\t"
		     ".align 8									\n\t"
		     "parasite_cmd:								\n\t"
		     ".long 0									\n\t"
		     "parasite_args:								\n\t"
		     ".long 0									\n\t"
		     ".skip "__stringify(PARASITE_ARG_SIZE)",0					\n\t"
		     ".skip "__stringify(PARASITE_STACK_SIZE)", 0				\n\t"
		     "parasite_stack:								\n\t"
		     ".long 0									\n\t"
		     "parasite_brk:								\n\t"
		     ".skip "__stringify(PARASITE_BRK_SIZE)", 0					\n\t"
		     ".long 0									\n\t");
}

#else /* CONFIG_X86_64 */
# error x86-32 bit mode not yet implemented
#endif /* CONFIG_X86_64 */
