#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "libnagios.h"

struct execution_information {
	squeue_event *sq_event;
	pid_t pid;
	struct timeval start;
	struct timeval stop;
	float runtime;
	struct rusage rusage;
};

static iobroker_set *iobs;
static squeue_t *sq;
static unsigned int started, running_jobs;
static int master_sd;
static int parent_pid;

/*
 * contains all information sent in a particular request
 */
struct request {
	char *cmd;
	int when;
	char **env;
	struct kvvec *request, *response;
};

static void exit_worker(int code, const char *msg)
{
	child_process *cp;
	int discard;

	if (msg) {
		perror(msg);
	}

	/*
	 * We must kill our children, so let's embark on that
	 * large scale filicide. First SIGTERM to all processes
	 * in our group, and then SIGKILL to every item in our
	 * event queue
	 */
	signal(SIGTERM, SIG_IGN);
	kill(0, SIGTERM);
	while (waitpid(-1, &discard, WNOHANG) > 0)
		; /* do nothing */
	sleep(1);
	while ((cp = (child_process *)squeue_pop(sq))) {
		(void)kill(cp->ei->pid, SIGKILL);
	}
	while (waitpid(-1, &discard, WNOHANG) > 0)
		; /* do nothing */

	exit(code);
}

/*
 * write a log message to master.
 * Note that this will break if we change delimiters someday,
 * but avoids doing several extra malloc()+free() for this
 * pretty simple case.
 */
static void wlog(const char *fmt, ...)
{
	va_list ap;
	static char lmsg[8192] = "log=";
	int len = 4, to_send;

	va_start(ap, fmt);
	len = vsnprintf(&lmsg[len], sizeof(lmsg) - 7, fmt, ap);
	va_end(ap);
	if (len < 0 || len >= sizeof(lmsg))
		return;

	len += 4; /* log= */

	/* add delimiter and send it. 1 extra as kv pair separator */
	to_send = len + MSG_DELIM_LEN_SEND + 1;
	lmsg[len] = 0;
	memcpy(&lmsg[len + 1], MSG_DELIM, MSG_DELIM_LEN_SEND);
	if (write(master_sd, lmsg, to_send) < 0) {
		if (errno == EPIPE) {
			/* master has died or abandoned us, so exit */
			exit_worker(1, "Failed to write() to master");
		}
	}
}

static void job_error(child_process *cp, struct kvvec *kvv, const char *fmt, ...)
{
	char msg[4096];
	int len;
	va_list ap;
	int ret;

	va_start(ap, fmt);
	len = vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
	va_end(ap);
	if (cp) {
		kvvec_addkv(kvv, "job_id", (char *)mkstr("%d", cp->id));
	}
	kvvec_addkv_wlen(kvv, "error_msg", 9, msg, len);
	ret = send_kvvec(master_sd, kvv);
	if (ret < 0 && errno == EPIPE)
		exit_worker(1, "Failed to send job error key/value vector to master");
	kvvec_destroy(kvv, 0);
}

int tv_delta_msec(const struct timeval *start, const struct timeval *stop)
{
	int msecs;
	unsigned long usecs = 0;

	msecs = (stop->tv_sec - start->tv_sec) * 1000;
	if (stop->tv_usec < start->tv_usec) {
		msecs -= 1000;
		usecs += 1000000;
	}
	usecs += stop->tv_usec - start->tv_usec;
	msecs += (usecs / 1000);

	return msecs;
}

float tv_delta_f(const struct timeval *start, const struct timeval *stop)
{
#define DIVIDER 1000000
	float ret;
	unsigned long usecs, stop_usec;

	ret = stop->tv_sec - start->tv_sec;
	stop_usec = stop->tv_usec;
	if (stop_usec < start->tv_usec) {
		ret -= 1.0;
		stop_usec += DIVIDER;
	}
	usecs = stop_usec - start->tv_usec;

	ret += (float)((float)usecs / DIVIDER);
	return ret;
}

#define MKSTR_BUFS 256 /* should be plenty */
const char *mkstr(const char *fmt, ...)
{
	static char buf[MKSTR_BUFS][32]; /* 8k statically on the stack */
	static int slot = 0;
	char *ret;
	va_list ap;

	ret = buf[slot++ & (MKSTR_BUFS - 1)];
	va_start(ap, fmt);
	vsnprintf(ret, sizeof(buf[0]), fmt, ap);
	va_end(ap);
	return ret;
}

