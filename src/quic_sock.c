/*
 * QUIC socket management.
 *
 * Copyright 2020 HAProxy Technologies, Frédéric Lécaille <flecaille@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <errno.h>

#include <sys/socket.h>
#include <sys/types.h>

#include <haproxy/connection.h>
#include <haproxy/listener.h>
#include <haproxy/session.h>
#include <haproxy/xprt_quic.h>

/* This function is called from the protocol layer accept() in order to
 * instantiate a new session on behalf of a given listener and frontend. It
 * returns a positive value upon success, 0 if the connection can be ignored,
 * or a negative value upon critical failure. The accepted connection is
 * closed if we return <= 0. If no handshake is needed, it immediately tries
 * to instantiate a new stream. The connection must already have been filled
 * with the incoming connection handle (a fd), a target (the listener) and a
 * source address.
 */
int quic_session_accept(struct connection *cli_conn)
{
	struct listener *l = __objt_listener(cli_conn->target);
	struct proxy *p = l->bind_conf->frontend;
	struct session *sess;

	cli_conn->proxy_netns = l->rx.settings->netns;
	/* This flag is ordinarily set by conn_ctrl_init() which cannot
	 * be called for now.
	 */
	cli_conn->flags |= CO_FL_CTRL_READY;

	/* wait for a PROXY protocol header */
	if (l->options & LI_O_ACC_PROXY)
		cli_conn->flags |= CO_FL_ACCEPT_PROXY;

	/* wait for a NetScaler client IP insertion protocol header */
	if (l->options & LI_O_ACC_CIP)
		cli_conn->flags |= CO_FL_ACCEPT_CIP;

	/* Add the handshake pseudo-XPRT */
	if (cli_conn->flags & (CO_FL_ACCEPT_PROXY | CO_FL_ACCEPT_CIP)) {
		if (xprt_add_hs(cli_conn) != 0)
			goto out_free_conn;
	}

	sess = session_new(p, l, &cli_conn->obj_type);
	if (!sess)
		goto out_free_conn;

	conn_set_owner(cli_conn, sess, NULL);

	if (conn_complete_session(cli_conn) < 0)
		goto out_free_sess;

	if (conn_xprt_start(cli_conn) >= 0)
		return 1;

 out_free_sess:
	/* prevent call to listener_release during session_free. It will be
	* done below, for all errors. */
	sess->listener = NULL;
	session_free(sess);
 out_free_conn:
	cli_conn->qc->conn = NULL;
	conn_stop_tracking(cli_conn);
	conn_xprt_close(cli_conn);
	conn_free(cli_conn);
 out:

	return -1;
}

/*
 * Inspired from session_accept_fd().
 * Instantiate a new connection (connection struct) to be attached to <qc>
 * QUIC connection of <l> listener.
 * Returns 1 if succeeded, 0 if not.
 */
static int new_quic_cli_conn(struct quic_conn *qc, struct listener *l,
                             struct sockaddr_storage *saddr)
{
	struct connection *cli_conn;
	struct sockaddr_storage *dst;

	dst = NULL;
	if (unlikely((cli_conn = conn_new(&l->obj_type)) == NULL))
		goto out;

	if (!sockaddr_alloc(&dst, saddr, sizeof *saddr))
		goto out_free_conn;

	qc->conn = cli_conn;
	cli_conn->qc = qc;

	cli_conn->dst = dst;
	cli_conn->handle.fd = l->rx.fd;
	cli_conn->target = &l->obj_type;

	/* XXX Should not be there. */
	l->accept = quic_session_accept;
	/* We need the xprt context before accepting (->accept()) the connection:
	 * we may receive packet before this connection acception.
	 */
	if (conn_prepare(cli_conn, l->rx.proto, l->bind_conf->xprt) < 0)
		goto out_free_conn;

	return 1;

 out_free_conn:
	qc->conn = NULL;
	conn_stop_tracking(cli_conn);
	conn_xprt_close(cli_conn);
	conn_free(cli_conn);
 out:

	return 0;
}

/* Tests if the receiver supports accepting connections. Returns positive on
 * success, 0 if not possible
 */
int quic_sock_accepting_conn(const struct receiver *rx)
{
	return 1;
}

/* Accept an incoming connection from listener <l>, and return it, as well as
 * a CO_AC_* status code into <status> if not null. Null is returned on error.
 * <l> must be a valid listener with a valid frontend.
 */
struct connection *quic_sock_accept_conn(struct listener *l, int *status)
{
	struct quic_conn *qc;
	struct quic_rx_packet *pkt;
	int ret;

	qc = NULL;
	pkt = MT_LIST_POP(&l->rx.pkts, struct quic_rx_packet *, rx_list);
	/* Should never happen. */
	if (!pkt)
		goto err;

	qc = pkt->qc;
	if (!new_quic_cli_conn(qc, l, &pkt->saddr))
		goto err;

	ret = CO_AC_DONE;

 done:
	if (status)
		*status = ret;

	return qc ? qc->conn : NULL;

 err:
	ret = CO_AC_PAUSE;
	goto done;
}

/* Function called on a read event from a listening socket. It tries
 * to handle as many connections as possible.
 */
void quic_sock_fd_iocb(int fd)
{
	ssize_t ret;
	struct buffer *buf;
	struct listener *l = objt_listener(fdtab[fd].owner);
	/* Source address */
	struct sockaddr_storage saddr = {0};
	socklen_t saddrlen;

	BUG_ON(!l);

	if (!(fdtab[fd].state & FD_POLL_IN) || !fd_recv_ready(fd))
		return;

	buf = get_trash_chunk();
	saddrlen = sizeof saddr;
	do {
		ret = recvfrom(fd, buf->area, buf->size, 0,
		               (struct sockaddr *)&saddr, &saddrlen);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				fd_cant_recv(fd);
			return;
		}
	} while (0);

	buf->data = ret;
	quic_lstnr_dgram_read(buf->area, buf->data, l, &saddr);
}
