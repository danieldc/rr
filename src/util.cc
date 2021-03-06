/* -*- Mode: C++; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

//#define DEBUGTAG "Util"

//#define FIRST_INTERESTING_EVENT 10700
//#define LAST_INTERESTING_EVENT 10900

#include "util.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <linux/ipc.h>
#include <linux/magic.h>
#include <linux/net.h>
#include <perfmon/perf_event.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <asm/ptrace-abi.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <limits>

#include "preload/syscall_buffer.h"

#include "hpc.h"
#include "log.h"
#include "recorder_sched.h"
#include "replayer.h"
#include "session.h"
#include "task.h"
#include "trace.h"
#include "types.h"

using namespace std;

#define NUM_MAX_MAPS 1024

struct flags flags;

ostream& operator<<(ostream& o, const mapped_segment_info& m)
{
	o << m.start_addr <<"-"<< m.end_addr <<" "<<HEX(m.prot)
	  <<" f:"<< HEX(m.flags);
	return o;
}

const struct flags* rr_flags(void)
{
	return &flags;
}

struct flags* rr_flags_for_init(void)
{
	static int initialized;
	if (!initialized) {
		initialized = 1;
		return &flags;
	}
	FATAL() <<"Multiple initialization of flags.";
	return NULL;		/* not reached */
}

void update_replay_target(pid_t process, int event)
{
	if (process > 0) {
		flags.target_process = process;
	}
	if (event > 0) {
		flags.goto_event = event;
	}
}

// FIXME this function assumes that there's only one address space.
// Should instead only look at the address space of the task in
// question.
static bool is_start_of_scratch_region(Task* t, void* start_addr)
{
	for (auto& kv : t->session().tasks()) {
		Task* c = kv.second;
		if (start_addr == c->scratch_ptr) {
			return true;
		}
	}
	return false;
}

double now_sec(void)
{
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (double)tp.tv_sec + (double)tp.tv_nsec / 1e9;
}

int nanosleep_nointr(const struct timespec* ts)
{
	struct timespec req = *ts;
	while (1) {
		struct timespec rem;
		int err = nanosleep(&req, &rem);
		if (0 == err || EINTR != errno) {
			return err;
		}
		req = rem;
	}
}

int probably_not_interactive(int fd)
{
	/* Eminently tunable heuristic, but this is guaranteed to be
	 * true during rr unit tests, where we care most about this
	 * check (to a first degree).  A failing test shouldn't
	 * hang. */
	return !isatty(fd);
}

void maybe_mark_stdio_write(Task* t, int fd)
{
	char buf[256];
	ssize_t len;

	if (!rr_flags()->mark_stdio || !(STDOUT_FILENO == fd
					 || STDERR_FILENO == fd)) {
		return;
	}
	snprintf(buf, sizeof(buf) - 1, "[rr %d %d]", t->tgid(), t->trace_time());
	len = strlen(buf);
	if (write(fd, buf, len) != len) {
		FATAL() <<"Couldn't write to "<< fd;
	}
}

const char* ptrace_event_name(int event)
{
	switch (event) {
#define CASE(_id) case PTRACE_EVENT_## _id: return #_id
	CASE(FORK);
	CASE(VFORK);
	CASE(CLONE);
	CASE(EXEC);
	CASE(VFORK_DONE);
	CASE(EXIT);
	/* XXX Ubuntu 12.04 defines a "PTRACE_EVENT_STOP", but that
	 * has the same value as the newer EVENT_SECCOMP, so we'll
	 * ignore STOP. */
#ifdef PTRACE_EVENT_SECCOMP_OBSOLETE
	CASE(SECCOMP_OBSOLETE);
#else
	CASE(SECCOMP);
#endif
	CASE(STOP);
	default:
		return "???EVENT";
#undef CASE
	}
}

const char* ptrace_req_name(int request)
{
#define CASE(_id) case PTRACE_## _id: return #_id
	switch (int(request)) {
	CASE(TRACEME);
	CASE(PEEKTEXT);
	CASE(PEEKDATA);
	CASE(PEEKUSER);
	CASE(POKETEXT);
	CASE(POKEDATA);
	CASE(POKEUSER);
	CASE(CONT);
	CASE(KILL);
	CASE(SINGLESTEP);
	CASE(GETREGS);
	CASE(SETREGS);
	CASE(GETFPREGS);
	CASE(SETFPREGS);
	CASE(ATTACH);
	CASE(DETACH);
	CASE(GETFPXREGS);
	CASE(SETFPXREGS);
	CASE(SYSCALL);
	CASE(SETOPTIONS);
	CASE(GETEVENTMSG);
	CASE(GETSIGINFO);
	CASE(SETSIGINFO);
	CASE(GETREGSET);
	CASE(SETREGSET);
	CASE(SEIZE);
	CASE(INTERRUPT);
	CASE(LISTEN);
	// These aren't part of the official ptrace-request enum.
	CASE(SYSEMU);
	CASE(SYSEMU_SINGLESTEP);
#undef CASE
	default:
		return "???REQ";
	}
}

const char* signalname(int sig)
{
	/* strsignal() would be nice to use here, but it provides TMI. */
	if (SIGRTMIN <= sig && sig <= SIGRTMAX) {
		static __thread char buf[] = "SIGRT00000000";
		snprintf(buf, sizeof(buf) - 1, "SIGRT%d", sig - SIGRTMIN);
		return buf;
	}

	switch (sig) {
#define CASE(_id) case _id: return #_id
	CASE(SIGHUP); CASE(SIGINT); CASE(SIGQUIT); CASE(SIGILL);
	CASE(SIGTRAP); CASE(SIGABRT); /*CASE(SIGIOT);*/ CASE(SIGBUS);
	CASE(SIGFPE); CASE(SIGKILL); CASE(SIGUSR1); CASE(SIGSEGV);
	CASE(SIGUSR2); CASE(SIGPIPE); CASE(SIGALRM); CASE(SIGTERM);
	CASE(SIGSTKFLT); /*CASE(SIGCLD);*/ CASE(SIGCHLD); CASE(SIGCONT);
	CASE(SIGSTOP); CASE(SIGTSTP); CASE(SIGTTIN); CASE(SIGTTOU);
	CASE(SIGURG); CASE(SIGXCPU); CASE(SIGXFSZ); CASE(SIGVTALRM);
	CASE(SIGPROF); CASE(SIGWINCH); /*CASE(SIGPOLL);*/ CASE(SIGIO);
	CASE(SIGPWR); CASE(SIGSYS);
#undef CASE

	default:
		return "???signal";
	}
}

