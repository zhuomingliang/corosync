/*
 * Copyright (c) 2002-2004 MontaVista Software, Inc.
 *
 * All rights reserved.
 *
 * Author: Steven Dake (sdake@mvista.com)
 *
 * This software licensed under BSD license, the text of which follows:
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <assert.h>
#include <pwd.h>
#include <grp.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/sysinfo.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <time.h>

#include "../include/ais_types.h"
#include "../include/ais_msg.h"
#include "../include/list.h"
#include "../include/queue.h"
#include "poll.h"
#include "gmi.h"
#include "mempool.h"
#include "parse.h"
#include "main.h"
#include "handlers.h"
#include "clm.h"
#include "amf.h"
#include "ckpt.h"
#include "print.h"

#define SERVER_BACKLOG 5

int connection_entries = 0;
struct connection *connections = 0;
int ais_uid = 0;
int gid_valid = 0;

struct gmi_groupname aisexec_groupname = { "0123" };

/*
 * All service handlers in the AIS
 */
struct service_handler *ais_service_handlers[] = {
    &clm_service_handler,
    &amf_service_handler,
    &ckpt_service_handler,
    &ckpt_checkpoint_service_handler,
    &ckpt_sectioniterator_service_handler
};

#define AIS_SERVICE_HANDLERS_COUNT 5
#define AIS_SERVICE_HANDLER_AISEXEC_FUNCTIONS_MAX 40

static int poll_handler_libais_deliver (poll_handle handle, int fd, int revent, void *data);

static inline void ais_done (int err)
{
	log_printf (LOG_LEVEL_ERROR, "AIS Executive exiting.\n");
	exit (1);
}

static inline int init_connection_entry (int fd)
{
	int res;

	memset (&connections[fd], 0, sizeof (struct connection));
	connections[fd].active = 1;
	res = queue_init (&connections[fd].outq, SIZEQUEUE, sizeof (struct outq_item));
	if (res != 0) {
		goto error_exit;
	}
	connections[fd].inb = malloc (sizeof (char) * SIZEINB);
	if (connections[fd].inb == 0) {
		queue_free (&connections[fd].outq);
		goto error_exit;
	}
	return (0);
	
error_exit:
	return (-1);
}

/*
 * Grows the connections table to fd + 1 in size clearing new entries
 */
static inline int grow_connections_table (int fd)
{
	struct connection *conn_temp;

	if (fd + 1 > connection_entries) {
		conn_temp = mempool_realloc (connections, (fd + 1) * sizeof (struct connection));
		if (conn_temp == 0) {
			return (-1);
		}
		connections = conn_temp;
		memset (&connections[connection_entries], 0,
			(fd - connection_entries + 1) * sizeof (struct connection));
		connection_entries = fd + 1;
	}
	return (0);
}

struct sockaddr_in this_ip;
#define LOCALHOST_IP inet_addr("127.0.0.1")

char *socketname = "libais.socket";

static void libais_disconnect (int fd)
{
	int i;

	close (fd);
	connections[fd].active = 0;
	queue_free (&connections[fd].outq);
	free (connections[fd].inb);

	for (i = 0; i < AIS_SERVICE_HANDLERS_COUNT; i++) {
		if (ais_service_handlers[i]->libais_exit_fn) {
			ais_service_handlers[i]->libais_exit_fn (fd);
		}
	}

	poll_dispatch_delete (aisexec_poll_handle, fd);
}