struct kvvec_buf *build_kvvec_buf(struct kvvec *kvv)
{
	struct kvvec_buf *kvvb;

	/*
	 * key=value, separated by PAIR_SEP and messages
	 * delimited by MSG_DELIM
	 */
	kvvb = kvvec2buf(kvv, KV_SEP, PAIR_SEP, MSG_DELIM_LEN_SEND);
	if (!kvvb) {
		return NULL;
	}
	memcpy(kvvb->buf + (kvvb->bufsize - MSG_DELIM_LEN_SEND), MSG_DELIM, MSG_DELIM_LEN_SEND);

	return kvvb;
}

int send_kvvec(int sd, struct kvvec *kvv)
{
	int ret;
	struct kvvec_buf *kvvb;

	kvvb = build_kvvec_buf(kvv);
	if (!kvvb)
		return -1;

	/* bufsize, not buflen, as it gets us the delimiter */
	ret = write(sd, kvvb->buf, kvvb->bufsize);
	free(kvvb->buf);
	free(kvvb);

	return ret;
}

#define kvvec_add_long(kvv, key, value) \
	do { \
		char *buf = (char *)mkstr("%ld", value); \
		kvvec_addkv_wlen(kvv, key, sizeof(key) - 1, buf, strlen(buf)); \
	} while (0)

#define kvvec_add_tv(kvv, key, value) \
	do { \
		char *buf = (char *)mkstr("%ld.%06ld", value.tv_sec, value.tv_usec); \
		kvvec_addkv_wlen(kvv, key, sizeof(key) - 1, buf, strlen(buf)); \
	} while (0)

int finish_job(child_process *cp, int reason)
{
	static struct kvvec resp = KVVEC_INITIALIZER;
	struct rusage *ru = &cp->ei->rusage;
	int i, ret;

	/* how many key/value pairs do we need? */
	if (kvvec_init(&resp, 12 + cp->request->kv_pairs) == NULL) {
		/* what the hell do we do now? */
		exit_worker(1, "Failed to init response key/value vector");
	}

	gettimeofday(&cp->ei->stop, NULL);

	if (running_jobs != squeue_size(sq)) {
		wlog("running_jobs(%d) != squeue_size(sq) (%d)\n",
			 running_jobs, squeue_size(sq));
		wlog("started: %d; running: %d; finished: %d\n",
			 started, running_jobs, started - running_jobs);
	}

	/*
	 * we must remove the job's timeout ticker,
	 * or we'll end up accessing an already free()'d
	 * pointer, or the pointer to a different child.
	 */
	squeue_remove(sq, cp->ei->sq_event);
	running_jobs--;

	/* get rid of still open filedescriptors */
	if (cp->outstd.fd != -1)
		iobroker_close(iobs, cp->outstd.fd);
	if (cp->outerr.fd != -1)
		iobroker_close(iobs, cp->outerr.fd);

	cp->ei->runtime = tv_delta_f(&cp->ei->start, &cp->ei->stop);

	/*
	 * Now build the return message.
	 * First comes the request, minus environment variables
	 */
	for (i = 0; i < cp->request->kv_pairs; i++) {
		struct key_value *kv = &cp->request->kv[i];
		/* skip environment macros */
		if (kv->key_len == 3 && !strcmp(kv->key, "env")) {
			continue;
		}
		kvvec_addkv_wlen(&resp, kv->key, kv->key_len, kv->value, kv->value_len);
	}
	kvvec_addkv(&resp, "wait_status", (char *)mkstr("%d", cp->ret));
	kvvec_addkv_wlen(&resp, "outstd", 6, cp->outstd.buf, cp->outstd.len);
	kvvec_addkv_wlen(&resp, "outerr", 6, cp->outerr.buf, cp->outerr.len);
	kvvec_add_tv(&resp, "start", cp->ei->start);
	kvvec_add_tv(&resp, "stop", cp->ei->stop);
	kvvec_addkv(&resp, "runtime", (char *)mkstr("%f", cp->ei->runtime));
	if (!reason) {
		/* child exited nicely */
		kvvec_addkv(&resp, "exited_ok", "1");
		kvvec_add_tv(&resp, "ru_utime", ru->ru_utime);
		kvvec_add_tv(&resp, "ru_stime", ru->ru_stime);
		kvvec_add_long(&resp, "ru_minflt", ru->ru_minflt);
		kvvec_add_long(&resp, "ru_majflt", ru->ru_majflt);
		kvvec_add_long(&resp, "ru_inblock", ru->ru_inblock);
		kvvec_add_long(&resp, "ru_oublock", ru->ru_oublock);
	} else {
		/* some error happened */
		kvvec_addkv(&resp, "exited_ok", "0");
		kvvec_addkv(&resp, "error_code", (char *)mkstr("%d", reason));
	}
	ret = send_kvvec(master_sd, &resp);
	if (ret < 0 && errno == EPIPE)
		exit_worker(1, "Failed to send_kvvec()");

	if (cp->outstd.buf) {
		free(cp->outstd.buf);
		cp->outstd.buf = NULL;
	}
	if (cp->outerr.buf) {
		free(cp->outerr.buf);
		cp->outerr.buf = NULL;
	}

	kvvec_destroy(cp->request, KVVEC_FREE_ALL);
	free(cp->cmd);
	free(cp->ei);
	free(cp);

	return 0;
}