const char* syscallname(int syscall)
{
	switch (syscall) {
#define SYSCALL_DEF0(_name, _)				\
		case __NR_## _name: return #_name;
#define SYSCALL_DEF1(_name, _, _1, _2)			\
		case __NR_## _name: return #_name;
#define SYSCALL_DEF1_DYNSIZE(_name, _, _1, _2)		\
		case __NR_## _name: return #_name;
#define SYSCALL_DEF1_STR(_name, _, _1)			\
		case __NR_## _name: return #_name;
#define SYSCALL_DEF2(_name, _, _1, _2, _3, _4)		\
		case __NR_## _name: return #_name;
#define SYSCALL_DEF3(_name, _, _1, _2, _3, _4, _5, _6)	\
		case __NR_## _name: return #_name;
#define SYSCALL_DEF4(_name, _, _1, _2, _3, _4, _5, _6, _7, _8)	\
		case __NR_## _name: return #_name;
#define SYSCALL_DEF_IRREG(_name, _)			\
		case __NR_## _name: return #_name;

#include "syscall_defs.h"

#undef SYSCALL_NUM
#undef SYSCALL_DEF0
#undef SYSCALL_DEF1
#undef SYSCALL_DEF1_DYNSIZE
#undef SYSCALL_DEF1_STR
#undef SYSCALL_DEF2
#undef SYSCALL_DEF3
#undef SYSCALL_DEF4
#undef SYSCALL_DEF_IRREG

	case SYS_restart_syscall:
		return "restart_syscall";
	default:
		return "???syscall";
	}
}

bool is_always_emulated_syscall(int syscallno)
{
	switch (syscallno) {
#define EMU() true
#define EXEC() false
#define EXEC_RET_EMU() false
#define MAY_EXEC() false

#define SYSCALL_DEF0(_name, _type)			\
		case __NR_## _name: return _type();
#define SYSCALL_DEF1(_name, _type, _1, _2)		\
		case __NR_## _name: return _type();
#define SYSCALL_DEF1_DYNSIZE(_name, _type, _1, _2)	\
		case __NR_## _name: return _type();
#define SYSCALL_DEF1_STR(_name, _type, _1)		\
		case __NR_## _name: return _type();
#define SYSCALL_DEF2(_name, _type, _1, _2, _3, _4)	\
		case __NR_## _name: return _type();
#define SYSCALL_DEF3(_name, _type, _1, _2, _3, _4, _5, _6)	\
		case __NR_## _name: return _type();
#define SYSCALL_DEF4(_name, _type, _1, _2, _3, _4, _5, _6, _7, _8)	\
		case __NR_## _name: return _type();
#define SYSCALL_DEF_IRREG(_name, _type)			\
		case __NR_## _name: return _type();

#include "syscall_defs.h"

#undef EMU
#undef EXEC
#undef EXEC_EMU_RET
#undef MAY_EXEC
#undef SYSCALL_DEF0
#undef SYSCALL_DEF1
#undef SYSCALL_DEF1_DYNSIZE
#undef SYSCALL_DEF1_STR
#undef SYSCALL_DEF2
#undef SYSCALL_DEF3
#undef SYSCALL_DEF4

	case SYS_restart_syscall:
		return false;
	default:
		FATAL() <<"Unknown syscall "<< syscallno;
		return "???syscall";
	}
}

int clone_flags_to_task_flags(int flags_arg)
{
	int flags = CLONE_SHARE_NOTHING;
	// See task.h for description of the flags.
	flags |= (CLONE_CHILD_CLEARTID & flags_arg) ? CLONE_CLEARTID : 0;
	flags |= (CLONE_SETTLS & flags_arg) ? CLONE_SET_TLS : 0;
	flags |= (CLONE_SIGHAND & flags_arg) ? CLONE_SHARE_SIGHANDLERS : 0;
	flags |= (CLONE_THREAD & flags_arg) ? CLONE_SHARE_TASK_GROUP : 0;
	flags |= (CLONE_VM & flags_arg) ? CLONE_SHARE_VM : 0;
	return flags;
}

int get_ipc_command(int raw_cmd)
{
	return raw_cmd & ~IPC_64;
}

void print_register_file_tid(Task* t)
{
	print_register_file(&t->regs());
}

void print_register_file(const struct user_regs_struct* regs)
{
	fprintf(stderr, "Printing register file:\n");
	fprintf(stderr, "eax: %lx\n", regs->eax);
	fprintf(stderr, "ebx: %lx\n", regs->ebx);
	fprintf(stderr, "ecx: %lx\n", regs->ecx);
	fprintf(stderr, "edx: %lx\n", regs->edx);
	fprintf(stderr, "esi: %lx\n", regs->esi);
	fprintf(stderr, "edi: %lx\n", regs->edi);
	fprintf(stderr, "ebp: %lx\n", regs->ebp);
	fprintf(stderr, "esp: %lx\n", regs->esp);
	fprintf(stderr, "eip: %lx\n", regs->eip);
	fprintf(stderr, "eflags %lx\n",regs->eflags);
	fprintf(stderr, "orig_eax %lx\n", regs->orig_eax);
	fprintf(stderr, "xcs: %lx\n", regs->xcs);
	fprintf(stderr, "xds: %lx\n", regs->xds);
	fprintf(stderr, "xes: %lx\n", regs->xes);
	fprintf(stderr, "xfs: %lx\n", regs->xfs);
	fprintf(stderr, "xgs: %lx\n", regs->xgs);
	fprintf(stderr, "xss: %lx\n", regs->xss);
	fprintf(stderr, "\n");

}

/**
 * Remove leading blank characters from |str| in-place.  |str| must be
 * a valid string.
 */
static void trim_leading_blanks(char* str)
{
	char* trimmed = str;
	while (isblank(*trimmed)) ++trimmed;
	memmove(str, trimmed, strlen(trimmed) + 1/*\0 byte*/);
}

static int caller_wants_segment_read(Task* t,
				     const struct mapped_segment_info* info,
				     read_segment_filter_t filt,
				     void* filt_data)
{
	if (kNeverReadSegment == filt) {
		return 0;
	}
	if (kAlwaysReadSegment == filt) {
		return 1;
	}
	return filt(filt_data, t, info);
}