extern int libais_send_response (int s, void *msg, int mlen)
{
	struct queue *outq;
	char *cmsg;
	int res;
	int queue_empty;
	struct outq_item *queue_item;
	struct outq_item queue_item_out;
	struct msghdr msg_send;
	struct iovec iov_send;

	outq = &connections[s].outq;

	msg_send.msg_iov = &iov_send;
	msg_send.msg_name = 0;
	msg_send.msg_namelen = 0;
	msg_send.msg_iovlen = 1;
	msg_send.msg_control = 0;
	msg_send.msg_controllen = 0;
	msg_send.msg_flags = 0;

	if (queue_is_full (outq)) {
		log_printf (LOG_LEVEL_ERROR, "queue is full.\n");
		ais_done (1);
	}
	while (!queue_is_empty (outq)) {
		queue_item = queue_item_get (outq);
		iov_send.iov_base = (void *)connections[s].byte_start;
		iov_send.iov_len = queue_item->mlen;

retry_sendmsg:
		res = sendmsg (s, &msg_send, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (res == -1 && errno == EINTR) {
			goto retry_sendmsg;
		}
		if (res == -1 && errno == EAGAIN) {
			break; /* outgoing kernel queue full, ais_done while not empty */
		}
		if (res == -1) {
			return (-1); /* message couldn't be sent */
		}

		/*
		 * Message sent, try sending another message
		 */
		queue_item_remove (outq);
		connections[s].byte_start = 0;
		mempool_free (queue_item->msg);
	} /* while queue not empty */

	res = 0;
	queue_empty = queue_is_empty (outq);
	/*
	 * Send requested message
	 */
	if (queue_empty) {
		iov_send.iov_base = msg;
		iov_send.iov_len = mlen;
retry_sendmsg_two:
		res = sendmsg (s, &msg_send, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (res == -1 && errno == EINTR) {
			goto retry_sendmsg_two;
		}
		if (res == -1 && errno != EAGAIN) {
			return (-1);
		}
	}

	/*
	 * If res == -1 , errrno == EAGAIN which means kernel queue full
	 */
	if (res == -1)  {
		cmsg = mempool_malloc (mlen);
		if (cmsg == 0) {
			ais_done (1);
		}
		queue_item_out.msg = cmsg;
		queue_item_out.mlen = mlen;
		memcpy (cmsg, msg, mlen);
		queue_item_add (outq, &queue_item_out);
	}
	return (0);
}
		
static int poll_handler_libais_accept (
	poll_handle handle,
	int fd,
	int revent,
	void *data)
{
	int addrlen;
	struct sockaddr_un un_addr;
	int new_fd;
	int on = 1;
	int res;

	addrlen = sizeof (struct sockaddr_un);

retry_accept:
	new_fd = accept (fd, (struct sockaddr *)&un_addr, &addrlen);
	if (new_fd == -1 && errno == EINTR) {
		goto retry_accept;
	}

	if (new_fd == -1) {
		log_printf (LOG_LEVEL_ERROR, "ERROR: Could not accept Library connection: %s\n", strerror (errno));
		return (0); /* This is an error, but -1 would indicate disconnect from poll loop */
	}
		
	/*
	 * Valid accept
	 */

	/*
	 * Request credentials of sender provided by kernel
	 */
	setsockopt(new_fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof (on));

	log_printf (LOG_LEVEL_DEBUG, "connection received from libais client %d.\n", new_fd);
	/*
	 * Generate new connections array
	 */
	res = grow_connections_table (new_fd);
	if (res == -1) {
		close (new_fd);
		return (0); /* This is an error, but -1 would indicate disconnect from poll */

	}

	res = init_connection_entry (new_fd);
	if (res == -1) {
		close (new_fd);
		return (0); /* This is an error, but -1 would indicate disconnect from poll */
	}
	poll_dispatch_add (aisexec_poll_handle, new_fd, POLLIN, 0, poll_handler_libais_deliver);

	connections[new_fd].service = SOCKET_SERVICE_INIT;
	memcpy (&connections[new_fd].ais_ci.un_addr, &un_addr, sizeof (struct sockaddr_un));
	return (0);
}

static int poll_handler_libais_deliver (poll_handle handle, int fd, int revent, void *data)
{
	int res;
	struct message_header *header;
	int service;
	struct msghdr msg_recv;
	struct iovec iov_recv;
	struct cmsghdr *cmsg;
	char cmsg_cred[CMSG_SPACE (sizeof (struct ucred))];
	struct ucred *cred;
	int on = 0;

	msg_recv.msg_iov = &iov_recv;
	msg_recv.msg_iovlen = 1;
	msg_recv.msg_name = 0;
	msg_recv.msg_namelen = 0;
	msg_recv.msg_flags = 0;

	if (connections[fd].authenticated) {
		msg_recv.msg_control = 0;
		msg_recv.msg_controllen = 0;
	} else {
		msg_recv.msg_control = (void *)cmsg_cred;
		msg_recv.msg_controllen = sizeof (cmsg_cred);
	}

	iov_recv.iov_base = &connections[fd].inb[connections[fd].inb_start];
	iov_recv.iov_len = (SIZEINB) - connections[fd].inb_start;
	assert (iov_recv.iov_len != 0);
//printf ("inb start inb inuse %d %d\n", connections[fd].inb_start, connections[fd].inb_inuse);

retry_recv:
	res = recvmsg (fd, &msg_recv, MSG_DONTWAIT | MSG_NOSIGNAL);
//printf ("received %d bytes\n", res);
	if (res == -1 && errno == EINTR) {
		goto retry_recv;
	} else
	if (res == -1) {
		goto error_exit;
	} else
	if (res == 0) {
		goto error_exit;
		return (-1);
	}

	/*
	 * Authenticate if this connection has not been authenticated
	 */
	if (connections[fd].authenticated == 0) {
		cmsg = CMSG_FIRSTHDR (&msg_recv);
		cred = (struct ucred *)CMSG_DATA (cmsg);
		if (cred) {
			if (cred->uid == 0 || cred->gid == gid_valid) {
				setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof (on));
				connections[fd].authenticated = 1;
			}
		}
		if (connections[fd].authenticated == 0) {
			log_printf (LOG_LEVEL_SECURITY, "Connection not authenticated because gid is %d, expecting %d\n", cred->gid, gid_valid);
		}
	}
	/*
	 * Dispatch all messages received in recvmsg that can be dispatched
	 * sizeof (struct message_header) needed at minimum to do any processing
	 */
	connections[fd].inb_inuse += res;
	connections[fd].inb_start += res;

	while (connections[fd].inb_inuse >= sizeof (struct message_header) && res != -1) {
		header = (struct message_header *)&connections[fd].inb[connections[fd].inb_start - connections[fd].inb_inuse];

		if (header->magic != MESSAGE_MAGIC) {
			log_printf (LOG_LEVEL_SECURITY, "Invalid magic is %x should be %x\n", header->magic, MESSAGE_MAGIC);
			res = -1;
			goto error_exit;
		}

		if (header->size > connections[fd].inb_inuse) {
			break;
		}
		service = connections[fd].service;

		/*
		 * If this service is in init phase, initialize service
		 * else handle message using service handlers
		 */
		if (service == SOCKET_SERVICE_INIT) {
			/*
			 * Initializing service
			 */
			res = ais_service_handlers[header->id]->libais_init_fn (fd, header);
		} else  {
			/*
			 * Not an init service, but a standard service
			 */
			if (header->id < 0 || header->id > ais_service_handlers[service - 1]->libais_handler_fns_count) {
				log_printf (LOG_LEVEL_SECURITY, "Invalid header id is %d min 0 max %d\n",
				header->id, ais_service_handlers[service - 1]->libais_handler_fns_count);
				res = -1;
				goto error_exit;
			}
	
			res = ais_service_handlers[service - 1]->libais_handler_fns[header->id](fd, header);
		}
		connections[fd].inb_inuse -= header->size;
	} /* while */

	if (connections[fd].inb_inuse == 0) {
		connections[fd].inb_start = 0;
	} else 
// BUG	if (connections[fd].inb_start + connections[fd].inb_inuse >= SIZEINB) {
	if (connections[fd].inb_start >= SIZEINB) {
		/*
		 * If in buffer is full, move it back to start
		 */
		memmove (connections[fd].inb,
			&connections[fd].inb[connections[fd].inb_start -
				connections[fd].inb_inuse],
			sizeof (char) * connections[fd].inb_inuse);
		connections[fd].inb_start = connections[fd].inb_inuse;
	}

	
	return (res);

error_exit:
	libais_disconnect (fd);
	return (-1); /* remove entry from poll list */
}

extern void print_stats (void);

void sigintr_handler (int signum)
{

#ifdef DEBUG_MEMPOOL
	int stats_inuse[MEMPOOL_GROUP_SIZE];
	int stats_avail[MEMPOOL_GROUP_SIZE];
	int stats_memoryused[MEMPOOL_GROUP_SIZE];
	int i;

	mempool_getstats (stats_inuse, stats_avail, stats_memoryused);
	log_printf (LOG_LEVEL_DEBUG, "Memory pools:\n");
	for (i = 0; i < MEMPOOL_GROUP_SIZE; i++) {
	log_printf (LOG_LEVEL_DEBUG, "order %d size %d inuse %d avail %d memory used %d\n",
		i, 1<<i, stats_inuse[i], stats_avail[i], stats_memoryused[i]);
	}
#endif

	print_stats ();
	ais_done (0);
}

static struct sched_param sched_param = { 
	sched_priority: 99
};

static int pool_sizes[] = { 0, 0, 0, 0, 0, 4096, 0, 1, 0, /* 256 */
					1024, 0, 1, 4096, 0, 0, 0, 0, /* 65536 */
					1, 1, 1, 1, 1, 1, 1, 1, 1 };

static int (*aisexec_handler_fns[AIS_SERVICE_HANDLER_AISEXEC_FUNCTIONS_MAX]) (int fd, void *);
static int aisexec_handler_fns_count = 0;

/*
 * Builds the handler table as an optimization
 */
static void aisexec_handler_fns_build (void)
{
	int i, j;

	for (i = 0; i < AIS_SERVICE_HANDLERS_COUNT; i++) {
		for (j = 0; j < ais_service_handlers[i]->aisexec_handler_fns_count; j++) {
			aisexec_handler_fns[aisexec_handler_fns_count++] = 
				ais_service_handlers[i]->aisexec_handler_fns[j];
		}
	}
	log_printf (LOG_LEVEL_DEBUG, "built %d handler functions\n", aisexec_handler_fns_count);
}

char delivery_data[MESSAGE_SIZE_MAX];

static void deliver_fn (
	struct gmi_groupname *groupname,
	struct iovec *iovec,
	int iov_len)
{
	struct message_header *header;
	int res;
	int pos = 0;
	int i;

	/*
	 * Build buffer without iovecs to make processing easier
	 * This is only used for messages which are multicast with iovecs
	 * and self-delivered.  All other mechanisms avoid the copy.
	 */
	if (iov_len > 1) {
		for (i = 0; i < iov_len; i++) {
			memcpy (&delivery_data[pos], iovec[i].iov_base, iovec[i].iov_len);
			pos += iovec[i].iov_len;
			assert (pos < MESSAGE_SIZE_MAX);
		}
		header = (struct message_header *)delivery_data;
	} else {
		header = iovec[0].iov_base;
	}
	res = aisexec_handler_fns[header->id](0, header);
}

static void confchg_fn (
	struct sockaddr_in *member_list, int member_list_entries,
	struct sockaddr_in *left_list, int left_list_entries,
	struct sockaddr_in *joined_list, int joined_list_entries)
{
	int i;

	/*
	 * Call configure change for all APIs
	 */
	for (i = 0; i < AIS_SERVICE_HANDLERS_COUNT; i++) {
		if (ais_service_handlers[i]->confchg_fn) {
			ais_service_handlers[i]->confchg_fn (member_list, member_list_entries,
				left_list, left_list_entries, joined_list, joined_list_entries);
		}
	}
}

static void aisexec_uid_determine (void)
{
	struct passwd *passwd;

	passwd = getpwnam("ais");
	if (passwd == 0) {
		log_printf (LOG_LEVEL_ERROR, "ERROR: The 'ais' user is not found in /etc/passwd, please read the documentation.\n");
		ais_done (-1);
	}
	ais_uid = passwd->pw_uid;
}

static void aisexec_gid_determine (void)
{
	struct group *group;
	group = getgrnam ("ais");
	if (group == 0) {
		log_printf (LOG_LEVEL_ERROR, "ERROR: The 'ais' group is not found in /etc/group, please read the documentation.\n");
		ais_done (-1);
	}
	gid_valid = group->gr_gid;
}

static void aisexec_priv_drop (void)
{
	setuid (ais_uid);
	setegid (ais_uid);
}

static void aisexec_mempool_init (void)
{
	int res;

	res = mempool_init (pool_sizes);
	if (res == ENOMEM) {
		log_printf (LOG_LEVEL_ERROR, "Couldn't allocate memory pools, not enough memory");
		ais_done (1);
	}
}

static void aisexec_tty_detach (void)
{
#define DEBUG
#ifndef DEBUG
	/*
	 * Disconnect from TTY if this is not a debug run
	 */
	switch (fork ()) {
		case -1:
			ais_done (1);
			break;
		case 0:
			/*
			 * child which is disconnected, run this process
			 */
			break;
		default:
			exit (0);
			break;
	}
#endif
#undef DEBUG
}

static void aisexec_service_handlers_init (void)
{
	int i;
	/*
	 * Initialize all services
	 */
	for (i = 0; i < AIS_SERVICE_HANDLERS_COUNT; i++) {
		if (ais_service_handlers[i]->aisexec_init_fn) {
			ais_service_handlers[i]->aisexec_init_fn ();
		}
	}
}

static void aisexec_libais_bind (int *server_fd)
{
	int libais_server_fd;
	struct sockaddr_un un_addr;
	int res;

	/*
	 * Create socket for libais clients, name socket, listen for connections
	 */
	libais_server_fd = socket (PF_UNIX, SOCK_STREAM, 0);
	if (libais_server_fd == -1) {
		log_printf (LOG_LEVEL_ERROR ,"Cannot create libais client connections socket.\n");
		ais_done (1);
	};

	memset (&un_addr, 0, sizeof (struct sockaddr_un));
	un_addr.sun_family = AF_UNIX;
	strcpy (un_addr.sun_path + 1, socketname);

	res = bind (libais_server_fd, (struct sockaddr *)&un_addr, sizeof (struct sockaddr_un));
	if (res) {
		log_printf (LOG_LEVEL_ERROR, "ERROR: Could not bind AF_UNIX: %s.\n", strerror (errno));
		ais_done (1);
	}
	listen (libais_server_fd, SERVER_BACKLOG);

	*server_fd = libais_server_fd;
}

static void aisexec_setscheduler (void)
{
	int res;

	res = sched_setscheduler (0, SCHED_RR, &sched_param);
	if (res == -1) {
		log_printf (LOG_LEVEL_WARNING, "Could not set SCHED_RR at priority 99: %s\n", strerror (errno));
	}
}

static void aisexec_mlockall (void)
{
	int res;

	res = mlockall (MCL_CURRENT | MCL_FUTURE);
	if (res == -1) {
		log_printf (LOG_LEVEL_WARNING, "Could not lock memory of service to avoid page faults: %s\n", strerror (errno));
	};
}

int main (int argc, char **argv)
{
	int libais_server_fd;
	int res;
	struct sockaddr_in sockaddr_in_mcast;
	struct sockaddr_in sockaddr_in_bindnet;
	gmi_join_handle handle;


	char *error_string;

	log_printf (LOG_LEVEL_NOTICE, "AIS Executive Service: Copyright (C) 2002-2004 MontaVista Software, Inc.\n");

	aisexec_uid_determine ();

	aisexec_gid_determine ();

	aisexec_poll_handle = poll_create ();

	/*
	 * if gmi_init doesn't have root priveleges, it cannot
	 * bind to a specific interface.  This only matters if
	 * there is more then one interface in a system, so
	 * in this case, only a warning is printed
	 */
	/*
	 * Initialize group messaging interface with multicast address
	 */
	res = amfReadNetwork (&error_string, &sockaddr_in_mcast, &sockaddr_in_bindnet);
	if (res == -1) {
		log_printf (LOG_LEVEL_ERROR, error_string);
		ais_done (1);
	}

	/*
	 * Set round robin realtime scheduling with priority 99
	 * Lock all memory to avoid page faults which may interrupt
	 * application healthchecking
	 */
	aisexec_setscheduler ();

	aisexec_mlockall ();

	gmi_init (&sockaddr_in_mcast, &sockaddr_in_bindnet,
		&aisexec_poll_handle, &this_ip);
	
	/*
	 * Drop root privleges to user 'ais'
	 * TODO: Don't really need full root capabilities;
	 *       needed capabilities are:
	 * CAP_NET_RAW (bindtodevice)
	 * CAP_SYS_NICE (setscheduler)
	 * CAP_IPC_LOCK (mlockall)
	 */
	aisexec_priv_drop ();

	aisexec_handler_fns_build ();

	aisexec_mempool_init ();

	res = amfReadGroups(&error_string);
	if (res == -1) {
		log_printf (LOG_LEVEL_ERROR, error_string);
		ais_done (1);
	}
	
	aisexec_tty_detach ();

	signal (SIGINT, sigintr_handler);

	aisexec_service_handlers_init ();

	aisexec_libais_bind (&libais_server_fd);

	res = grow_connections_table (libais_server_fd);
	if (res == -1) {
		log_printf (LOG_LEVEL_ERROR, "Could not allocate memory for listening socket.\n");
		ais_done (1);
	}

	log_printf (LOG_LEVEL_NOTICE, "AIS Executive Service: started and ready to receive connections.\n");

	/*
	 * Setup libais connection dispatch routine
	 */
	poll_dispatch_add (aisexec_poll_handle, libais_server_fd,
		POLLIN, 0, poll_handler_libais_accept);

	/*
	 * Join multicast group and setup delivery
	 *  and configuration change functions
	 */
	gmi_join (0, deliver_fn, confchg_fn, &handle);

	/*
	 * Start main processing loop
	 */
	poll_run (aisexec_poll_handle);

	return (0);
}