static int check_completion(child_process *cp, int flags)
{
	int result, status;
	time_t max_time;

	if (!cp || !cp->ei->pid) {
		return 0;
	}

	max_time = time(NULL) + 1;

	/*
	 * we mustn't let EINTR interrupt us, since it could well
	 * be a SIGCHLD from the properly exiting process doing it
	 */
	do {
		errno = 0;
		result = wait4(cp->ei->pid, &status, flags, &cp->ei->rusage);
	} while (result == -1 && errno == EINTR && time(NULL) < max_time);

	if (result == cp->ei->pid) {
		cp->ret = status;
		finish_job(cp, 0);
		return 0;
	}

	if (errno == ECHILD) {
		cp->ret = status;
		finish_job(cp, errno);
	}

	return -errno;
}

static void kill_job(child_process *cp, int reason)
{
	int ret;
	struct rusage ru;

	if (!cp->ei->pid) {
		wlog("No pid for job %d (%u running); '%s'", cp->id, running_jobs, cp->cmd);
		return;
	}

	/* brutal but efficient */
	ret = kill(cp->ei->pid, SIGKILL);
	if (ret < 0) {
		if (errno == ESRCH) {
			finish_job(cp, reason);
			return;
		}
		wlog("kill(%d, SIGKILL) failed: %s\n", cp->ei->pid, strerror(errno));
	}

	ret = wait4(cp->ei->pid, &cp->ret, 0, &ru);
	finish_job(cp, reason);

#ifdef PLAY_NICE_IN_kill_job
	int i, sig = SIGTERM;

	pid = cp->ei->pid;

	for (i = 0; i < 2; i++) {
		/* check one last time if the job is done */
		ret = check_completion(cp, WNOHANG);
		if (!ret || ret == -ECHILD) {
			/* check_completion ran finish_job() */
			return;
		}

		/* not done, so signal it. SIGTERM first and check again */
		errno = 0;
		ret = kill(pid, sig);
		if (ret < 0) {
			finish_job(cp, -errno);
		}
		sig = SIGKILL;
		check_completion(cp, WNOHANG);
		if (ret < 0) {
			finish_job(cp, errno);
		}
		usleep(50000);
	}
#endif /* PLAY_NICE_IN_kill_job */
}

static void gather_output(child_process *cp, iobuf *io)
{
	iobuf *other_io;

	other_io = io == &cp->outstd ? &cp->outerr : &cp->outstd;

	for (;;) {
		char buf[4096];
		int rd;

		rd = read(io->fd, buf, sizeof(buf));
		if (rd < 0) {
			if (errno == EINTR)
				continue;
			/* XXX: handle the error somehow */
			check_completion(cp, WNOHANG);
		}

		if (rd) {
			/* we read some data */
			io->buf = realloc(io->buf, rd + io->len + 1);
			memcpy(&io->buf[io->len], buf, rd);
			io->len += rd;
			io->buf[io->len] = '\0';
		} else {
			iobroker_close(iobs, io->fd);
			io->fd = -1;
			if (other_io->fd < 0) {
				check_completion(cp, 0);
			} else {
				check_completion(cp, WNOHANG);
			}
		}
		break;
	}
}


static int stderr_handler(int fd, int events, void *cp_)
{
	child_process *cp = (child_process *)cp_;
	gather_output(cp, &cp->outerr);
	return 0;
}