void iterate_memory_map(Task* t,
			memory_map_iterator_t it, void* it_data,
			read_segment_filter_t filt, void* filt_data)
{
	FILE* maps_file;
	char line[PATH_MAX];
	{
		char maps_path[PATH_MAX];
		snprintf(maps_path, sizeof(maps_path) - 1, "/proc/%d/maps",
			t->tid);
		ASSERT(t, (maps_file = fopen(maps_path, "r")))
			<<"Failed to open "<< maps_path;
	}
	while (fgets(line, sizeof(line), maps_file)) {
		uint64_t start, end;
		struct map_iterator_data data;
		char flags[32];
		int nparsed;
		int next_action;

		memset(&data, 0, sizeof(data));
		data.raw_map_line = line;

		nparsed = sscanf(line, "%llx-%llx %31s %Lx %x:%x %Lu %s",
				 &start, &end,
				 flags, &data.info.file_offset,
				 &data.info.dev_major, &data.info.dev_minor,
				 &data.info.inode, data.info.name);
		ASSERT(t, (8/*number of info fields*/ == nparsed
			   || 7/*num fields if name is blank*/ == nparsed))
			<<"Only parsed "<< nparsed <<" fields of segment info from\n"
			<< data.raw_map_line;

		trim_leading_blanks(data.info.name);
		if (start > numeric_limits<uint32_t>::max()
		    || end > numeric_limits<uint32_t>::max()
		    || !strcmp(data.info.name, "[vsyscall]")) {
			// We manually read the exe link here because
			// this helper is used to set
			// |t->vm()->exe_image()|, so we can't rely on
			// that being correct yet.
			char proc_exe[PATH_MAX];
			char exe[PATH_MAX];
			snprintf(proc_exe, sizeof(proc_exe),
				 "/proc/%d/exe", t->tid);
			readlink(proc_exe, exe, sizeof(exe));
			FATAL() <<"Sorry, tracee "<< t->tid <<" has x86-64 image "
				<< exe <<" and that's not supported.";
		}
		data.info.start_addr = (byte*)start;
		data.info.end_addr = (byte*)end;

		data.info.prot |= strchr(flags, 'r') ? PROT_READ : 0;
		data.info.prot |= strchr(flags, 'w') ? PROT_WRITE : 0;
		data.info.prot |= strchr(flags, 'x') ? PROT_EXEC : 0;
		data.info.flags |= strchr(flags, 'p') ? MAP_PRIVATE : 0;
		data.info.flags |= strchr(flags, 's') ? MAP_SHARED : 0;
		data.size_bytes = ((intptr_t)data.info.end_addr -
				   (intptr_t)data.info.start_addr);
		if (caller_wants_segment_read(t, &data.info,
					      filt, filt_data)) {
			void* addr = data.info.start_addr;
			ssize_t nbytes = data.size_bytes;
			data.mem = (byte*)malloc(nbytes);
			data.mem_len = t->read_bytes_fallible(addr, nbytes,
							      data.mem);
			/* TODO: expose read errors, somehow. */
			data.mem_len = max(0, data.mem_len);
		}

		next_action = it(it_data, t, &data);
		free(data.mem);

		if (STOP_ITERATING == next_action) {
			break;
		}
	}
	fclose(maps_file);
}

static int print_process_mmap_iterator(void* unused, Task* t,
				       const struct map_iterator_data* data)
{
	fputs(data->raw_map_line, stderr);
	return CONTINUE_ITERATING;
}

void print_process_mmap(Task* t)
{
	return iterate_memory_map(t, print_process_mmap_iterator, NULL,
				  kNeverReadSegment, NULL);
}

bool is_page_aligned(const byte* addr)
{
	return is_page_aligned(reinterpret_cast<size_t>(addr));
}

bool is_page_aligned(size_t sz)
{
	return 0 == (sz % page_size());
}

size_t page_size()
{
	return sysconf(_SC_PAGE_SIZE);
}

size_t ceil_page_size(size_t sz)
{
	size_t page_mask = ~(page_size() - 1);
	return (sz + page_size() - 1) & page_mask;
}

void* ceil_page_size(void* addr)
{
	uintptr_t ceil = ceil_page_size((uintptr_t)addr);
	return (void*)ceil;
}

void print_process_state(pid_t tid)
{
	char path[64];
	FILE* file;
	printf("child tid: %d\n", tid);
	fflush(stdout);
	bzero(path, 64);
	sprintf(path, "/proc/%d/status", tid);
	if ((file = fopen(path, "r")) == NULL) {
		perror("error reading child memory status\n");
	}

	int c = getc(file);
	while (c != EOF) {
		putchar(c);
		c = getc(file);
	}
	fclose(file);
}


void print_cwd(pid_t tid, char *str)
{
	char path[64];
	fflush(stdout);
	bzero(path, 64);
	sprintf(path, "/proc/%d/cwd", tid);
	assert(readlink(path, str, 1024) != -1);
}


static void maybe_print_reg_mismatch(int mismatch_behavior, const char* regname,
				     const char* label1, long val1,
				     const char* label2, long val2)
{
	if (mismatch_behavior >= BAIL_ON_MISMATCH) {
		LOG(error) << regname <<" "<< HEX(val1) <<" != "
			   << HEX(val2) <<" ("<< label1 <<" vs. "<< label2 <<")";
	} else if (mismatch_behavior >= LOG_MISMATCHES) {
		LOG(info) << regname <<" "<< HEX(val1) <<" != "
			  << HEX(val2) <<" ("<< label1 <<" vs. "<< label2 <<")";
	}
}

int compare_register_files(Task* t,
			   const char* name1,
			   const struct user_regs_struct* reg1,
			   const char* name2,
			   const struct user_regs_struct* reg2,
			   int mismatch_behavior)
{
	int bail_error = (mismatch_behavior >= BAIL_ON_MISMATCH);
	/* TODO: do any callers use this? */
	int errbit = 0;
	int err = 0;

#define REGCMP(_reg, _bit)						   \
	do {								   \
		if (reg1-> _reg != reg2-> _reg) {			   \
			maybe_print_reg_mismatch(mismatch_behavior, #_reg, \
						 name1, reg1-> _reg,	   \
						 name2, reg2-> _reg);	   \
			err |= (1 << (_bit));				   \
		}							   \
	} while (0)

	REGCMP(eax, ++errbit);
	REGCMP(ebx, ++errbit);
	REGCMP(ecx, ++errbit);
	REGCMP(edx, ++errbit);
	REGCMP(esi, ++errbit);
	REGCMP(edi, ++errbit);
	REGCMP(ebp, ++errbit);
	REGCMP(eip, ++errbit);
	REGCMP(xfs, ++errbit);
	REGCMP(xgs, ++errbit);
	/* The following are eflags that have been observed to be
	 * nondeterministic in practice.  We need to mask them off in
	 * this comparison to prevent replay from diverging. */
	enum {
		/* The linux kernel has been observed to report this
		 * as zero in some states during system calls. It
		 * always seems to be 1 during user-space execution so
		 * we should be able to ignore it. */
		RESERVED_FLAG_1 = 1 << 1,
		/* According to www.logix.cz/michal/doc/i386/chp04-01.htm
		 *
		 *   The RF flag temporarily disables debug exceptions
		 *   so that an instruction can be restarted after a
		 *   debug exception without immediately causing
		 *   another debug exception. Refer to Chapter 12 for
		 *   details.
		 *
		 * Chapter 12 isn't particularly clear on the point,
		 * but the flag appears to be set by |int3|
		 * exceptions.
		 *
		 * This divergence has been observed when continuing a
		 * tracee to an execution target by setting an |int3|
		 * breakpoint, which isn't used during recording.  No
		 * single-stepping was used during the recording
		 * either.
		 */
		RESUME_FLAG = 1 << 16,
		/* It's no longer known why this bit is ignored. */
		CPUID_ENABLED_FLAG = 1 << 21,
	};
	/* check the deterministic eflags */
	const long det_mask =
		~(RESERVED_FLAG_1 | RESUME_FLAG | CPUID_ENABLED_FLAG);
	long eflags1 = (reg1->eflags & det_mask);
	long eflags2 = (reg2->eflags & det_mask);
	if (eflags1 != eflags2) {
		maybe_print_reg_mismatch(mismatch_behavior, "deterministic eflags",
					 name1, eflags1, name2, eflags2);
		err |= (1 << ++errbit);
	}

	ASSERT(t, !bail_error || !err)
		<<"Fatal register mismatch (rbc/rec:"
		<< read_rbc(t->hpc) <<"/"<< t->trace.rbc <<")";

	if (!err && mismatch_behavior == LOG_MISMATCHES) {
		LOG(info) <<"(register files are the same for "<< name1
			  <<" and "<< name2 <<")";
	}

	return err;
}

