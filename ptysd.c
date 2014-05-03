/*
 * ptysd: listens for connections on a local TCP port.  For each new connection,
 * launch a login shell so the user gets a shell on this system.  This was
 * written to test my understanding of pseudo-terminals.  Run as:
 * 	make
 * 	./ptysd
 *
 * Then somewhere else:
 *
 * 	nc localhost 8080
 *
 * You should get another shell on this system.
 *
 * Known issues:
 * 	- erroneous "child read: Bad file" error because we try to read from a
 * 	  closed descriptor in ps_relay_one()
 * 	- each command gets echoed before it is run
 * 	- haven't been able to verify job control because "nc" doesn't seem to
 * 	  forward ^Z.  I've tried changing "suspend" on the client to some other
 * 	  character but ^Z still doesn't get sent along.  Maybe need another
 * 	  terminal flag...
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <stropts.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

static int ps_server_init(int);
static void ps_server_run(void);
static int ps_server_connected(int);
static int ps_relay(int, int);
static void *ps_relay_one(void *);
static int ps_init_slavepty(int);

/* CONFIGURATION */
static int ps_port_net = 8080;		/* incoming TCP port */

/* GLOBAL STATE */
static int ps_sockfd = -1;		/* TCP server socket */

int
main(int argc, char *argv[])
{
	if (ps_server_init(ps_port_net) != 0) {
		(void) fprintf(stderr, "failed to setup server");
		return (1);
	}

	ps_server_run();

	return (0);
}

/*
 * Bind to the specified port.
 */