static int stdout_handler(int fd, int events, void *cp_)
{
	child_process *cp = (child_process *)cp_;
	gather_output(cp, &cp->outstd);
	return 0;
}

int start_cmd(child_process *cp)
{
	int pfd[2] = {-1, -1}, pfderr[2] = {-1, -1};

	cp->outstd.fd = runcmd_open(cp->cmd, pfd, pfderr, NULL);
	if (cp->outstd.fd < 0) {
		return -1;
	}

	cp->outerr.fd = pfderr[0];
	cp->ei->pid = runcmd_pid(cp->outstd.fd);
	/* no pid means we somehow failed */
	if (!cp->ei->pid) {
		return -1;
	}
	iobroker_register(iobs, cp->outstd.fd, cp, stdout_handler);
	iobroker_register(iobs, cp->outerr.fd, cp, stderr_handler);

	return 0;
}

static iocache *ioc;

child_process *parse_command_kvvec(struct kvvec *kvv)
{
	int i;
	child_process *cp;

	/* get this command's struct and insert it at the top of the list */
	cp = calloc(1, sizeof(*cp));
	if (!cp) {
		wlog("Failed to calloc() a child_process struct");
		return NULL;
	}
	cp->ei = calloc(1, sizeof(*cp->ei));
	if (!cp->ei) {
		wlog("Failed to calloc() a execution_information struct");
		return NULL;
	}

	/*
	 * we must copy from the vector, since it points to data
	 * found in the iocache where we read the command, which will
	 * be overwritten when we receive one next
	 */
	for (i = 0; i < kvv->kv_pairs; i++) {
		struct key_value *kv = &kvv->kv[i];
		char *key = kv->key;
		char *value = kv->value;
		char *endptr;

		if (!strcmp(key, "command")) {
			cp->cmd = strdup(value);
			continue;
		}
		if (!strcmp(key, "job_id")) {
			cp->id = (unsigned int)strtoul(value, &endptr, 0);
			continue;
		}
		if (!strcmp(key, "timeout")) {
			cp->timeout = (unsigned int)strtoul(value, &endptr, 0);
			continue;
		}
	}

	/* jobs without a timeout get a default of 60 seconds. */
	if (!cp->timeout) {
		cp->timeout = 60;
	}

	return cp;
}

static void spawn_job(struct kvvec *kvv, int(*cb)(child_process *))
{
	int result;
	child_process *cp;

	if (!kvv) {
		wlog("Received NULL command key/value vector. Bug in iocache.c or kvvec.c?");
		return;
	}

	cp = parse_command_kvvec(kvv);
	if (!cp) {
		job_error(NULL, kvv, "Failed to parse worker-command");
		return;
	}
	if (!cp->cmd) {
		job_error(cp, kvv, "Failed to parse commandline. Ignoring job %u", cp->id);
		return;
	}

	gettimeofday(&cp->ei->start, NULL);
	cp->request = kvv;
	cp->ei->sq_event = squeue_add(sq, cp->timeout + time(NULL), cp);
	started++;
	running_jobs++;
	result = cb(cp);
	if (result < 0) {
		job_error(cp, kvv, "Failed to start child: %s: %s", runcmd_strerror(result), strerror(errno));
		squeue_remove(sq, cp->ei->sq_event);
		running_jobs--;
		return;
	}
}

static int receive_command(int sd, int events, void *arg)
{
	int ioc_ret;
	char *buf;
	unsigned long size;

	if (!ioc) {
		ioc = iocache_create(512 * 1024);
	}
	ioc_ret = iocache_read(ioc, sd);

	/* master closed the connection, so we exit */
	if (ioc_ret == 0) {
		iobroker_close(iobs, sd);
		exit_worker(0, NULL);
	}
	if (ioc_ret < 0) {
		/* XXX: handle this somehow */
	}

#if 0
	/* debug-volley */
	buf = iocache_use_size(ioc, ioc_ret);
	write(master_sd, buf, ioc_ret);
	return 0;
#endif
	/*
	 * now loop over all inbound messages in the iocache.
	 * Since KV_TERMINATOR is a nul-byte, they're separated by 3 nuls
	 */
	while ((buf = iocache_use_delim(ioc, MSG_DELIM, MSG_DELIM_LEN_RECV, &size))) {
		struct kvvec *kvv;
		/* we must copy vars here, as we preserve them for the response */
		kvv = buf2kvvec(buf, (unsigned int)size, KV_SEP, PAIR_SEP, KVVEC_COPY);
		if (kvv)
			spawn_job(kvv, arg);
	}

	return 0;
}