void assert_child_regs_are(Task* t, const struct user_regs_struct* regs)
{
	compare_register_files(t, "replaying", &t->regs(), "recorded", regs,
			       BAIL_ON_MISMATCH);
	/* TODO: add perf counter validations (hw int, page faults, insts) */
}

/**
 * Dump |buf_len| words in |buf| to |out|, starting with a line
 * containing |label|.  See |dump_binary_data()| for a description of
 * the remaining parameters.
 */
static void dump_binary_chunk(FILE* out, const char* label,
			      const uint32_t* buf, size_t buf_len,
			      void* start_addr)
{
	int i;

	fprintf(out,"%s\n", label);
	for (i = 0 ; i < ssize_t(buf_len); i += 1) {
		uint32_t word = buf[i];
		fprintf(out, "0x%08x | [%p]\n", word,
			(byte*)start_addr + i * sizeof(*buf));
	}
}

void dump_binary_data(const char* filename, const char* label,
		      const uint32_t* buf, size_t buf_len, void* start_addr)
{
	FILE* out = fopen64(filename, "w");
	if (!out) {
		return;
	}
	dump_binary_chunk(out, label, buf, buf_len, start_addr);
	fclose(out);
}

void format_dump_filename(Task* t, int global_time, const char* tag,
			  char* filename, size_t filename_size)
{
	snprintf(filename, filename_size - 1, "%s/%d_%d_%s",
		 t->trace_dir().c_str(), t->rec_tid, global_time, tag);
}

int should_dump_memory(Task* t, const struct trace_frame& f)
{
	const struct flags* flags = rr_flags();

#if defined(FIRST_INTERESTING_EVENT)
	int is_syscall_exit = event >= 0 && state == STATE_SYSCALL_EXIT;
	if (is_syscall_exit
	    && RECORD == flags->option
	    && FIRST_INTERESTING_EVENT <= global_time
	    && global_time <= LAST_INTERESTING_EVENT) {
		return 1;
	}
	if (global_time > LAST_INTERESTING_EVENT) {
		return 0;
	}
#endif
	return (flags->dump_on == DUMP_ON_ALL
		|| flags->dump_at == int(f.global_time));
}

void dump_process_memory(Task* t, int global_time, const char* tag)
{
	char filename[PATH_MAX];
	FILE* dump_file;

	format_dump_filename(t, global_time, tag, filename, sizeof(filename));
	dump_file = fopen64(filename, "w");

	const AddressSpace& as = *(t->vm());
	for (auto& kv : as.memmap()) {
		const Mapping& first = kv.first;
		const MappableResource& second = kv.second; 
		vector<byte> mem;
		mem.resize(first.num_bytes());

		ssize_t mem_len = t->read_bytes_fallible(first.start,
				first.num_bytes(), mem.data());
		mem_len = max(0, mem_len);

		string label = first.str() + ' ' + second.str();

		if (!is_start_of_scratch_region(t, first.start)) {
			dump_binary_chunk(dump_file, label.c_str(), (const uint32_t*)mem.data(),
					mem_len / sizeof(uint32_t), first.start);
		}
	}
	fclose(dump_file);
}

static void notify_checksum_error(Task* t, int global_time,
				  unsigned checksum, unsigned rec_checksum,
				  const string &raw_map_line)
{
	char cur_dump[PATH_MAX];
	char rec_dump[PATH_MAX];

	dump_process_memory(t, global_time, "checksum_error");

	/* TODO: if the right recorder memory dump is present,
	 * automatically compare them, taking the oddball
	 * not-mapped-during-replay region(s) into account.  And if
	 * not present, tell the user how to make one in a future
	 * run. */
	format_dump_filename(t, global_time, "checksum_error",
			     cur_dump, sizeof(cur_dump));
	format_dump_filename(t, global_time, "rec",
			     rec_dump, sizeof(rec_dump));

	Event ev(t->trace.ev);
	ASSERT(t, checksum == rec_checksum)
<<"Divergence in contents of memory segment after '"<< ev <<"':\n"
"\n"
<< raw_map_line
<<"    (recorded checksum:"<< HEX(rec_checksum)
<<"; replaying checksum:"<< HEX(checksum) <<")\n"
"\n"
<<"Dumped current memory contents to "<< cur_dump <<". If you've created a memory dump for\n"
<<"the '"<< ev <<"' event (line "<< t->trace_time() <<") during recording by using, for example with\n"
<<"the args\n"
"\n"
<<"$ rr --dump-at="<< t->trace_time() <<" record ...\n"
"\n"
<<"then you can use the following to determine which memory cells differ:\n"
"\n"
<<"$ diff -u "<< rec_dump <<" "<< cur_dump <<" > mem-diverge.diff\n";
}

/**
 * This helper does the heavy lifting of storing or validating
 * checksums.  The iterator data determines which behavior the helper
 * function takes on, and to/from which file it writes/read.
 */
enum ChecksumMode { STORE_CHECKSUMS, VALIDATE_CHECKSUMS };
struct checksum_iterator_data {
	ChecksumMode mode;
	FILE* checksums_file;
	int global_time;
};

static int checksum_segment_filter(const Mapping &m,
				   const MappableResource &r)
{
	struct stat st;
	int may_diverge;

	if (stat(r.fsname.c_str(), &st)) {
		/* If there's no persistent resource backing this
		 * mapping, we should expect it to change. */
		LOG(debug) <<"CHECKSUMMING unlinked '"<< r.fsname <<"'";
		return 1;
	}
	/* If we're pretty sure the backing resource is effectively
	 * immutable, skip checksumming, it's a waste of time.  Except
	 * if the mapping is mutable, for example the rw data segment
	 * of a system library, then it's interesting. */
	may_diverge = (should_copy_mmap_region(r.fsname.c_str(), &st,
					       m.prot, m.flags,
					       DONT_WARN_SHARED_WRITEABLE)
		       || (PROT_WRITE & m.prot));
	LOG(debug) << (may_diverge ? "CHECKSUMMING" : "  skipping")
		   <<" '"<< r.fsname <<"'";
	return may_diverge;
}