static int
ps_server_init(int port)
{
	struct sockaddr_in addr;
	int reuse = 1;

	if ((ps_sockfd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket");
		return (-1);
	}

	if (setsockopt(ps_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse,
	    sizeof (reuse)) != 0)
		perror("warning: setsockopt(SO_REUSEADDR)");

	bzero(&addr, sizeof (addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ps_port_net);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(ps_sockfd, (struct sockaddr *)&addr, sizeof (addr)) != 0) {
		(void) close(ps_sockfd);
		perror("bind");
		return (-1);
	}

	if (listen(ps_sockfd, 10) != 0) {
		(void) close(ps_sockfd);
		perror("listen");
		return (-1);
	}

	return (0);
}

/*
 * Blocks waiting for new connections.  When a connection is made, forks a child
 * to handle it (but waits for it to exit before accepting more connections).
 * The child executes ps_server_connected.
 */
static void
ps_server_run(void)
{
	int peerfd, status;
	struct sockaddr peer;
	socklen_t plen;
	pid_t pid, newpid;

	while ((peerfd = accept(ps_sockfd, &peer, &plen)) >= 0) {
		if ((pid = fork()) < 0) {
			perror("fork");
			break;
		}

		if (pid != 0) {
			(void) close(peerfd);
			(void) printf("forked child %lu for new connection\n",
			    pid);
			newpid = wait(&status);
			assert(newpid == pid);
			(void) printf("child exited with status %d\n", status);
			continue;
		}

		(void) close(ps_sockfd);
		exit(ps_server_connected(peerfd));
	}

	if (peerfd < 0)
		perror("accept");

	(void) close(ps_sockfd);
}

/*
 * Run by the child process to handle a new connection.  We open up a new pty
 * and fork.  In the new child we create a new session, set up the file
 * descriptors, change to the current user's home directory, and exec bash.  In
 * the original child (parent of this second fork) we create two threads to
 * relay data between the socket and the pseudo-terminal master.
 */
static int
ps_server_connected(int peerfd)
{
	int mfd;
	pid_t childpid;

	if ((mfd = posix_openpt(O_RDWR | O_NOCTTY)) < 0) {
		perror("child: posix_openpt");
		return (-1);
	}

	if (grantpt(mfd) != 0) {
		perror("child: grantpt");
		return (-1);
	}

	if (unlockpt(mfd) != 0) {
		perror("child: unlockpt");
		return (-1);
	}

	if ((childpid = fork()) < 0) {
		perror("child: fork");
		return (-1);
	}

	if (childpid != 0) {
		if (ps_relay(peerfd, mfd) != 0)
			return (-1);

		(void) wait(NULL);
		return (0);
	}

	(void) close(peerfd);

	(void) setsid();

	if (ps_init_slavepty(mfd) != 0)
		exit(1);

	if (chdir(getenv("HOME")) != 0)
		perror("child: WARN: chdir");

	execl("/bin/bash", "/bin/bash", "-l", NULL);
	perror("child's child: exec");
	exit(1);
}

/*
 * Invoked in the child's child to initialize the slave side of the pty and set
 * of file descriptors for forking the child.
 */
static int
ps_init_slavepty(int mfd)
{
	char *slavename;
	int sfd, setup;

	if ((slavename = ptsname(mfd)) == NULL) {
		perror("child's child: ptsname");
		return (-1);
	}

	(void) close(mfd);

	if ((sfd = open(slavename, O_RDWR)) < 0) {
		perror("child's child: open pty");
		return (-1);
	}

	if ((setup = ioctl(sfd, I_FIND, "ldterm")) < 0) {
		perror("child's child: ioctl(I_FIND, \"ldterm\")");
		return (-1);
	}

	if (setup == 0) {
		if (ioctl(sfd, I_PUSH, "ptem") < 0) {
			perror("child's child: ioctl(I_PUSH, \"ptem\")");
			return (-1);
		}

		if (ioctl(sfd, I_PUSH, "ldterm") < 0) {
			perror("child's child: ioctl(I_PUSH, \"ldterm\")");
			return (-1);
		}

		if (ioctl(sfd, I_PUSH, "ttcompat") < 0) {
			perror("child's child: ioctl(I_PUSH, \"ttcompat\")");
			return (-1);
		}
	}

	if (dup2(sfd, STDIN_FILENO) != STDIN_FILENO ||
	    dup2(sfd, STDOUT_FILENO) != STDOUT_FILENO ||
	    dup2(sfd, STDERR_FILENO) != STDERR_FILENO) {
		perror("child's child: dup2");
		return (-1);
	}

	closefrom(STDERR_FILENO + 1);
	return (0);
}

int ps_relay_fds[2] = { -1, -1 };
pthread_t ps_relay_threads[2];

/*
 * Spawn two threads to relay data between the two given file descriptors.
 * State is shared between the threads in ps_relay_threads and ps_relay_fds.
 */
static int
ps_relay(int peerfd, int mfd)
{
	ps_relay_fds[0] = peerfd;
	ps_relay_fds[1] = mfd;

	if (pthread_create(&ps_relay_threads[0], NULL, ps_relay_one, 0) != 0) {
		perror("child: pthread_create(1)");
		return (-1);
	}

	if (pthread_create(&ps_relay_threads[1], NULL, ps_relay_one,
	    (void *)1) != 0) {
		perror("child: pthread_create(2)");
		/* XXX signal and set variable */
		return (-1);
	}

	(void) pthread_detach(ps_relay_threads[0]);
	(void) pthread_detach(ps_relay_threads[1]);
	return (0);
}

static void *
ps_relay_one(void *arg)
{
	int sourcefd, destfd, nread, towrite, nwritten;
	pthread_t other;
	char buf[512];

	sourcefd = ps_relay_fds[(int) arg];
	destfd = ps_relay_fds[(sourcefd + 1) % 2];
	other = ps_relay_threads[(sourcefd + 1) % 2];

	while ((nread = read(sourcefd, buf, sizeof (buf))) >= 0) {
		if (nread == 0) {
			(void) close(destfd);
			break;
		}

		towrite = nread;

		while (towrite > 0) {
			nwritten = write(destfd, buf + (nread - towrite),
			    towrite);
			if (nwritten < 0) {
				perror("child: write");
				break;
			}
			towrite -= nwritten;
		}

		if (towrite > 0)
			break;

		assert(towrite == 0);
	}

	if (nread < 0)
		perror("child: read");

	(void) pthread_cancel(other);
	return (NULL);
}