int set_socket_options(int sd, int bufsize)
{
	int ret;

	ret = fcntl(sd, F_SETFD, FD_CLOEXEC);
	ret |= fcntl(sd, F_SETFL, O_NONBLOCK);

	if (!bufsize)
		return ret;
	ret |= setsockopt(sd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(int));
	ret |= setsockopt(sd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(int));

	return ret;
}


void enter_worker(int sd, int (*cb)(child_process*))
{
	/* created with socketpair(), usually */
	master_sd = sd;
	parent_pid = getppid();
	(void)chdir("/tmp");
	(void)chdir("nagios-workers");

	if (setpgid(0, 0)) {
		/* XXX: handle error somehow, or maybe just ignore it */
	}

	/* we need to catch child signals the default way */
	signal(SIGCHLD, SIG_DFL);

	fcntl(fileno(stdout), F_SETFD, FD_CLOEXEC);
	fcntl(fileno(stderr), F_SETFD, FD_CLOEXEC);
	fcntl(master_sd, F_SETFD, FD_CLOEXEC);
	iobs = iobroker_create();
	if (!iobs) {
		/* XXX: handle this a bit better */
		exit_worker(EXIT_FAILURE, "Worker failed to create io broker socket set");
	}

	/*
	 * Create a modest scheduling queue that will be
	 * more than enough for our needs
	 */
	sq = squeue_create(1024);
	set_socket_options(master_sd, 256 * 1024);

	iobroker_register(iobs, master_sd, cb, receive_command);
	while (iobroker_get_num_fds(iobs) > 0) {
		int poll_time = -1;

		/* check for timed out jobs */
		while (running_jobs) {
			child_process *cp;
			struct timeval now, tmo;

			/* stop when scheduling queue is empty */
			cp = (child_process *)squeue_peek(sq);
			if (!cp)
				break;

			tmo.tv_usec = cp->ei->start.tv_usec;
			tmo.tv_sec = cp->ei->start.tv_sec + cp->timeout;
			gettimeofday(&now, NULL);
			poll_time = tv_delta_msec(&now, &tmo);
			/*
			 * A little extra takes care of rounding errors and
			 * ensures we never kill a job before it times out.
			 * 5 milliseconds is enough to take care of that.
			 */
			poll_time += 5;
			if (poll_time > 0)
				break;

			/* this job timed out, so kill it */
			wlog("job with pid %d timed out. Killing it", cp->ei->pid);
			kill_job(cp, ETIME);
		}

		iobroker_poll(iobs, poll_time);
	}

	/* we exit when the master shuts us down */
	exit(EXIT_SUCCESS);
}

int spawn_named_helper(char *path, char **argv)
{
	int ret, pid;

	pid = fork();
	if (pid < 0) {
		return -1;
	}

	/* parent leaves early */
	if (pid)
		return pid;

	ret = execvp(path, argv);
	/* if execvp() fails, there's really nothing we can do */
	exit(ret);
}

int spawn_helper(char **argv)
{
	return spawn_named_helper(argv[0], argv);
}

struct worker_process *spawn_worker(void (*init_func)(void *), void *init_arg)
{
	int sv[2];
	int pid;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		return NULL;

	pid = fork();
	if (pid < 0) {
		close(sv[0]);
		close(sv[1]);
		return NULL;
	}

	/* parent leaves the child */
	if (pid) {
		worker_process *worker = calloc(1, sizeof(worker_process));
		close(sv[1]);
		if (!worker) {
			kill(SIGKILL, pid);
			close(sv[0]);
			return NULL;
		}
		worker->sd = sv[0];
		worker->pid = pid;
		worker->ioc = iocache_create(1 * 1024 * 1024);

		/* 1 socket for master, 2 fd's for each child */
		worker->max_jobs = (iobroker_max_usable_fds() - 1) / 2;
		worker->jobs = calloc(worker->max_jobs, sizeof(worker_job *));

		return worker;
	}

	/* child closes parent's end of socket and gets busy */
	close(sv[0]);

	if (init_func) {
		init_func(init_arg);
	}
	enter_worker(sv[1], start_cmd);

	/* not reached, ever */
	exit(EXIT_FAILURE);
}