/**
 * Either create and store checksums for each segment mapped in |t|'s
 * address space, or validate an existing computed checksum.  Behavior
 * is selected by |mode|.
 */
static void iterate_checksums(Task* t, ChecksumMode mode, int global_time)
{
	struct checksum_iterator_data c;
	memset(&c, 0, sizeof(c));
	char filename[PATH_MAX];
	const char* fmode = (STORE_CHECKSUMS == mode) ? "w" : "r";

	c.mode = mode;
	snprintf(filename, sizeof(filename) - 1, "%s/%d_%d",
		 t->trace_dir().c_str(), global_time, t->rec_tid);
	c.checksums_file = fopen64(filename, fmode);
	c.global_time = global_time;
	if (!c.checksums_file) {
		FATAL() <<"Failed to open checksum file "<< filename;
	}

	const AddressSpace& as = *(t->vm());
	for (auto& kv : as.memmap()) {
		const Mapping& first = kv.first;
		const MappableResource& second = kv.second; 

		vector<byte> mem;
		ssize_t valid_mem_len = 0; 

		if (checksum_segment_filter(first, second)) {
			mem.resize(first.num_bytes());
			valid_mem_len = t->read_bytes_fallible(first.start,
			    first.num_bytes(), mem.data());
			valid_mem_len = max(0, valid_mem_len);
		}

		unsigned* buf = (unsigned*)mem.data();
		unsigned checksum = 0;
		int i;

		if (second.fsname.find(SYSCALLBUF_SHMEM_PATH_PREFIX)
				!= string::npos) {
			/* The syscallbuf consists of a region that's written
			* deterministically wrt the trace events, and a
			* region that's written nondeterministically in the
			* same way as trace scratch buffers.  The
			* deterministic region comprises committed syscallbuf
			* records, and possibly the one pending record
			* metadata.  The nondeterministic region starts at
			* the "extra data" for the possibly one pending
			* record.
			*
			* So here, we set things up so that we only checksum
			* the deterministic region. */
			void* child_hdr = first.start;
			struct syscallbuf_hdr hdr;
			t->read_mem(child_hdr, &hdr);
			valid_mem_len = !buf ? 0 :
				sizeof(hdr) + hdr.num_rec_bytes +
				sizeof(struct syscallbuf_record);
		}

		/* If this segment was filtered, then data->mem_len will be 0
		 * to indicate nothing was read.  And data->mem will be NULL
		 * to double-check that.  In that case, the checksum will just
		 * be 0. */
		ASSERT(t, buf || valid_mem_len == 0);
		for (i = 0; i < ssize_t(valid_mem_len / sizeof(*buf)); ++i) {
			checksum += buf[i];
		}

		string raw_map_line = first.str() + ' ' + second.str();
		if (STORE_CHECKSUMS == c.mode) {
			fprintf(c.checksums_file,"(%x) %s\n",
					checksum, raw_map_line.c_str());
		} else {
			char line[1024];
			unsigned rec_checksum;
			void* rec_start_addr;
			void* rec_end_addr;
			int nparsed;

			fgets(line, sizeof(line), c.checksums_file);
			nparsed = sscanf(line, "(%x) %p-%p", &rec_checksum,
					&rec_start_addr, &rec_end_addr);
			ASSERT(t, 3 == nparsed) << "Only parsed "<< nparsed <<" items";

			ASSERT(t, (rec_start_addr == first.start
						&& rec_end_addr == first.end))
				<< "Segment "<< rec_start_addr <<"-"<< rec_end_addr
				<<" changed to "<< first <<"??";

			if (is_start_of_scratch_region(t, rec_start_addr)) {
				/* Replay doesn't touch scratch regions, so
				 * their contents are allowed to diverge.
				 * Tracees can't observe those segments unless
				 * they do something sneaky (or disastrously
				 * buggy). */
				LOG(debug) << "Not validating scratch starting at 0x"
					<< hex << rec_start_addr << dec;
				continue;
			}
			if (checksum != rec_checksum) {
				notify_checksum_error(t, c.global_time,
						checksum, rec_checksum, raw_map_line.c_str());
			}
		}
	}

	fclose(c.checksums_file);
}

int should_checksum(Task* t, const struct trace_frame& f)
{
	int checksum = rr_flags()->checksum;
	int is_syscall_exit = (EV_SYSCALL == f.ev.type
			       && STATE_SYSCALL_EXIT == f.ev.state);

#if defined(FIRST_INTERESTING_EVENT)
	if (is_syscall_exit
	    && FIRST_INTERESTING_EVENT <= global_time
	    && global_time <= LAST_INTERESTING_EVENT) {
		return 1;
	}
	if (global_time > LAST_INTERESTING_EVENT) {
		return 0;
	}
#endif
	if (CHECKSUM_NONE == checksum) {
		return 0;
	}
	if (CHECKSUM_ALL == checksum) {
		return 1;
	}
	if (CHECKSUM_SYSCALL == checksum) {
		return is_syscall_exit;
	}
	/* |checksum| is a global time point. */
	return checksum <= int(f.global_time);
}

void checksum_process_memory(Task* t, int global_time)
{
	iterate_checksums(t, STORE_CHECKSUMS, global_time);
}

void validate_process_memory(Task* t, int global_time)
{
	iterate_checksums(t, VALIDATE_CHECKSUMS, global_time);
}

void cleanup_code_injection(struct current_state_buffer* buf)
{
	free(buf);
}

void copy_syscall_arg_regs(struct user_regs_struct* to,
			   const struct user_regs_struct* from)
{
	to->ebx = from->ebx;
	to->ecx = from->ecx;
	to->edx = from->edx;
	to->esi = from->esi;
	to->edi = from->edi;
	to->ebp = from->ebp;
}

void record_struct_msghdr(Task* t, struct msghdr* child_msghdr)
{
	struct msghdr msg;
	t->read_mem(child_msghdr, &msg);

	// Record the entire struct, because some of the direct fields
	// are written as inoutparams.
	t->record_local(child_msghdr, sizeof(msg), &msg);
	t->record_remote(msg.msg_name, msg.msg_namelen);

	// Read all the inout iovecs in one shot.
	struct iovec iovs[msg.msg_iovlen];
	t->read_bytes_helper(msg.msg_iov,
			     msg.msg_iovlen * sizeof(iovs[0]), (byte*)iovs);
	for (size_t i = 0; i < msg.msg_iovlen; ++i) {
		const struct iovec* iov = &iovs[i];
		t->record_remote(iov->iov_base, iov->iov_len);
	}

	t->record_remote(msg.msg_control, msg.msg_controllen);
}

void record_struct_mmsghdr(Task* t, struct mmsghdr* child_mmsghdr)
{
	/* struct mmsghdr has an inline struct msghdr as its first
	 * field, so it's OK to make this "cast". */
	record_struct_msghdr(t, (struct msghdr*)child_mmsghdr);
	/* We additionally have to record the outparam number of
	 * received bytes. */
	t->record_remote(&child_mmsghdr->msg_len,
			 sizeof(child_mmsghdr->msg_len));
}

void restore_struct_msghdr(Task* t, struct msghdr* child_msghdr)
{
	struct msghdr msg;
	t->read_mem(child_msghdr, &msg);

	// Restore msg itself.
	t->set_data_from_trace();
	// Restore msg.msg_name.
	t->set_data_from_trace();
	// For each iovec arg, restore its recorded data.
	for (size_t i = 0; i < msg.msg_iovlen; ++i) {
		// Restore iov_base buffer.
		t->set_data_from_trace();
	}
	// Restore msg_control buffer.
	t->set_data_from_trace();
}

void restore_struct_mmsghdr(Task* t, struct mmsghdr* child_mmsghdr)
{
	restore_struct_msghdr(t, (struct msghdr*)child_mmsghdr);
	t->set_data_from_trace();
}

bool is_now_contended_pi_futex(Task* t, void* futex, uint32_t* next_val)
{
	static_assert(sizeof(uint32_t) == sizeof(long),
		      "Sorry, need to add Task::read_int()");
	uint32_t val = t->read_word(futex);
	pid_t owner_tid = (val & FUTEX_TID_MASK);
	bool now_contended = (owner_tid != 0 && owner_tid != t->rec_tid
			      && !(val & FUTEX_WAITERS));
	if (now_contended) {
		LOG(debug) << t->tid <<": futex "<< futex <<" is "
			   << val <<", so WAITERS bit will be set";
		*next_val = (owner_tid & FUTEX_TID_MASK) | FUTEX_WAITERS;
	}
	return now_contended;
}

int default_action(int sig)
{
	if (SIGRTMIN <= sig && sig <= SIGRTMAX) {
		return TERMINATE;
	}
	switch (sig) {
		/* TODO: SSoT for signal defs/semantics. */
#define CASE(_sig, _act) case SIG## _sig: return _act
	CASE(HUP, TERMINATE);
	CASE(INT, TERMINATE);
	CASE(QUIT, DUMP_CORE);
	CASE(ILL, DUMP_CORE);
	CASE(ABRT, DUMP_CORE);
	CASE(FPE, DUMP_CORE);
	CASE(KILL, TERMINATE);
	CASE(SEGV, DUMP_CORE);
	CASE(PIPE, TERMINATE);
	CASE(ALRM, TERMINATE);
	CASE(TERM, TERMINATE);
	CASE(USR1, TERMINATE);
	CASE(USR2, TERMINATE);
	CASE(CHLD, IGNORE);
	CASE(CONT, CONTINUE);
	CASE(STOP, STOP);
	CASE(TSTP, STOP);
	CASE(TTIN, STOP);
	CASE(TTOU, STOP);
	CASE(BUS, DUMP_CORE);
	/*CASE(POLL, TERMINATE);*/
	CASE(PROF, TERMINATE);
	CASE(SYS, DUMP_CORE);
	CASE(TRAP, DUMP_CORE);
	CASE(URG, IGNORE);
	CASE(VTALRM, TERMINATE);
	CASE(XCPU, DUMP_CORE);
	CASE(XFSZ, DUMP_CORE);
	/*CASE(IOT, DUMP_CORE);*/
	/*CASE(EMT, TERMINATE);*/
	CASE(STKFLT, TERMINATE);
	CASE(IO, TERMINATE);
	CASE(PWR, TERMINATE);
	/*CASE(LOST, TERMINATE);*/
	CASE(WINCH, IGNORE);
	default:
		FATAL() <<"Unknown signal "<< sig;
		return -1;	// not reached
#undef CASE
	}
}

bool possibly_destabilizing_signal(Task* t, int sig)
{
	sig_handler_t disp = t->signal_disposition(sig);
	int action = default_action(sig);
	// If the diposition is IGN or user handler, then the signal
	// won't be fatal.  So we only need to check for DFL.
	return SIG_DFL == disp && (DUMP_CORE == action || TERMINATE == action);
}

static bool has_fs_name(const char* path)
{
	struct stat dummy;
	return 0 == stat(path, &dummy);
}

static bool is_tmp_file(const char* path)
{
	struct statfs sfs;
	statfs(path, &sfs);
	return (TMPFS_MAGIC == sfs.f_type
		// In observed configurations of Ubuntu 13.10, /tmp is
		// a folder in the / fs, not a separate tmpfs.
		|| path == strstr(path, "/tmp/"));
}

bool should_copy_mmap_region(const char* filename, const struct stat* stat,
			     int prot, int flags,
			     int warn_shared_writeable)
{
	bool private_mapping = (flags & MAP_PRIVATE);

	// TODO: handle mmap'd files that are unlinked during
	// recording.
	if (!has_fs_name(filename)) {
		LOG(debug) <<"  copying unlinked file";
		return true;
	}
	if (is_tmp_file(filename)) {
		LOG(debug) <<"  copying file on tmpfs";
		return true;
	}
	if (private_mapping && (prot & PROT_EXEC)) {
		/* We currently don't record the images that we
		 * exec(). Since we're being optimistic there (*cough*
		 * *cough*), we're doing no worse (in theory) by being
		 * optimistic about the shared libraries too, most of
		 * which are system libraries. */
		LOG(debug) <<"  (no copy for +x private mapping "<< filename <<")";
		return false;
	}
	if (private_mapping && (0111 & stat->st_mode)) {
		/* A private mapping of an executable file usually
		 * indicates mapping data sections of object files.
		 * Since we're already assuming those change very
		 * infrequently, we can avoid copying the data
		 * sections too. */
		LOG(debug) <<"  (no copy for private mapping of +x "<< filename <<")";
		return false;
	}

	// TODO: using "can the euid of the rr process write this
	// file" as an approximation of whether the tracee can write
	// the file.  If the tracee is messing around with
	// set*[gu]id(), the real answer may be different.
	bool can_write_file = (0 == access(filename, W_OK));

	if (!can_write_file && 0 == stat->st_uid) {
		// We would like to assert this, but on Ubuntu 13.10,
		// the file /lib/i386-linux-gnu/libdl-2.17.so is
		// writeable by root for unknown reasons.
		//assert(!(prot & PROT_WRITE));
		/* Mapping a file owned by root: we don't care if this
		 * was a PRIVATE or SHARED mapping, because unless the
		 * program is disastrously buggy or unlucky, the
		 * mapping is effectively PRIVATE.  Bad luck can come
		 * from this program running during a system update,
		 * or a user being added, which is probably less
		 * frequent than even system updates.
		 *
		 * XXX what about the fontconfig cache files? */
		LOG(debug) <<"  (no copy for root-owned "<< filename <<")";
		return false;
	}
	if (private_mapping) {
		/* Some programs (at least Firefox) have been observed
		 * to use cache files that are expected to be
		 * consistent and unchanged during the bulk of
		 * execution, but may be destroyed or mutated at
		 * shutdown in preparation for the next session.  We
		 * don't otherwise know what to do with private
		 * mappings, so err on the safe side.
		 *
		 * TODO: could get into dirty heuristics here like
		 * trying to match "cache" in the filename ...	 */
		LOG(debug) <<"  copying private mapping of non-system -x "
			   << filename;
		return true;
	}
	if (!(0222 & stat->st_mode)) {
		/* We couldn't write the file because it's read only.
		 * But it's not a root-owned file (therefore not a
		 * system file), so it's likely that it could be
		 * temporary.  Copy it. */
		LOG(debug) <<"  copying read-only, non-system file";
		return true;
	}
	if (!can_write_file) {
		/* mmap'ing another user's (non-system) files?  Highly
		 * irregular ... */
		FATAL() <<"Unhandled mmap "<< filename <<"(prot:"
			<< HEX(prot) << ((flags & MAP_SHARED) ? ";SHARED" : "")
			<<"); uid:"<< stat->st_uid <<" mode:"<< stat->st_mode;
	}
	/* Shared mapping that we can write.  Should assume that the
	 * mapping is likely to change. */
	LOG(debug) <<"  copying writeable SHARED mapping "<< filename;
	if (PROT_WRITE | prot) {
		if (warn_shared_writeable) {
			LOG(debug) << filename <<" is SHARED|WRITEABLE; that's not handled correctly yet. Optimistically hoping it's not written by programs outside the rr tracee tree.";
		}
	}
	return true;
}

int create_shmem_segment(const char* name, size_t num_bytes, int cloexec)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path) - 1, "%s/%s", SHMEM_FS, name);

	int fd = open(path, O_CREAT | O_EXCL | O_RDWR | cloexec, 0600);
	if (0 > fd) {
		FATAL() <<"Failed to create shmem segment "<< path;
	}
	/* Remove the fs name so that we don't have to worry about
	 * cleaning up this segment in error conditions. */
	unlink(path);
	resize_shmem_segment(fd, num_bytes);

	LOG(debug) <<"created shmem segment "<< path;
	return fd;
}

void resize_shmem_segment(int fd, size_t num_bytes)
{
	if (ftruncate(fd, num_bytes)) {
		FATAL() <<"Failed to resize shmem to "<< num_bytes;
	}
}

void prepare_remote_syscalls(Task* t, struct current_state_buffer* state)
{
	/* Save current state of |t|. */
	memset(state, 0, sizeof(*state));
	state->pid = t->tid;
	state->regs = t->regs();
	state->code_size = sizeof(syscall_insn);
	state->start_addr = (byte*)state->regs.eip;
	t->read_bytes(state->start_addr, state->code_buffer);
	/* Inject phony syscall instruction. */
	t->write_bytes(state->start_addr, syscall_insn);
}

void* push_tmp_mem(Task* t, struct current_state_buffer* state,
		   const byte* mem, ssize_t num_bytes,
		   struct restore_mem* restore)
{
	restore->len = num_bytes;
	restore->saved_sp = (void*)state->regs.esp;

	state->regs.esp -= restore->len;
	t->set_regs(state->regs);
	restore->addr = (void*)state->regs.esp;

	restore->data = (byte*)malloc(restore->len);
	t->read_bytes_helper(restore->addr, restore->len, restore->data);

	t->write_bytes_helper(restore->addr, restore->len, mem);

	return restore->addr;
}

void* push_tmp_str(Task* t, struct current_state_buffer* state,
		   const char* str, struct restore_mem* restore)
{
	return push_tmp_mem(t, state,
			    (const byte*)str, strlen(str) + 1/*null byte*/,
			    restore);
}

void pop_tmp_mem(Task* t, struct current_state_buffer* state,
		 struct restore_mem* mem)
{
	assert(mem->saved_sp == (byte*)state->regs.esp + mem->len);

	t->write_bytes_helper(mem->addr, mem->len, mem->data);
	free(mem->data);

	state->regs.esp += mem->len;
	t->set_regs(state->regs);
}

// XXX this is probably dup'd somewhere else
static void advance_syscall(Task* t)
{
	do {
		t->cont_syscall();
	} while (t->is_ptrace_seccomp_event() || SIGCHLD == t->pending_sig());
	assert(t->ptrace_event() == 0);
}

long remote_syscall(Task* t, struct current_state_buffer* state,
		    int wait, int syscallno,
		    long a1, long a2, long a3, long a4, long a5, long a6)
{
	assert(t->tid == state->pid);

	/* Prepare syscall arguments. */
	struct user_regs_struct callregs = state->regs;
	callregs.eax = syscallno;
	callregs.ebx = a1;
	callregs.ecx = a2;
	callregs.edx = a3;
	callregs.esi = a4;
	callregs.edi = a5;
	callregs.ebp = a6;
	t->set_regs(callregs);

	advance_syscall(t);

	ASSERT(t, t->regs().orig_eax == syscallno)
		<<"Should be entering "<< syscallname(syscallno)
		<<", but instead at "<< syscallname(callregs.orig_eax);

	/* Start running the syscall. */
	t->cont_syscall_nonblocking();
	if (WAIT == wait) {
		return wait_remote_syscall(t, state, syscallno);
	}
	return 0;
}

long wait_remote_syscall(Task* t, struct current_state_buffer* state,
			 int syscallno)
{
	struct user_regs_struct regs;
	/* Wait for syscall-exit trap. */
	t->wait();

	ASSERT(t, t->regs().orig_eax == syscallno)
		<<"Should be entering "<< syscallname(syscallno)
		<<", but instead at "<< syscallname(regs.orig_eax);

	return t->regs().eax;
}

void finish_remote_syscalls(Task* t, struct current_state_buffer* state)
{
	pid_t tid = t->tid;

	assert(tid == state->pid);

	/* Restore stomped instruction. */
	t->write_bytes(state->start_addr, state->code_buffer);

	/* Restore stomped registers. */
	t->set_regs(state->regs);
}

void destroy_buffers(Task* t, int flags)
{
	// NB: we have to pay all this complexity here because glibc
	// makes its SYS_exit call through an inline int $0x80 insn,
	// instead of going through the vdso.  There may be a deep
	// reason for why it does that, but if it starts going through
	// the vdso in the future, this code can be eliminated in
	// favor of a *much* simpler vsyscall SYS_exit hook in the
	// preload lib.

	if (!(DESTROY_ALREADY_AT_EXIT_SYSCALL & flags)) {
		// Advance the tracee into SYS_exit so that it's at a
		// known state that we can manipulate it from.
		advance_syscall(t);
	}

	struct user_regs_struct exit_regs = t->regs();
	ASSERT(t, SYS_exit == exit_regs.orig_eax)
		<< "Tracee should have been at exit, but instead at "
		<< syscallname(exit_regs.orig_eax);

	// The tracee is at the entry to SYS_exit, but hasn't started
	// the call yet.  We can't directly start injecting syscalls
	// because the tracee is still in the kernel.  And obviously,
	// if we finish the SYS_exit syscall, the tracee isn't around
	// anymore.
	//
	// So hijack this SYS_exit call and rewrite it into a harmless
	// one that we can exit successfully, SYS_gettid here (though
	// that choice is arbitrary).
	exit_regs.orig_eax = SYS_gettid;
	t->set_regs(exit_regs);
	// This exits the hijacked SYS_gettid.  Now the tracee is
	// ready to do our bidding.
	advance_syscall(t);

	// Restore these regs to what they would have been just before
	// the tracee trapped at SYS_exit.  When we've finished
	// cleanup, we'll restart the SYS_exit call.
	exit_regs.orig_eax = -1;
	exit_regs.eax = SYS_exit;
	exit_regs.eip -= sizeof(syscall_insn);

	byte insn[sizeof(syscall_insn)];
	t->read_bytes((void*)exit_regs.eip, insn);
	ASSERT(t, !memcmp(insn, syscall_insn, sizeof(insn)))
		<<"Tracee should have entered through int $0x80.";

	// Do the actual buffer and fd cleanup.
	t->destroy_buffers(DESTROY_SCRATCH | DESTROY_SYSCALLBUF);

	// Prepare to restart the SYS_exit call.
	t->set_regs(exit_regs);
	if (DESTROY_NEED_EXIT_SYSCALL_RESTART & flags) {
		advance_syscall(t);
	}
}

static const byte vsyscall_impl[] = {
    0x51,                       /* push %ecx */
    0x52,                       /* push %edx */
    0x55,                       /* push %ebp */
    0x89, 0xe5,                 /* mov %esp,%ebp */
    0x0f, 0x34,                 /* sysenter */
    0x90,                       /* nop */
    0x90,                       /* nop */
    0x90,                       /* nop */
    0x90,                       /* nop */
    0x90,                       /* nop */
    0x90,                       /* nop */
    0x90,                       /* nop */
    0xcd, 0x80,                 /* int $0x80 */
    0x5d,                       /* pop %ebp */
    0x5a,                       /* pop %edx */
    0x59,                       /* pop %ecx */
    0xc3,                       /* ret */
};

/**
 * Return true iff |addr| points to a known |__kernel_vsyscall()|
 * implementation.
 */
static bool is_kernel_vsyscall(Task* t, void* addr)
{
	byte impl[sizeof(vsyscall_impl)];
	t->read_bytes(addr, impl);
	for (size_t i = 0; i < sizeof(vsyscall_impl); ++i) {
		if (vsyscall_impl[i] != impl[i]) {
			LOG(warn) <<"Byte "<< i <<" of __kernel_vsyscall should be "
				  << HEX(vsyscall_impl[i]) <<" but is "
				  << HEX(impl[i]);
			return false;
		}
	}
	return true;
}

/**
 * Return the address of a recognized |__kernel_vsyscall()|
 * implementation in |t|'s address space.
 */
static void* locate_and_verify_kernel_vsyscall(Task* t)
{
	void* vdso_start = t->vm()->vdso().start;
	// __kernel_vsyscall() has been observed to be mapped at the
	// following offsets from the vdso start address.  We'll try
	// to recognize a known __kernel_vsyscall() impl at any of
	// these offsets.
	const ssize_t known_offsets[] = {
		0x414,		// x86 native kernel ca. 3.12.10-300
		0x420		// x86 process on x64 kernel
	};
	for (size_t i = 0; i < ALEN(known_offsets); ++i) {
		void* addr = (byte*)vdso_start + known_offsets[i];
		if (is_kernel_vsyscall(t, addr)) {
			return addr;
		}
	}
	return nullptr;
}

// NBB: the placeholder bytes in |struct insns_template| below must be
// kept in sync with this.
static const byte vsyscall_monkeypatch[] = {
	0x50,                         // push %eax
	0xb8, 0x00, 0x00, 0x00, 0x00, // mov $_vsyscall_hook_trampoline, %eax
	// The immediate param of the |mov| is filled in dynamically
	// by the template mechanism below.  The NULL here is a
	// placeholder.
	0xff, 0xe0,		// jmp *%eax
};

struct insns_template {
	// NBB: |vsyscall_monkeypatch| must be kept in sync with these
	// placeholder bytes.
	byte push_eax_insn;
	byte mov_vsyscall_hook_trampoline_eax_insn;
	void* vsyscall_hook_trampoline;
} __attribute__((packed));

/**
 * Monkeypatch |t|'s |__kernel_vsyscall()| helper to jump to
 * |vsyscall_hook_trampoline|.
 */
static void monkeypatch(Task* t, void* kernel_vsyscall,
			void* vsyscall_hook_trampoline)
{
	union {
		byte bytes[sizeof(vsyscall_monkeypatch)];
		struct insns_template insns;
	} __attribute__((packed)) patch;
	// Write the basic monkeypatch onto to the template, except
	// for the (dynamic) $vsyscall_hook_trampoline address.
	memcpy(patch.bytes, vsyscall_monkeypatch, sizeof(patch.bytes));
	// (Try to catch out-of-sync |vsyscall_monkeypatch| and
	// |struct insns_template|.)
	assert(nullptr == patch.insns.vsyscall_hook_trampoline);
	patch.insns.vsyscall_hook_trampoline = vsyscall_hook_trampoline;

	t->write_bytes(kernel_vsyscall, patch.bytes);
	LOG(debug) <<"monkeypatched __kernel_vsyscall to jump to "
		   << vsyscall_hook_trampoline;
}

void monkeypatch_vdso(Task* t)
{
	void* kernel_vsyscall = locate_and_verify_kernel_vsyscall(t);
	if (!kernel_vsyscall) {
		FATAL() <<
"Failed to monkeypatch vdso: your __kernel_vsyscall() wasn't recognized.\n"
"    Syscall buffering is now effectively disabled.  If you're OK with\n"
"    running rr without syscallbuf, then run the recorder passing the\n"
"    --no-syscall-buffer arg.\n"
"    If you're *not* OK with that, file an issue.";
	}

	LOG(debug) <<"__kernel_vsyscall is "<< kernel_vsyscall;

	ASSERT(t, 1 == t->vm()->task_set().size())
		<<"TODO: monkeypatch multithreaded process";

	// NB: the tracee can't be interrupted with a signal while
	// we're processing the rrcall, because it's masked off all
	// signals.
	void* vsyscall_hook_trampoline = (void*)t->regs().ebx;
	// Luckily, linux is happy for us to scribble directly over
	// the vdso mapping's bytes without mprotecting the region, so
	// we don't need to prepare remote syscalls here.
	monkeypatch(t, kernel_vsyscall, vsyscall_hook_trampoline);

	struct user_regs_struct r = t->regs();
	r.eax = 0;
	t->set_regs(r);
}
