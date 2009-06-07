/********************************************************************************
 *                               Dionaea
 *                           - catches bugs -
 *
 *
 *
 * Copyright (C) 2009  Paul Baecher & Markus Koetter
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 * 
 *             contact nepenthesdev@gmail.com  
 *
 *******************************************************************************/

#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>


#include <sys/time.h>
#include <time.h>

#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>

#include <udns.h>
#include <glib.h>

#define D_LOG_DOMAIN "connection"

#define CL g_dionaea->loop

#include "dionaea.h"
#include "connection.h"
#include "dns.h"
#include "util.h"
#include "log.h"

#define CONOFF(x)							((void *)x - sizeof(struct connection))
#define CONOFF_IO_IN(x)  					((struct connection *)(((void *)x) - offsetof (struct connection, events.io_in)))
#define CONOFF_IO_OUT(x) 					((struct connection *)(((void *)x) - offsetof (struct connection, events.io_out)))
#define CONOFF_LISTEN_TIMEOUT(x) 			((struct connection *)(((void *)x) - offsetof (struct connection, events.listen_timeout)))
#define CONOFF_CONNECTING_TIMEOUT(x) 		((struct connection *)(((void *)x) - offsetof (struct connection, events.connecting_timeout)))
#define CONOFF_CONNECT_TIMEOUT(x) 			((struct connection *)(((void *)x) - offsetof (struct connection, events.connect_timeout)))
#define CONOFF_DNS_TIMEOUT(x) 				((struct connection *)(((void *)x) - offsetof (struct connection, events.dns_timeout)))
#define CONOFF_HANDSHAKE_TIMEOUT(x) 		((struct connection *)(((void *)x) - offsetof (struct connection, events.handshake_timeout)))
#define CONOFF_CLOSE_TIMEOUT(x) 			((struct connection *)(((void *)x) - offsetof (struct connection, events.close_timeout)))
#define CONOFF_RECONNECT_TIMEOUT(x) 		((struct connection *)(((void *)x) - offsetof (struct connection, events.reconnect_timeout)))
#define CONOFF_THROTTLE_IO_IN_TIMEOUT(x) 	((struct connection *)(((void *)x) - offsetof (struct connection, events.throttle_io_in_timeout)))
#define CONOFF_THROTTLE_IO_OUT_TIMEOUT(x) 	((struct connection *)(((void *)x) - offsetof (struct connection, events.throttle_io_out_timeout)))
#define CONOFF_FREE(x)						((struct connection *)(((void *)x) - offsetof (struct connection, events.free)))



int ssl_tmp_keys_init(struct connection *con);


/*
 *
 * connection generic
 *
 */

struct connection *connection_new(enum connection_transport type)
{
	struct connection *con = g_malloc0(sizeof(struct connection));

	con->trans = type;

	con->socket = -1;
	gettimeofday(&con->stats.start, NULL);
	switch ( type )
	{
	case connection_transport_tcp:
		con->transport.tcp.io_in = g_string_new("");
		con->transport.tcp.io_out = g_string_new("");
		break;

	case connection_transport_tls:
		con->transport.tls.meth = SSLv23_method();
		con->transport.tls.ctx = SSL_CTX_new(con->transport.tls.meth);
		SSL_CTX_set_session_cache_mode(con->transport.tls.ctx, SSL_SESS_CACHE_OFF);
		con->transport.tls.io_in = g_string_new("");
		con->transport.tls.io_out = g_string_new("");
		con->transport.tls.io_out_again = g_string_new("");
//		SSL_CTX_set_timeout(con->transport.ssl.ctx, 60);
		break;

	case connection_transport_udp:
		break;

	case connection_transport_io:
		break;
	}

	con->stats.io_out.throttle.last_throttle = ev_now(CL);
	con->stats.io_out.throttle.interval_start = ev_now(CL);
	con->stats.io_in.throttle.last_throttle = ev_now(CL);
	con->stats.io_in.throttle.interval_start = ev_now(CL);

	refcount_init(&con->refcount);
	con->events.close_timeout.repeat = 10.0;
	con->events.connecting_timeout.repeat = 5.0;

	return con;
}

bool connection_node_set_local(struct connection *con)
{
	socklen_t sizeof_sa = sizeof(struct sockaddr_storage);
	getsockname(con->socket, (struct sockaddr *)&con->local.addr, &sizeof_sa);
	return node_info_set(&con->local, &con->local.addr);
}

bool connection_node_set_remote(struct connection *con)
{
	socklen_t sizeof_sa = sizeof(struct sockaddr_storage);
	getpeername(con->socket, (struct sockaddr *)&con->remote.addr, &sizeof_sa);
	return node_info_set(&con->remote, &con->remote.addr);
}

bool bind_local(struct connection *con)
{
	struct sockaddr_storage sa;
	memset(&sa, 0,  sizeof(struct sockaddr_storage));

	socklen_t sizeof_sa = 0;
	int socket_domain = 0;

	if(con->local.hostname == NULL && ntohs(con->local.port) == 0)
		return true;

	if ( !parse_addr(con->local.hostname, con->local.iface_scope, ntohs(con->local.port), &sa, &socket_domain, &sizeof_sa) )
		return false;

	g_debug("%s socket %i %s:%i", __PRETTY_FUNCTION__, con->socket, con->local.hostname, ntohs(con->local.port));

//	memcpy(&con->src.addr,  &sa, sizeof(struct sockaddr_storage));

	int val=1;

	switch ( con->trans )
	{
	case connection_transport_tls:
	case connection_transport_tcp:
		setsockopt(con->socket, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)); 
//		setsockopt(con->socket, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val)); 
		if ( bind(con->socket, (struct sockaddr *)&sa, sizeof_sa) != 0 )
		{
			g_warning("Could not bind %s:%i (%s)", con->local.hostname, ntohs(con->local.port), strerror(errno));
			close(con->socket);
			con->socket = -1;
			return false;
		}

		// fill src node
		connection_node_set_local(con);

		g_debug("ip '%s' node '%s'", con->local.ip_string, con->local.node_string);

//		connection_set_nonblocking(con);
		return true;

		break;

	case connection_transport_udp:
		break;

	case connection_transport_io:
		break;

	}
	return false;
}

bool connection_bind(struct connection *con, const char *addr, uint16_t port, const char *iface_scope)
{
	g_debug(__PRETTY_FUNCTION__);
	struct sockaddr_storage sa;
	memset(&sa, 0,  sizeof(struct sockaddr_storage));

	socklen_t sizeof_sa = 0;
	int socket_domain = 0;

	char *laddr = (char *)addr;
	if ( laddr == NULL )
		laddr = "0.0.0.0";

	con->local.port = htons(port);
	con->local.hostname = strdup(laddr);
	if ( iface_scope )
		strcpy(con->local.iface_scope, iface_scope);


	if ( !parse_addr(con->local.hostname, con->local.iface_scope, ntohs(con->local.port), &sa, &socket_domain, &sizeof_sa) )
		return false;

	con->local.domain = socket_domain;

	int val=1;

	switch ( con->trans )
	{
	case connection_transport_udp:
		con->type = connection_type_bind;
		con->socket = socket(socket_domain, SOCK_DGRAM, 0);
		setsockopt(con->socket, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)); 
//		setsockopt(con->socket, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val)); 
		if ( bind(con->socket, (struct sockaddr *)&sa, sizeof_sa) != 0 )
		{
			g_warning("Could not bind %s:%i (%s)", con->local.hostname, con->local.port, strerror(errno));
			if ( port != 0 && errno == EADDRINUSE )
			{
				g_warning("Could not bind %s:%i (%s)", con->local.hostname, ntohs(con->local.port), strerror(errno));
				close(con->socket);
				con->socket = -1;
				return false;
			}
		}

		// fill src node
		connection_node_set_local(con);

		g_debug("ip '%s' node '%s'", con->local.ip_string, con->local.node_string);

		connection_set_nonblocking(con);
		ev_io_init(&con->events.io_in, connection_udp_io_in_cb, con->socket, EV_READ);
		if ( port != 0 )
			ev_io_start(CL, &con->events.io_in);

//		con->protocol.ctx = con->protocol.ctx_new(con);

		return true;
		break;

	case connection_transport_io:
	case connection_transport_tcp:
	case connection_transport_tls:
		break;
	}

	return false;
}

bool connection_listen(struct connection *con, int len)
{
	g_debug(__PRETTY_FUNCTION__);

	switch ( con->trans )
	{
	case connection_transport_tcp:
		con->type = connection_type_listen;
		con->socket = socket(con->local.domain, SOCK_STREAM, 0);
		bind_local(con);
		if ( listen(con->socket, len) != 0 )
		{
			close(con->socket);
			g_warning("Could not listen %s:%i (%s)", con->local.hostname, ntohs(con->local.port), strerror(errno));
			return false;
		}
		connection_set_nonblocking(con);
		ev_io_init(&con->events.io_in, connection_tcp_accept_cb, con->socket, EV_READ);
		ev_io_start(CL, &con->events.io_in);
		return true;
		break;

	case connection_transport_tls:
		con->type = connection_type_listen;
		con->socket = socket(con->local.domain, SOCK_STREAM, 0);
		bind_local(con);
		if ( listen(con->socket, 15) != 0 )
		{
			close(con->socket);
			g_warning("Could not listen %s:%i (%s)", con->local.hostname, ntohs(con->local.port), strerror(errno));
			return false;
		}
		connection_set_nonblocking(con);
		connection_tls_mkcert(con);
//		connection_tls_set_certificate(con,"/tmp/server.crt",SSL_FILETYPE_PEM);
//		connection_tls_set_key(con,"/tmp/server.pem",SSL_FILETYPE_PEM);
//		SSL_CTX_set_timeout(con->transport.ssl.ctx, 15);
		ssl_tmp_keys_init(con);
		ev_io_init(&con->events.io_in, connection_tls_accept_cb, con->socket, EV_READ);
		ev_io_start(CL, &con->events.io_in);
		return true;
		break;

	case connection_transport_io:
	case connection_transport_udp:
		break;
	}

	return false;
}

void connection_close(struct connection *con)
{
	g_debug(__PRETTY_FUNCTION__);

	if ( !ev_is_active(&con->events.close_timeout) && con->type != connection_type_listen && 
		 (con->trans == connection_transport_tcp ||	con->trans == connection_transport_tls )
	   )
	{
		ev_timer_init(&con->events.close_timeout, connection_close_timeout_cb, 0., con->events.close_timeout.repeat);
		ev_timer_again(CL, &con->events.close_timeout);
	}


	switch ( con->trans )
	{
	case connection_transport_tcp:
		
		if(con->type != connection_type_listen)
		{
			if ( con->transport.tcp.io_out->len == 0 )
			{
				shutdown(con->socket, SHUT_RD);
				connection_set_state(con, connection_state_shutdown);
			}
		}else
		{
			connection_set_state(con, connection_state_close);
			connection_tcp_disconnect(con);
			return;
		}
		break;

	case connection_transport_tls:
		
		if ( con->transport.tls.ssl != NULL )
		{
			if ( con->transport.tls.io_out->len == 0 && con->transport.tls.io_out_again->len == 0 )
			{
				connection_set_state(con, connection_state_shutdown);
				connection_tls_shutdown_cb(CL, &con->events.io_in, 0);
			}
				
		}else
		{
			connection_set_state(con, connection_state_close);
			connection_tls_disconnect(con);
		}
		break;

	
	case connection_transport_udp:
		connection_set_state(con, connection_state_close);
		connection_udp_disconnect(con);
		return;
		break;

	case connection_transport_io:
		return;
		break;
	}
}


void connection_close_timeout_cb(EV_P_ struct ev_timer *w, int revents)
{
	g_debug("%s loop %p w %p revents %i",__PRETTY_FUNCTION__, EV_A_ w, revents);   

//	struct connection *con = w->data;
	struct connection *con = CONOFF_CLOSE_TIMEOUT(w);
	g_debug("connect to be close_timeouted is %p", con);

	switch ( con->trans )
	{
	case connection_transport_tcp:
		connection_tcp_disconnect(con);
		break;

	case connection_transport_tls:
		connection_tls_disconnect(con);
		break;

	default:
		break;
	}
}

void connection_free(struct connection *con)
{
	g_debug("%s con %p",__PRETTY_FUNCTION__, con);
	ev_timer_init(&con->events.free, connection_free_cb, 0., .5);
	ev_timer_again(CL, &con->events.free);
}

void connection_free_cb(EV_P_ struct ev_timer *w, int revents)
{
	g_debug("%s loop %p",__PRETTY_FUNCTION__, EV_A);   

	struct connection *con = CONOFF_FREE(w);

	if ( ! refcount_is_zero(&con->refcount) )
		return;

#ifdef COMPLETE
	struct action_connection *action = action_connection_new(ACTION_CONNECTION_FREE, con);
	action_emit(g_dionaea, TOACTION(action));
	action_connection_free(action);
#endif

	ev_timer_stop(EV_A_ &con->events.free);

	switch ( con->trans )
	{
	case connection_transport_tcp:
		g_string_free(con->transport.tcp.io_in, TRUE);
		g_string_free(con->transport.tcp.io_out, TRUE);
		break;

	case connection_transport_tls:
		g_string_free(con->transport.tls.io_in, TRUE);
		g_string_free(con->transport.tls.io_out, TRUE);
		g_string_free(con->transport.tls.io_out_again, TRUE);

		if ( con->transport.tls.ssl != NULL )
			SSL_free(con->transport.tls.ssl);
		con->transport.tls.ssl = NULL;

		if ( con->type == connection_type_listen &&  con->transport.tls.ctx != NULL )
			SSL_CTX_free(con->transport.tls.ctx);
		con->transport.tls.ctx = NULL;
		break;

	default:
		break;
	}
	node_info_addr_clear(&con->local);
	node_info_addr_clear(&con->remote);

	if ( con->protocol.ctx_free != NULL )
	{
		con->protocol.ctx_free(con->protocol.ctx);
	}

#ifdef HAVE_STREAMPROCESSORS_H
	if ( con->spd != NULL )
	{
		stream_processors_clear(con);
	}
#endif
	
	refcount_exit(&con->refcount);

	memset(con, 0, sizeof(struct connection));
	free(con);
}

void connection_set_nonblocking(struct connection *con)
{
	g_debug(__PRETTY_FUNCTION__);
	int flags = fcntl(con->socket, F_GETFL, 0);
	flags |= O_NONBLOCK;        
	fcntl(con->socket, F_SETFL, flags);
}

void connection_set_blocking(struct connection *con)
{
	g_debug(__PRETTY_FUNCTION__);
	int flags = fcntl(con->socket, F_GETFL, 0);
	flags |= ~O_NONBLOCK;        
	fcntl(con->socket, F_SETFL, flags);
}


void connection_connect_next_addr(struct connection *con)
{
	g_debug(__PRETTY_FUNCTION__);

	const char *addr;
	while ( (addr = node_info_get_next_addr(&con->remote)) != NULL )
	{
		g_debug("connecting %s", addr);
		struct sockaddr_storage sa;
		memset(&sa, 0,  sizeof(struct sockaddr_storage));
		socklen_t sizeof_sa = 0;
		int socket_domain = 0;

		if ( !parse_addr(addr, con->remote.iface_scope, ntohs(con->remote.port), &sa, &socket_domain, &sizeof_sa) )
		{
			g_debug("could not parse addr");
			continue;
		}

		if(con->local.hostname != NULL && con->local.domain != socket_domain)
		{
			g_debug("remote will be unreachable due to different protocol versions (%i <-> %i)", socket_domain, con->local.domain);
			continue;
		}

		g_debug("connecting %s:%i", addr, ntohs(con->remote.port));
		int ret;
		switch ( con->trans )
		{
		case connection_transport_tcp:
			// create protocol specific data
			if(con->protocol.ctx == NULL)
				con->protocol.ctx = con->protocol.ctx_new(con);

			g_debug("tcp");

			if ( con->socket <= 0 )
				con->socket = socket(socket_domain, SOCK_STREAM, 0);

/*			if (bind_local(con) != true)
			{
				close(con->socket);
				con->socket = -1;
				continue;
			}
*/
			connection_set_nonblocking(con);
			ret = connect(con->socket, (struct sockaddr *)&sa, sizeof_sa);


			if ( ret == -1 )
			{
				if ( errno == EINPROGRESS )
				{
					// set connecting timer
					if ( ev_is_active(&con->events.connecting_timeout) )
						ev_timer_stop(CL, &con->events.connecting_timeout);
					ev_timer_init(&con->events.connecting_timeout, connection_tcp_connecting_timeout_cb, 0., con->events.connecting_timeout.repeat);
					ev_timer_again(CL, &con->events.connecting_timeout);

					ev_io_init(&con->events.io_out, connection_tcp_connecting_cb, con->socket, EV_WRITE);
					ev_io_start(CL, &con->events.io_out);
					connection_set_state(con, connection_state_connecting);
					return;
				} else
					if ( errno == EISCONN )
				{
					connection_established(con);
					return;
				} else
				{
					g_warning("Could not connect %s:%i (%s)", con->remote.hostname, ntohs(con->remote.port), strerror(errno));
					close(con->socket);
					con->socket = -1;
					continue;
				}
			} else
				if ( ret == 0 )
			{
				connection_established(con);
				return;
			}

			break;


		case connection_transport_tls:
			// create protocol specific data
			if(con->protocol.ctx == NULL)
				con->protocol.ctx = con->protocol.ctx_new(con);

			g_debug("ssl");
			if ( con->socket <= 0 )
				con->socket = socket(socket_domain, SOCK_STREAM, 0);
			connection_set_nonblocking(con);
			ret = connect(con->socket, (struct sockaddr *)&sa, sizeof_sa);


			if ( ret == -1 )
			{
				if ( errno == EINPROGRESS )
				{
					// set connecting timer
					if ( ev_is_active(&con->events.connecting_timeout) )
						ev_timer_stop(CL, &con->events.connecting_timeout);
					ev_timer_init(&con->events.connecting_timeout, connection_tls_connecting_timeout_cb, 0., con->events.connecting_timeout.repeat);
					ev_timer_again(CL, &con->events.connecting_timeout);

					ev_io_init(&con->events.io_out, connection_tls_connecting_cb, con->socket, EV_WRITE);
					ev_io_start(CL, &con->events.io_out);
					connection_set_state(con, connection_state_connecting);
					return;
				} else
				{
					g_warning("Could not connect %s:%i (%s)", con->remote.hostname, ntohs(con->remote.port), strerror(errno));
					close(con->socket);
					con->socket = -1;
					continue;
				}
			} else
			if ( ret == 0 )
			{
				connection_set_state(con, connection_state_handshake);
				connection_tls_connect_again_cb(CL, &con->events.io_in, 0);
				return;
			}

			break;


		case connection_transport_udp:
			// create protocol specific data
//			con->protocol.ctx = con->protocol.ctx_new(con);

			g_debug("udp");
			if ( con->socket <= 0 )
				con->socket = socket(socket_domain, SOCK_DGRAM, 0);
			connection_set_nonblocking(con);
			if ( con->remote.port != 0 )
			{
				ret = connect(con->socket, (struct sockaddr *)&sa, sizeof_sa);
				if ( ret != 0 )
					g_warning("Could not connect %s:%i (%s)", con->remote.hostname, ntohs(con->remote.port), strerror(errno));
			}
			connection_node_set_local(con);
			connection_node_set_remote(con);
			memcpy(&con->remote.addr, &sa, sizeof_sa);
			g_debug("connected %s -> %s", con->local.node_string,  con->remote.node_string);

			if ( con->state == connection_state_connected )
				return;

			connection_established(con);
			return;
			break;

		case connection_transport_io:
			break;


		}
	}

	if ( addr == NULL )
	{
		con->protocol.error(con, EHOSTUNREACH);
		connection_close(con);
	}
}

void connection_connect(struct connection* con, const char* addr, uint16_t port, const char *iface_scope)
{
	g_debug("%s con %p addr %s port %i iface %s",__PRETTY_FUNCTION__, con, addr, port, iface_scope);
	struct sockaddr_storage sa;
	memset(&sa, 0,  sizeof(struct sockaddr_storage));

	socklen_t sizeof_sa = 0;
	int socket_domain = 0;


	if ( iface_scope )
		strcpy(con->remote.iface_scope, iface_scope);
	else
		con->remote.iface_scope[0] = '\0';

	con->remote.port = htons(port);

	con->remote.hostname = strdup(addr);

	connection_set_type(con, connection_type_connect);


	if ( !parse_addr(addr, NULL, port, &sa, &socket_domain, &sizeof_sa))
	{
		connection_connect_resolve(con);
	} else
	{
		node_info_add_addr(&con->remote, addr);
		connection_connect_next_addr(con);
	}
}

void connection_reconnect_timeout_set(struct connection *con, double timeout_interval_ms)
{
	ev_timer_init(&con->events.reconnect_timeout, connection_reconnect_timeout_cb, 0., timeout_interval_ms);
}

double connection_reconnect_timeout_get(struct connection *con)
{
	return con->events.reconnect_timeout.repeat;
}


void connection_reconnect(struct connection *con)
{
	g_debug("%s con %p",__PRETTY_FUNCTION__, con);   

	if ( con->socket > 0 )
	{
		close(con->socket);
		con->socket = -1;
	}

	connection_set_state(con, connection_state_reconnect);
	if ( con->events.reconnect_timeout.repeat > 0. )
	{
		ev_timer_again(CL, &con->events.reconnect_timeout);
	} else
	{
		connection_reconnect_timeout_cb(CL, &con->events.reconnect_timeout, 0);
	}
}

void connection_reconnect_timeout_cb(EV_P_ struct ev_timer *w, int revents)
{
	g_debug("%s loop %p",__PRETTY_FUNCTION__, EV_A);   
	struct connection *con = CONOFF_RECONNECT_TIMEOUT(w);

	struct sockaddr_storage sa;
	memset(&sa, 0,  sizeof(struct sockaddr_storage));

	socklen_t sizeof_sa = 0;
	int socket_domain = 0;


	ev_timer_stop(EV_A_ w);

	if ( !parse_addr(con->remote.hostname, NULL, ntohs(con->remote.port), &sa, &socket_domain, &sizeof_sa) )
	{ /* domain */
		if ( con->remote.dns.resolved_address_count == con->remote.dns.current_address )
		{ /* tried all resolved ips already */
			node_info_addr_clear(&con->remote);
			connection_connect_resolve(con);
		}else
		{ /* try next */
			connection_connect_next_addr(con);
		}
	} else
	{ /* single ip(s) */ 
		if ( con->remote.dns.resolved_address_count == con->remote.dns.current_address )
			/* reset and reconnect */
			con->remote.dns.current_address = 0;
		connection_connect_next_addr(con);
	}
}


void connection_disconnect(struct connection *con)
{

//	bistream_debug(&con->bistream);

	if ( ev_is_active(&con->events.io_in) )
		ev_io_stop(CL, &con->events.io_in);

	if ( ev_is_active(&con->events.io_out) )
		ev_io_stop(CL,  &con->events.io_out);

	if ( ev_is_active(&con->events.listen_timeout) )
		ev_timer_stop(CL,  &con->events.listen_timeout);

	if ( ev_is_active(&con->events.connect_timeout) )
		ev_timer_stop(CL,  &con->events.connect_timeout);

	if ( ev_is_active(&con->events.connecting_timeout) )
		ev_timer_stop(CL,  &con->events.connecting_timeout);

	if ( ev_is_active(&con->events.throttle_io_out_timeout) )
		ev_timer_stop(CL,  &con->events.throttle_io_out_timeout);

	if ( ev_is_active(&con->events.throttle_io_in_timeout) )
		ev_timer_stop(CL,  &con->events.throttle_io_in_timeout);

	if ( ev_is_active(&con->events.close_timeout) )
		ev_timer_stop(CL,  &con->events.close_timeout);

	if ( ev_is_active(&con->events.handshake_timeout) )
		ev_timer_stop(CL,  &con->events.handshake_timeout);

	if ( con->socket != -1 )
		close(con->socket);
	con->socket = -1;
}

void connection_send(struct connection *con, const void *data, uint32_t size)
{
	g_debug("%s con %p data %p size %i",__PRETTY_FUNCTION__, con, data, size);

	switch ( con->trans )
	{
	case connection_transport_tcp:
		g_string_append_len(con->transport.tcp.io_out, (gchar *)data, size);
		// flush as much as possible
		// revents=0 indicates send() might return 0
		// in this case we do not close & free the connection
		if ( con->state == connection_state_connected )
			connection_tcp_io_out_cb(g_dionaea->loop, &con->events.io_out, 0);
		break;

	case connection_transport_tls:
		g_string_append_len(con->transport.tls.io_out, (gchar *)data, size);
		// flush as much as possible
		if ( con->state == connection_state_connected )
			connection_tls_io_out_cb(g_dionaea->loop, &con->events.io_out, 0);
		break;


	case connection_transport_udp:
		{
			struct udp_packet *packet = g_malloc0(sizeof(struct udp_packet));
			packet->data = g_string_new_len(data, size);
			memcpy(&packet->to, &con->remote.addr, sizeof(struct sockaddr_storage));
			con->transport.udp.io_out = g_list_append(con->transport.udp.io_out, packet);
			connection_udp_io_out_cb(g_dionaea->loop, &con->events.io_out, 0);
		}
		break;

	case connection_transport_io:
		break;
	}
}

void connection_send_string(struct connection *con, const char *str)
{
	connection_send(con, str, strlen(str));
}



void connection_connect_timeout_set(struct connection *con, double timeout_interval_ms)
{
	g_debug(__PRETTY_FUNCTION__);

	switch ( con->trans )
	{
	case connection_transport_tcp:
		ev_timer_init(&con->events.connect_timeout, connection_tcp_connect_timeout_cb, 0., timeout_interval_ms);
/*
		if (con->typex == connection_tcp_accept || con->typex == connection_tcp_connect)
		{
			ev_timer_again(EV_A_ &con->events.connect_timeout);
		}
*/
		break;

	case connection_transport_tls:
		ev_timer_init(&con->events.connect_timeout, connection_tls_connect_timeout_cb, 0., timeout_interval_ms);

/*		if (con->typex == connection_tls_accept || con->typex == connection_tls_connect)
		{
			ev_timer_again(EV_A_ &con->events.connect_timeout);
		}
*/
		break;

	case connection_transport_udp:
		if ( ev_is_active(&con->events.connect_timeout) )
			ev_timer_stop(CL, &con->events.connect_timeout);

		ev_timer_init(&con->events.connect_timeout, connection_udp_connect_timeout_cb, 0., timeout_interval_ms);
		ev_timer_again(CL, &con->events.connect_timeout);
		break;

	default:
		break;

	}
}

double connection_connect_timeout_get(struct connection *con)
{
	return con->events.connect_timeout.repeat;
}


void connection_listen_timeout_set(struct connection *con, double timeout_interval_ms)
{
	g_debug(__PRETTY_FUNCTION__);
	switch ( con->trans )
	{
	case connection_transport_tcp:
		ev_timer_init(&con->events.listen_timeout, connection_tcp_listen_timeout_cb, 0., timeout_interval_ms);
		ev_timer_again(CL, &con->events.listen_timeout);
		break;

	case connection_transport_tls:
		ev_timer_init(&con->events.listen_timeout, connection_tls_listen_timeout_cb, 0., timeout_interval_ms);
		ev_timer_again(CL, &con->events.listen_timeout);
		break;

	default:
		break;
	}
}

double connection_listen_timeout_get(struct connection *con)
{
	return con->events.listen_timeout.repeat;
}


void connection_handshake_timeout_set(struct connection *con, double timeout_interval_ms)
{
	g_debug(__PRETTY_FUNCTION__);
	switch ( con->trans )
	{
	case connection_transport_tls:
		ev_timer_init(&con->events.handshake_timeout, NULL, 0., timeout_interval_ms);
		break;

	default:
		break;
	}
}



double connection_handshake_timeout_get(struct connection *con)
{
	return con->events.handshake_timeout.repeat;
}





void connection_connecting_timeout_set(struct connection *con, double timeout_interval_ms)
{
	g_debug(__PRETTY_FUNCTION__);
	switch ( con->trans )
	{
	case connection_transport_tcp:
	case connection_transport_tls:
		ev_timer_init(&con->events.connecting_timeout, NULL, 0., timeout_interval_ms);
		break;

	case connection_transport_udp:
	case connection_transport_io:
		break;
	}

}
double connection_connecting_timeout_get(struct connection *con)
{
	return con->events.connecting_timeout.repeat;
}



void connection_established(struct connection *con)
{
	g_debug(__PRETTY_FUNCTION__);
	ev_io_stop(CL, &con->events.io_in);
	ev_io_stop(CL, &con->events.io_out);

	connection_node_set_local(con);
	connection_node_set_remote(con);


	connection_set_state(con, connection_state_connected);
	switch ( con->trans )
	{
	case connection_transport_tcp:
		ev_io_init(&con->events.io_in, connection_tcp_io_in_cb, con->socket, EV_READ);
		ev_io_init(&con->events.io_out, connection_tcp_io_out_cb, con->socket, EV_WRITE);

		// start only io_in
		ev_io_start(CL, &con->events.io_in);

		// inform protocol about new connection
		con->protocol.established(con);

		// set timer
		if ( con->events.connect_timeout.repeat >= 0. )
			ev_timer_again(CL,  &con->events.connect_timeout);

		// if there is something to send, send
		if ( con->transport.tcp.io_out->len > 0 )
			ev_io_start(CL, &con->events.io_out);

		break;

	case connection_transport_tls:
		ev_io_init(&con->events.io_in, connection_tls_io_in_cb, con->socket, EV_READ);
		ev_io_init(&con->events.io_out, connection_tls_io_out_cb, con->socket, EV_WRITE);

		// start only io_in
		ev_io_start(CL, &con->events.io_in);


		// inform protocol about new connection
		con->protocol.established(con);

		// set connect-handshake timeout for the connection
		if ( con->events.connect_timeout.repeat >= 0. )
			ev_timer_again(CL,  &con->events.connect_timeout);

		if ( con->transport.tls.io_out_again->len > 0 || con->transport.tls.io_out->len > 0 )
			ev_io_start(CL, &con->events.io_out);

		break;

	case connection_transport_udp:
		// inform protocol about new connection
		con->protocol.established(con);
		ev_io_init(&con->events.io_in, connection_udp_io_in_cb, con->socket, EV_READ);
		ev_io_start(CL, &con->events.io_in);
		break;

	case connection_transport_io:
		break;
	}
}



void connection_throttle_io_in_set(struct connection *con, uint32_t max_bytes_per_second)
{
	g_debug(__PRETTY_FUNCTION__);
	con->stats.io_in.throttle.max_bytes_per_second = max_bytes_per_second;
}


void connection_throttle_io_out_set(struct connection *con, uint32_t max_bytes_per_second)
{
	g_debug(__PRETTY_FUNCTION__);
	con->stats.io_out.throttle.max_bytes_per_second = max_bytes_per_second;
}

int connection_throttle(struct connection *con, struct connection_throttle *thr)
{
	g_debug(__PRETTY_FUNCTION__);

	if ( thr->max_bytes_per_second == 0 )
		return 64*1024;

	double delta = 0.; // time in ms for this session 
	double expect = 0.; // expected time frame for the sended bytes

	double last_throttle;
	last_throttle = ev_now(CL) - thr->last_throttle;

	g_debug("last_throttle %f", last_throttle);
	if ( last_throttle > 1.0 )
	{
		g_debug("resetting connection");
		connection_throttle_reset(thr);
	}
	thr->last_throttle = ev_now(CL);

	delta = ev_now(CL) - thr->interval_start;
	expect = (double)thr->interval_bytes / (double)thr->max_bytes_per_second;

	g_debug("throttle: delta  %f expect %f", delta, expect);

	int bytes = 1;
	bytes = (delta+0.125)* thr->max_bytes_per_second;
	bytes -= thr->interval_bytes;

	if ( expect > delta )
	{ // we sent to much
		double slp = expect - delta;

		if ( slp < 0.200 && bytes > 0 )
		{
			thr->sleep_adjust = slp;
			g_debug("throttle: discarding sleep do %i bytes", bytes);
			return bytes;
		}

		if ( &con->stats.io_in.throttle == thr )
		{
			g_debug("throttle: io_in");
			ev_io_stop(CL, &con->events.io_in);
			if ( !ev_is_active(&con->events.throttle_io_in_timeout) )
			{
				if ( slp < 0.200 )
					slp = 0.200;
				ev_timer_init(&con->events.throttle_io_in_timeout, connection_throttle_io_in_timeout_cb, slp+thr->sleep_adjust, 0.);
				ev_timer_start(CL, &con->events.throttle_io_in_timeout);
			}
			return 0;
		} else
		if ( &con->stats.io_out.throttle == thr )
		{
			g_debug("throttle: io_out");
			ev_io_stop(CL, &con->events.io_out);
			if ( !ev_is_active(&con->events.throttle_io_out_timeout) )
			{
				if ( slp < 0.200 )
					slp = 0.200;
				ev_timer_init(&con->events.throttle_io_out_timeout, connection_throttle_io_out_timeout_cb, slp+thr->sleep_adjust, 0.);
				ev_timer_start(CL, &con->events.throttle_io_out_timeout);
			}
			return 0;
		}
	} else
	{
		bytes = (delta+0.250)* thr->max_bytes_per_second;
		bytes -= thr->interval_bytes;
		g_debug("throttle: can do %i bytes", bytes);
	}

	return bytes;
}

void connection_throttle_update(struct connection *con, struct connection_throttle *thr, int bytes)
{
	g_debug("%s con %p thr %p bytes %i",__PRETTY_FUNCTION__, con, thr, bytes);
	if ( bytes > 0 )
		thr->interval_bytes += bytes;

	if ( &con->stats.io_in.throttle == thr )
	{
		con->stats.io_in.traffic += bytes;
	} else
	if ( &con->stats.io_out.throttle == thr )
	{
		con->stats.io_out.traffic += bytes;
	}
}

void connection_throttle_io_in_timeout_cb(EV_P_ struct ev_timer *w, int revents)
{                                      
	g_debug(__PRETTY_FUNCTION__);
	struct connection *con = CONOFF_THROTTLE_IO_IN_TIMEOUT(w);
	ev_io_start(EV_A_ &con->events.io_in);
}                                      

void connection_throttle_io_out_timeout_cb(EV_P_ struct ev_timer *w, int revents)
{
	g_debug(__PRETTY_FUNCTION__);
	struct connection *con = CONOFF_THROTTLE_IO_OUT_TIMEOUT(w);
	ev_io_start(EV_A_ &con->events.io_out);
}

void connection_throttle_reset(struct connection_throttle *thr)
{
	thr->interval_bytes = 0;
	thr->sleep_adjust = 0;
}


/*
 *
 * connection tcp
 *
 */



void connection_tcp_accept_cb (EV_P_ struct ev_io *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);
	struct connection *con = CONOFF_IO_IN(w);

	g_debug("w %p w->data %p con %p offset %lu offsetof %lu", w, con, w->data, 
		   (unsigned long int)w->data - (unsigned long int)w,
		   offsetof(struct connection, events.io_in));

	while ( 1 )
	{

		struct sockaddr_storage sa;
		socklen_t sizeof_sa = sizeof(struct sockaddr_storage);

		int accepted_socket = accept(con->socket, (struct sockaddr *)&sa, &sizeof_sa);

		if ( accepted_socket == -1 )//&& (errno == EAGAIN || errno == EWOULDBLOCK) )
			break;


		struct connection *accepted = connection_new(connection_transport_tcp);
		connection_set_type(accepted, connection_type_accept);
		accepted->socket = accepted_socket;


		connection_node_set_local(accepted);
		connection_node_set_remote(accepted);

		g_debug("accept() %i local:'%s' remote:'%s'", accepted->socket, accepted->local.node_string,  accepted->remote.node_string);

#ifdef HAVE_ACTION_H
        struct action_connection *action = action_connection_new(ACTION_CONNECTION_ACCEPTED,accepted);
        action_emit(g_dionaea, TOACTION(action));
        action_connection_free(action);
#endif

		connection_set_nonblocking(accepted);

		// set protocol for accepted connection
		memcpy(&accepted->protocol, &con->protocol, sizeof(struct protocol));

//	accepted->protocol.ctx = con->protocol.ctx;

		// copy connect timeout to new connection
		ev_timer_init(&accepted->events.connect_timeout, connection_tcp_connect_timeout_cb, 0. ,con->events.connect_timeout.repeat);


		// create protocol specific data
		accepted->protocol.ctx = con->protocol.ctx_new(accepted);
//		stream_processors_init(accepted);
		accepted->stats.io_in.throttle.max_bytes_per_second = con->stats.io_in.throttle.max_bytes_per_second;
		accepted->stats.io_out.throttle.max_bytes_per_second = con->stats.io_out.throttle.max_bytes_per_second;
		connection_established(accepted);
	}

	if ( ev_is_active(&con->events.listen_timeout) )
	{
		ev_clear_pending(EV_A_ &con->events.listen_timeout);
		ev_timer_again(EV_A_  &con->events.listen_timeout);
	}
}



void connection_tcp_listen_timeout_cb(EV_P_ struct ev_timer *w, int revents)
{
	g_debug(__PRETTY_FUNCTION__);
	struct connection *con = CONOFF_LISTEN_TIMEOUT(w);
	connection_tcp_disconnect(con);
}


void connection_tcp_connecting_timeout_cb(EV_P_ struct ev_timer *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);
	struct connection *con = CONOFF_CONNECTING_TIMEOUT(w);

	ev_timer_stop(EV_A_ &con->events.connecting_timeout);

	connection_tcp_disconnect(con);
}

void connection_tcp_connecting_cb(EV_P_ struct ev_io *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);
	struct connection *con = CONOFF_IO_OUT(w);

	ev_timer_stop(EV_A_ &con->events.connecting_timeout);

	int socket_error = 0;
	int error_size = sizeof(socket_error);


	int ret = getsockopt(con->socket, SOL_SOCKET, SO_ERROR, &socket_error,(socklen_t *)&error_size);

	if ( ret != 0 || socket_error != 0 )
	{
		errno = socket_error;
//		perror("getsockopt");
//		con->protocol.connect_error(con);
//    	connection_tcp_disconnect(EV_A_ con);

		ev_io_stop(EV_A_ &con->events.io_out);
		close(con->socket);
		con->socket = -1;
		connection_connect_next_addr(con);
		return;
	}

	connection_set_state(con, connection_state_connected);

	connection_node_set_local(con);
	connection_node_set_remote(con);

	g_debug("connection %s -> %s", con->local.node_string, con->remote.node_string);

	connection_established(con);
/*
	con->protocol.ctx = con->protocol.ctx_new(con);

	con->protocol.established(con);
	connection_tcp_io_out_cb(EV_A_ w, revents);

	ev_io_stop(EV_A_ &con->events.io_out);
	ev_io_init(&con->events.io_out, connection_tcp_io_out_cb, con->socket, EV_WRITE);
//	ev_io_start(EV_A_ &con->events.io_out);

	ev_io_start(EV_A_ &con->events.io_in);
*/
}


void connection_tcp_connect_timeout_cb(EV_P_ struct ev_timer *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);
	struct connection *con = CONOFF_CONNECT_TIMEOUT(w);

	if ( con->protocol.timeout == NULL || con->protocol.timeout(con, con->protocol.ctx) == false )
		connection_tcp_disconnect(con);
	else
		ev_timer_again(EV_A_ &con->events.connect_timeout);
}


void connection_tcp_io_in_cb(EV_P_ struct ev_io *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p socket %i events %i revents %i",__PRETTY_FUNCTION__, EV_A_ w, w->data, w->fd, w->events, revents);
	struct connection *con = CONOFF_IO_IN(w);

	int size, buf_size;

	/* determine how many bytes we can recv */
	if (ioctl(con->socket, SIOCINQ, &buf_size) != 0)
		buf_size=1024;
	buf_size++;

	g_debug("can recv %i bytes", buf_size);

	unsigned char buf[buf_size];


	int recv_throttle = connection_throttle(con, &con->stats.io_in.throttle);
	int recv_size = MIN(buf_size, recv_throttle);

	g_debug("io_in: throttle can %i want %i", buf_size, recv_size);

	if ( recv_size == 0 )
		return;

	GString *new_in = g_string_sized_new(buf_size);

	while ( (size = recv(con->socket, buf, recv_size, 0)) > 0 )
	{
		g_string_append_len(new_in, (gchar *)buf, size);
		recv_size -= size;
		if ( recv_size <= 0 )
			break;
	}

	if ( con->spd != NULL )
	{
		g_debug("HHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHH");
		GString *old_in = con->transport.tcp.io_in;
		con->transport.tcp.io_in = new_in;
//		stream_processors_io_in(con);
		con->transport.tcp.io_in = old_in;
	}

	connection_throttle_update(con, &con->stats.io_in.throttle, new_in->len);
	// append
	g_string_append_len(con->transport.tcp.io_in, new_in->str, new_in->len);

	if ( size==0 )//&& size != MIN(buf_size, recv_throttle) )
	{
		g_debug("remote closed connection");
		if ( new_in->len > 0 )
			con->protocol.io_in(con, con->protocol.ctx, (unsigned char *)con->transport.tcp.io_in->str, con->transport.tcp.io_in->len);

		connection_tcp_disconnect(con);
	} else
	if ( (size == -1 && errno == EAGAIN) || size == MIN(buf_size, recv_throttle) )
	{
		g_debug("EAGAIN");
		if ( ev_is_active(&con->events.connect_timeout) )
			ev_timer_again(EV_A_  &con->events.connect_timeout);

		int consumed = 0;

		if(new_in->len > 0)
			consumed = con->protocol.io_in(con, con->protocol.ctx, (unsigned char *)con->transport.tcp.io_in->str, con->transport.tcp.io_in->len);

        g_string_erase(con->transport.tcp.io_in, 0, consumed);

		if ( con->transport.tcp.io_out->len > 0 && !ev_is_active(&con->events.io_out) )
			ev_io_start(EV_A_ &con->events.io_out);

	} else
	{
		g_warning("recv failed (%s)", strerror(errno));
		connection_tcp_disconnect(con);
	}
	con->stats.io_in.traffic += new_in->len;
	g_string_free(new_in, TRUE);
}

void connection_tcp_io_out_cb(EV_P_ struct ev_io *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);
	struct connection *con = CONOFF_IO_OUT(w);
	int send_throttle = connection_throttle(con, &con->stats.io_out.throttle);
	int send_size = MIN(con->transport.tcp.io_out->len, send_throttle);


	if ( send_size == 0 )
		return;


	int size = send(con->socket, con->transport.tcp.io_out->str, send_size, 0);

	if ( ev_is_active(&con->events.connect_timeout) )
		ev_timer_again(EV_A_  &con->events.connect_timeout);




	if ( size > 0 )
	{
		connection_throttle_update(con, &con->stats.io_out.throttle, size);
		con->stats.io_out.traffic += size;

		if ( con->spd != NULL )
		{
			int oldsize = con->transport.tcp.io_out->len;
			con->transport.tcp.io_out->len = size;
//			stream_processors_io_out(con);
			con->transport.tcp.io_out->len = oldsize;
		}

//		bistream_data_add(&con->bistream, bistream_out, con->transport.tcp.io_out->str, size);
		g_string_erase(con->transport.tcp.io_out, 0 , size);
		if ( con->transport.tcp.io_out->len == 0 )
		{
			ev_io_stop(EV_A_ w);
			if ( con->state == connection_state_close )
				connection_tcp_disconnect(con);
		}
	} else
		if ( size == -1 )
	{
		if ( errno == EAGAIN || errno == EWOULDBLOCK )
		{

		} else
			if ( revents != 0 )
			connection_tcp_disconnect(con);
	}
}

void connection_tcp_disconnect(struct connection *con)
{
	g_debug("%s con %p",__PRETTY_FUNCTION__, con);

	connection_set_state(con, connection_state_close);
	connection_disconnect(con);

	if ( con->protocol.disconnect != NULL )
	{
		bool reconnect = con->protocol.disconnect(con, con->protocol.ctx);
		g_debug("reconnect is %i", reconnect);
		if ( reconnect == true && con->type == connection_type_connect )
		{
			connection_reconnect(con);
			return;
		}
	}
	connection_free(con);
}


/*
 *
 * connection ssl
 *
 */

/*
 * the ssl dh key setup is taken from the mod_ssl package from apache
 */

#ifndef SSLC_VERSION_NUMBER
#define SSLC_VERSION_NUMBER 0x0000
#endif

DH *myssl_dh_configure(unsigned char *p, int plen,
                        unsigned char *g, int glen)
{
    DH *dh;

    if (!(dh = DH_new())) {
        return NULL;
    }

#if defined(OPENSSL_VERSION_NUMBER) || (SSLC_VERSION_NUMBER < 0x2000)
    dh->p = BN_bin2bn(p, plen, NULL);
    dh->g = BN_bin2bn(g, glen, NULL);
    if (!(dh->p && dh->g)) {
        DH_free(dh);
        return NULL;
    }
#else
    R_EITEMS_add(dh->data, PK_TYPE_DH, PK_DH_P, 0, p, plen, R_EITEMS_PF_COPY);
    R_EITEMS_add(dh->data, PK_TYPE_DH, PK_DH_G, 0, g, glen, R_EITEMS_PF_COPY);
#endif

    return dh;
}





/*
 * Handle the Temporary RSA Keys and DH Params
 */


/*
** Diffie-Hellman-Parameters: (512 bit)
**     prime:
**         00:9f:db:8b:8a:00:45:44:f0:04:5f:17:37:d0:ba:
**         2e:0b:27:4c:df:1a:9f:58:82:18:fb:43:53:16:a1:
**         6e:37:41:71:fd:19:d8:d8:f3:7c:39:bf:86:3f:d6:
**         0e:3e:30:06:80:a3:03:0c:6e:4c:37:57:d0:8f:70:
**         e6:aa:87:10:33
**     generator: 2 (0x2)
** Diffie-Hellman-Parameters: (1024 bit)
**     prime:
**         00:d6:7d:e4:40:cb:bb:dc:19:36:d6:93:d3:4a:fd:
**         0a:d5:0c:84:d2:39:a4:5f:52:0b:b8:81:74:cb:98:
**         bc:e9:51:84:9f:91:2e:63:9c:72:fb:13:b4:b4:d7:
**         17:7e:16:d5:5a:c1:79:ba:42:0b:2a:29:fe:32:4a:
**         46:7a:63:5e:81:ff:59:01:37:7b:ed:dc:fd:33:16:
**         8a:46:1a:ad:3b:72:da:e8:86:00:78:04:5b:07:a7:
**         db:ca:78:74:08:7d:15:10:ea:9f:cc:9d:dd:33:05:
**         07:dd:62:db:88:ae:aa:74:7d:e0:f4:d6:e2:bd:68:
**         b0:e7:39:3e:0f:24:21:8e:b3
**     generator: 2 (0x2)
*/

static unsigned char dh512_p[] = {
    0x9F, 0xDB, 0x8B, 0x8A, 0x00, 0x45, 0x44, 0xF0, 0x04, 0x5F, 0x17, 0x37,
    0xD0, 0xBA, 0x2E, 0x0B, 0x27, 0x4C, 0xDF, 0x1A, 0x9F, 0x58, 0x82, 0x18,
    0xFB, 0x43, 0x53, 0x16, 0xA1, 0x6E, 0x37, 0x41, 0x71, 0xFD, 0x19, 0xD8,
    0xD8, 0xF3, 0x7C, 0x39, 0xBF, 0x86, 0x3F, 0xD6, 0x0E, 0x3E, 0x30, 0x06,
    0x80, 0xA3, 0x03, 0x0C, 0x6E, 0x4C, 0x37, 0x57, 0xD0, 0x8F, 0x70, 0xE6,
    0xAA, 0x87, 0x10, 0x33,
};
static unsigned char dh512_g[] = {
    0x02,
};

static DH *get_dh512(void)
{
    return myssl_dh_configure(dh512_p, sizeof(dh512_p),
                               dh512_g, sizeof(dh512_g));
}

static unsigned char dh1024_p[] = {
    0xD6, 0x7D, 0xE4, 0x40, 0xCB, 0xBB, 0xDC, 0x19, 0x36, 0xD6, 0x93, 0xD3,
    0x4A, 0xFD, 0x0A, 0xD5, 0x0C, 0x84, 0xD2, 0x39, 0xA4, 0x5F, 0x52, 0x0B,
    0xB8, 0x81, 0x74, 0xCB, 0x98, 0xBC, 0xE9, 0x51, 0x84, 0x9F, 0x91, 0x2E,
    0x63, 0x9C, 0x72, 0xFB, 0x13, 0xB4, 0xB4, 0xD7, 0x17, 0x7E, 0x16, 0xD5,
    0x5A, 0xC1, 0x79, 0xBA, 0x42, 0x0B, 0x2A, 0x29, 0xFE, 0x32, 0x4A, 0x46,
    0x7A, 0x63, 0x5E, 0x81, 0xFF, 0x59, 0x01, 0x37, 0x7B, 0xED, 0xDC, 0xFD,
    0x33, 0x16, 0x8A, 0x46, 0x1A, 0xAD, 0x3B, 0x72, 0xDA, 0xE8, 0x86, 0x00,
    0x78, 0x04, 0x5B, 0x07, 0xA7, 0xDB, 0xCA, 0x78, 0x74, 0x08, 0x7D, 0x15,
    0x10, 0xEA, 0x9F, 0xCC, 0x9D, 0xDD, 0x33, 0x05, 0x07, 0xDD, 0x62, 0xDB,
    0x88, 0xAE, 0xAA, 0x74, 0x7D, 0xE0, 0xF4, 0xD6, 0xE2, 0xBD, 0x68, 0xB0,
    0xE7, 0x39, 0x3E, 0x0F, 0x24, 0x21, 0x8E, 0xB3,
};
static unsigned char dh1024_g[] = {
    0x02,
};

static DH *get_dh1024(void)
{
    return myssl_dh_configure(dh1024_p, sizeof(dh1024_p),
                               dh1024_g, sizeof(dh1024_g));
}

/* ----END GENERATED SECTION---------- */

DH *ssl_dh_GetTmpParam(int nKeyLen)
{
    DH *dh;

    if (nKeyLen == 512)
        dh = get_dh512();
    else if (nKeyLen == 1024)
        dh = get_dh1024();
    else
        dh = get_dh1024();
    return dh;
}

DH *ssl_dh_GetParamFromFile(char *file)
{
    DH *dh = NULL;
    BIO *bio;

    if ((bio = BIO_new_file(file, "r")) == NULL)
        return NULL;
#if 0 //SSL_LIBRARY_VERSION < 0x00904000
    dh = PEM_read_bio_DHparams(bio, NULL, NULL);
#else
    dh = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
#endif
    BIO_free(bio);
    return (dh);
}


#define MYSSL_TMP_KEY_FREE(con, type, idx) \
    if (con->transport.tls.pTmpKeys[idx]) { \
        type##_free((type *)con->transport.tls.pTmpKeys[idx]); \
        con->transport.tls.pTmpKeys[idx] = NULL; \
    }

#define MYSSL_TMP_KEYS_FREE(con, type) \
    MYSSL_TMP_KEY_FREE(con, type, SSL_TMP_KEY_##type##_512); \
    MYSSL_TMP_KEY_FREE(con, type, SSL_TMP_KEY_##type##_1024)

void ssl_tmp_keys_free(struct connection *con)
{
    MYSSL_TMP_KEYS_FREE(con, RSA);
    MYSSL_TMP_KEYS_FREE(con, DH);
}

int ssl_tmp_key_init_rsa(struct connection *con, int bits, int idx)
{
    if (!(con->transport.tls.pTmpKeys[idx] = RSA_generate_key(bits, RSA_F4, NULL, NULL)))
    {
        g_error("Init: Failed to generate temporary %d bit RSA private key", bits);
        return -1;
    }

    return 0;
}

static int ssl_tmp_key_init_dh(struct connection *con, int bits, int idx)
{
    if (!(con->transport.tls.pTmpKeys[idx] = ssl_dh_GetTmpParam(bits)))
    {
        g_error("Init: Failed to generate temporary %d bit DH parameters", bits);
        return -1;
    }

    return 0;
}

#define MYSSL_TMP_KEY_INIT_RSA(s, bits) \
    ssl_tmp_key_init_rsa(s, bits, SSL_TMP_KEY_RSA_##bits)

#define MYSSL_TMP_KEY_INIT_DH(s, bits) \
    ssl_tmp_key_init_dh(s, bits, SSL_TMP_KEY_DH_##bits)

int ssl_tmp_keys_init(struct connection *con)
{

	g_message("Init: Generating temporary RSA private keys (512/1024 bits)");

    if (MYSSL_TMP_KEY_INIT_RSA(con, 512) ||
        MYSSL_TMP_KEY_INIT_RSA(con, 1024)) {
        return -1;
    }

    g_message("Init: Generating temporary DH parameters (512/1024 bits)");

    if (MYSSL_TMP_KEY_INIT_DH(con, 512) ||
        MYSSL_TMP_KEY_INIT_DH(con, 1024)) {
        return -1;
    }

    return 0;
}

RSA *ssl_callback_TmpRSA(SSL *ssl, int export, int keylen)
{
    struct connection *c = (struct connection *)SSL_get_app_data(ssl);
    int idx;

    g_debug("handing out temporary %d bit RSA key", keylen);

    /* doesn't matter if export flag is on,
     * we won't be asked for keylen > 512 in that case.
     * if we are asked for a keylen > 1024, it is too expensive
     * to generate on the fly.
     * XXX: any reason not to generate 2048 bit keys at startup?
     */

    switch (keylen) {
      case 512:
        idx = SSL_TMP_KEY_RSA_512;
        break;

      case 1024:
      default:
        idx = SSL_TMP_KEY_RSA_1024;
    }

    return (RSA *)c->transport.tls.pTmpKeys[idx];
}

/*
 * Hand out the already generated DH parameters...
 */
DH *ssl_callback_TmpDH(SSL *ssl, int export, int keylen)
{
    struct connection *c = (struct connection *)SSL_get_app_data(ssl);
    int idx;

    g_debug("handing out temporary %d bit DH key", keylen);

    switch (keylen) {
      case 512:
        idx = SSL_TMP_KEY_DH_512;
        break;

      case 1024:
      default:
        idx = SSL_TMP_KEY_DH_1024;
    }

    return (DH *)c->transport.tls.pTmpKeys[idx];
}



bool connection_tls_set_certificate(struct connection *con, const char *path, int type)
{
	int ret = SSL_CTX_use_certificate_file(con->transport.tls.ctx, path, type);
	if ( ret != 1 )
	{
		perror("SSL_CTX_use_certificate_file");
		return false;
	}

//	connection_tls_error(con);

	return true;
}

bool connection_tls_set_key(struct connection *con, const char *path, int type)
{
	int ret = SSL_CTX_use_PrivateKey_file(con->transport.tls.ctx, path, type);
	if ( ret != 1 )
	{
		perror("SSL_CTX_use_PrivateKey_file");
		return false;
	}
//	connection_tls_error(con);
	return true;
}


int add_ext(X509 *cert, int nid, char *value)
{
	X509_EXTENSION *ex;
	X509V3_CTX ctx;
	/* This sets the 'context' of the extensions. */
	/* No configuration database */
	X509V3_set_ctx_nodb(&ctx);
	/* Issuer and subject certs: both the target since it is self signed,
	 * no request and no CRL
	 */
	X509V3_set_ctx(&ctx, cert, cert, NULL, NULL, 0);
	ex = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
	if ( !ex )
		return 0;

	X509_add_ext(cert,ex,-1);
	X509_EXTENSION_free(ex);
	return 1;
}

static void callback(int p, int n, void *arg)
{
	char c='B';

	if ( p == 0 ) c='.';
	if ( p == 1 ) c='+';
	if ( p == 2 ) c='*';
	if ( p == 3 ) c='\n';
	fputc(c,stderr);
}

bool connection_tls_mkcert(struct connection *con)
{
	g_debug("%s con %p",__PRETTY_FUNCTION__, con);
	int bits = 512*4;
	int serial = time(NULL);
	int days = 365;


	X509 *x;
	EVP_PKEY *pk;
	RSA *rsa;
	X509_NAME *name=NULL;

	if ( (pk=EVP_PKEY_new()) == NULL )
		goto err;

	if ( (x=X509_new()) == NULL )
		goto err;

	rsa=RSA_generate_key(bits,RSA_F4,callback,NULL);
	if ( !EVP_PKEY_assign_RSA(pk,rsa) )
	{
		perror("EVP_PKEY_assign_RSA");
		goto err;
	}
	rsa=NULL;

	X509_set_version(x,2);
	ASN1_INTEGER_set(X509_get_serialNumber(x),serial);
	X509_gmtime_adj(X509_get_notBefore(x),0);
	X509_gmtime_adj(X509_get_notAfter(x),(long)60*60*24*days);
	X509_set_pubkey(x,pk);

	name=X509_get_subject_name(x);

	X509_NAME_add_entry_by_txt(name,"C",
							   MBSTRING_ASC, (const unsigned char *)"DE", -1, -1, 0);
	X509_NAME_add_entry_by_txt(name,"CN",
							   MBSTRING_ASC, (const unsigned char *)"Nepenthes Development Team", -1, -1, 0);
	X509_NAME_add_entry_by_txt(name,"O",
							   MBSTRING_ASC, (const unsigned char *)"dionaea.carnivore.it", -1, -1, 0);
	X509_NAME_add_entry_by_txt(name,"OU",
							   MBSTRING_ASC, (const unsigned char *)"anv", -1, -1, 0);


	/* Its self signed so set the issuer name to be the same as the
	 * subject.
	 */
	X509_set_issuer_name(x,name);

	add_ext(x, NID_netscape_cert_type, "server");
	add_ext(x, NID_netscape_ssl_server_name, "localhost");

	if ( !X509_sign(x,pk,EVP_md5()) )
		goto err;


	int ret = SSL_CTX_use_PrivateKey(con->transport.tls.ctx, pk);
	if ( ret != 1 )
	{
		perror("SSL_CTX_use_PrivateKey");
		return false;
	}

	ret = SSL_CTX_use_certificate(con->transport.tls.ctx, x);
	if ( ret != 1 )
	{
		perror("SSL_CTX_use_certificate");
		return false;
	}


	return true;
err:
	return false;

}

void connection_tls_io_out_cb(EV_P_ struct ev_io *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);

	struct connection *con = NULL;

	if ( w->events == EV_READ)
		con = CONOFF_IO_IN(w);
	else
		con = CONOFF_IO_OUT(w);

	if ( con->transport.tls.io_out_again->len == 0 )
	{
		GString *io_out_again = con->transport.tls.io_out_again;
		con->transport.tls.io_out_again = con->transport.tls.io_out;
		con->transport.tls.io_out = io_out_again;
		con->transport.tls.io_out_again_size = 0;
	}


	int send_throttle = connection_throttle(con, &con->stats.io_out.throttle);
	if ( con->transport.tls.io_out_again_size == 0 )
		con->transport.tls.io_out_again_size = MIN((int)con->transport.tls.io_out_again->len, send_throttle);

	if ( con->transport.tls.io_out_again_size <= 0 )
		return;

	g_debug("send_throttle %i con->transport.tcp.io_out_again->len %i con->transport.ssl.io_out_again_size %i todo %i",
		   send_throttle, (int)con->transport.tls.io_out_again->len, con->transport.tls.io_out_again_size,
		   (int)con->transport.tls.io_out_again->len + (int)con->transport.tls.io_out->len);


	int err = SSL_write(con->transport.tls.ssl, con->transport.tls.io_out_again->str, con->transport.tls.io_out_again_size);
	connection_tls_error(con);

	if ( err <= 0 )
	{
		int action = SSL_get_error(con->transport.tls.ssl, err);
		connection_tls_error(con);
		switch ( action )
		{
		case SSL_ERROR_NONE:
			g_debug("%s:%i", __FILE__,  __LINE__);
			break;
		case SSL_ERROR_ZERO_RETURN:
			g_debug("%s:%i", __FILE__,  __LINE__);
			if ( revents != 0 )
				connection_tls_disconnect(con);
			else
				connection_set_state(con, connection_state_close);
			break;

		case SSL_ERROR_WANT_READ:
			g_debug("SSL_WANT_READ %s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_WANT_WRITE:
			g_debug("SSL_WANT_WRITE %s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_WANT_ACCEPT:
			g_debug("SSL_ERROR_WANT_ACCEPT%s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_WANT_X509_LOOKUP:
			g_debug("SSL_ERROR_WANT_X509_LOOKUP %s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_SYSCALL:
			g_debug("SSL_ERROR_SYSCALL %s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_SSL:
			g_debug("SSL_ERROR_SSL %s:%i", __FILE__,  __LINE__);
			break;
		}
	} else
	{
		int size = err;

		if ( size == con->transport.tls.io_out_again_size )
		{
			connection_throttle_update(con, &con->stats.io_out.throttle, size);

			g_string_erase(con->transport.tls.io_out_again, 0 , con->transport.tls.io_out_again_size);
			con->transport.tls.io_out_again_size = 0;

			if ( con->transport.tls.io_out_again->len == 0 && con->transport.tls.io_out->len == 0 )
			{
				g_debug("connection is flushed");
				ev_io_stop(EV_A_ w);
				if ( con->state == connection_state_close )
					connection_tls_shutdown_cb(EV_A_ w, revents);
			}

		} else
		{
			g_debug("unexpected %s:%i...", __FILE__,  __LINE__);
		}

	}
}


void connection_tls_shutdown_cb(EV_P_ struct ev_io *w, int revents)
{
	g_debug(__PRETTY_FUNCTION__);

	struct connection *con = NULL;

	if ( w->events == EV_READ )
		con = CONOFF_IO_IN(w);
	else
		con = CONOFF_IO_OUT(w);

	if ( con->type == connection_type_listen )
	{
		g_debug("connection was listening, closing!");
		connection_tls_disconnect(con);
		return;
	}

	if ( SSL_get_shutdown(con->transport.tls.ssl) & (SSL_SENT_SHUTDOWN|SSL_RECEIVED_SHUTDOWN) )
	{
		g_debug("connection has sent&received shutdown");
		connection_tls_disconnect(con);
		return;
	}

	ev_io_stop(EV_A_ &con->events.io_in);
	ev_io_stop(EV_A_ &con->events.io_out);

	connection_tls_error(con);

	int err = SSL_shutdown(con->transport.tls.ssl);
	connection_tls_error(con);

	int action;

	switch ( err )
	{
	case 1:
		connection_tls_disconnect(con);
		break;

	case 0:
		err = SSL_shutdown(con->transport.tls.ssl);
		action = SSL_get_error(con->transport.tls.ssl, err);
		connection_tls_error(con);

		switch ( action )
		{
		case SSL_ERROR_WANT_READ:
			g_debug("SSL_WANT_READ %s:%i", __FILE__,  __LINE__);
			ev_io_init(&con->events.io_in, connection_tls_shutdown_cb, con->socket, EV_READ);
			ev_io_start(EV_A_ &con->events.io_in);
			break;

		case SSL_ERROR_WANT_WRITE:
			g_debug("SSL_WANT_WRITE %s:%i", __FILE__,  __LINE__);
			ev_io_init(&con->events.io_out, connection_tls_shutdown_cb, con->socket, EV_WRITE);
			ev_io_start(EV_A_ &con->events.io_out);
			break;

		case SSL_ERROR_WANT_ACCEPT:
			g_debug("SSL_ERROR_WANT_ACCEPT%s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_WANT_X509_LOOKUP:
			g_debug("SSL_ERROR_WANT_X509_LOOKUP %s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_SYSCALL:
			g_debug("SSL_ERROR_SYSCALL %i %s %s:%i", errno, strerror(errno), __FILE__,  __LINE__);
			if ( errno == 0 )
			{ 
				/* 
				 * HACK actually a bug in openssl - a patch sent on
				 * 2006-06-29 0:12:51
				 * with subject
				 * [PATCH2] Fix for SSL_shutdown() with non-blocking not returning -1
				 * by Darryl L. Miles
				 * actually fixes the issue 
				 *  
				 * patch was merged into openssl
				 * 2009-Apr-07 18:28 http://cvs.openssl.org/chngview?cn=17995
				 * and will (hopefully) ship with openssl 0.9.8l 
				 *  
 				 * given the 3 years it took openssl to accept a patch, 
				 * it did not take me that long to figure out 
				 * why SSL_shutdown failed on nonblocking sockets 
				 *  
				 * at the time of this writing, 0.9.8k is current 
				 * 0.9.8g is shipped by all major vendors as stable 
				 *  
				 * so it may take some time to get this fix to the masses 
				 *  
				 * due to unclear&complex openssl version situation 
				 * I decided not to provide an workaround, just close the connection instead
				 * and rant about openssl 
				 *  
				 */
				connection_tls_disconnect(con);
			}
				
			break;

		case SSL_ERROR_SSL:
			g_debug("SSL_ERROR_SSL %s:%i", __FILE__,  __LINE__);
			connection_tls_disconnect(con);
			break;
		}

		break;

	case -1:
		g_debug("SSL_shutdown -1 %s:%i", __FILE__,  __LINE__);
		action = SSL_get_error(con->transport.tls.ssl, err);
		connection_tls_error(con);

		switch ( action )
		{
		case SSL_ERROR_WANT_READ:
			g_debug("SSL_WANT_READ %s:%i", __FILE__,  __LINE__);
			ev_io_init(&con->events.io_in, connection_tls_shutdown_cb, con->socket, EV_READ);
			ev_io_start(EV_A_ &con->events.io_in);
			break;

		case SSL_ERROR_WANT_WRITE:
			g_debug("SSL_WANT_WRITE %s:%i", __FILE__,  __LINE__);
			ev_io_init(&con->events.io_out, connection_tls_shutdown_cb, con->socket, EV_WRITE);
			ev_io_start(EV_A_ &con->events.io_out);
			break;

		default:
			g_debug("SSL_ERROR_ %i %s:%i", action, __FILE__,  __LINE__);
			connection_tls_disconnect(con);
			break;
		}
		break;

	default:
		g_debug("SSL_shutdown %i %s:%i", err, __FILE__,  __LINE__);
		break;
	}
}

void connection_tls_io_in_cb(EV_P_ struct ev_io *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);

	struct connection *con = NULL;

	if ( w->events == EV_READ)
		con = CONOFF_IO_IN(w);
	else
		con = CONOFF_IO_OUT(w);

	int recv_throttle = connection_throttle(con, &con->stats.io_in.throttle);
	g_debug("recv throttle %i\n", recv_throttle);
	if ( recv_throttle == 0 )
		return;

	unsigned char buf[recv_throttle];

	int err=0;
	if ( (err = SSL_read(con->transport.tls.ssl, buf, recv_throttle)) > 0 )
	{
		g_debug("SSL_read %i %.*s", err, err, buf);
		g_string_append_len(con->transport.tls.io_in, (gchar *)buf, err);
	}
	connection_tls_error(con);

	int action = SSL_get_error(con->transport.tls.ssl, err);
	connection_tls_error(con);

	if ( err<=0 )
	{
		switch ( action )
		{
		case SSL_ERROR_NONE:
			g_debug("%s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_ZERO_RETURN:
			g_debug("%s:%i", __FILE__,  __LINE__);
			connection_tls_shutdown_cb(EV_A_ w, revents);
			break;

		case SSL_ERROR_WANT_READ:
			g_debug("SSL_WANT_READ %s:%i", __FILE__,  __LINE__);
			// FIXME EAGAIN
			break;

		case SSL_ERROR_WANT_WRITE:
			g_debug("SSL_WANT_WRITE %s:%i", __FILE__,  __LINE__);
/*			ev_io_stop(EV_A_ &con->events.io_out);
			ev_io_init(&con->events.io_out, connection_tls_io_in_cb, con->socket, EV_WRITE);
 			ev_io_start(EV_A_ &con->events.io_out);
 */
			break;

		case SSL_ERROR_WANT_ACCEPT:
			g_debug("SSL_ERROR_WANT_ACCEPT%s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_WANT_X509_LOOKUP:
			g_debug("SSL_ERROR_WANT_X509_LOOKUP %s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_SYSCALL:
			g_debug("SSL_ERROR_SYSCALL %s:%i", __FILE__,  __LINE__);
			if ( err == 0 )
				g_debug("remote closed protocol, violating the specs!");
			else
				if ( err == -1 )
				perror("read");

			connection_tls_disconnect(con);
			break;

		case SSL_ERROR_SSL:
			g_debug("SSL_ERROR_SSL %s:%i", __FILE__,  __LINE__);
			break;
		}
	} else
	if ( err > 0 )
	{
		connection_throttle_update(con, &con->stats.io_in.throttle, err);
		if ( ev_is_active(&con->events.connect_timeout) )
			ev_timer_again(EV_A_  &con->events.connect_timeout);


		con->protocol.io_in(con, con->protocol.ctx, (unsigned char *)con->transport.tls.io_in->str, con->transport.tls.io_in->len);
		con->transport.tls.io_in->len = 0;
//		SSL_renegotiate(con->transport.ssl.ssl);

		if ( (con->transport.tls.io_out->len > 0 || con->transport.tls.io_out_again->len > 0 ) && !ev_is_active(&con->events.io_out) )
			ev_io_start(EV_A_ &con->events.io_out);

	}
}

void connection_tls_accept_cb (EV_P_ struct ev_io *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);
	struct connection *con = CONOFF_IO_IN(w);


	while ( 1 )
	{
		struct sockaddr_storage sa;
		socklen_t sizeof_sa = sizeof(struct sockaddr_storage);

		// clear accept timeout, reset


		int accepted_socket = accept(con->socket, (struct sockaddr *)&sa, &sizeof_sa);

		if ( accepted_socket == -1 && (errno == EAGAIN || errno == EWOULDBLOCK) )
			break;

		struct connection *accepted = connection_new(connection_transport_tls);
		SSL_CTX_free(accepted->transport.tls.ctx);
		connection_set_type(accepted, connection_type_accept);
		accepted->socket = accepted_socket;


		connection_node_set_local(accepted);
		connection_node_set_remote(accepted);

		g_debug("accept() %i local:'%s' remote:'%s'", accepted->socket, accepted->local.node_string,  accepted->remote.node_string);
		connection_set_nonblocking(accepted);

		// set protocol for accepted connection
		memcpy(&accepted->protocol, &con->protocol, sizeof(struct protocol));

		accepted->stats.io_out.throttle.max_bytes_per_second = con->stats.io_out.throttle.max_bytes_per_second;

		accepted->transport.tls.ctx = con->transport.tls.ctx;
		accepted->transport.tls.ssl = SSL_new(accepted->transport.tls.ctx);
		SSL_set_fd(accepted->transport.tls.ssl, accepted->socket);

		SSL_set_app_data(accepted->transport.tls.ssl, con);
//		SSL_set_app_data2(ssl, NULL); /* will be request_rec */
	
//		sslconn->ssl = ssl;
	
		/*
		 *  Configure callbacks for SSL connection
		 */
//		memcpy(accepted->transport.ssl.pTmpKeys, con->transport.ssl.pTmpKeys, sizeof(void *)*SSL_TMP_KEY_MAX);
//		accepted->transport.ssl.parent = con;
		SSL_set_tmp_rsa_callback(accepted->transport.tls.ssl, ssl_callback_TmpRSA);
		SSL_set_tmp_dh_callback(accepted->transport.tls.ssl,  ssl_callback_TmpDH);


		ev_timer_init(&accepted->events.handshake_timeout, connection_tls_accept_again_timeout_cb, 0., con->events.handshake_timeout.repeat);
		ev_timer_init(&accepted->events.connect_timeout, connection_tls_accept_again_timeout_cb, 0., con->events.connect_timeout.repeat);


		// create protocol specific data
		accepted->protocol.ctx = accepted->protocol.ctx_new(accepted);

		accepted->events.io_in.events = EV_READ;

		accepted->stats.io_in.throttle.max_bytes_per_second = con->stats.io_in.throttle.max_bytes_per_second;
		accepted->stats.io_out.throttle.max_bytes_per_second = con->stats.io_out.throttle.max_bytes_per_second;

		connection_set_state(accepted, connection_state_handshake);
		connection_tls_accept_again_cb(EV_A_ &accepted->events.io_in, 0);
	}

	if ( ev_is_active(&con->events.listen_timeout) )
	{
		ev_clear_pending(EV_A_ &con->events.listen_timeout);
		ev_timer_again(EV_A_  &con->events.listen_timeout);
	}
}

void connection_tls_accept_again_cb (EV_P_ struct ev_io *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);

	struct connection *con = NULL;

	if ( w->events == EV_READ )
		con = CONOFF_IO_IN(w);
	else
		con = CONOFF_IO_OUT(w);


	ev_io_stop(EV_A_ &con->events.io_in);
	ev_io_stop(EV_A_ &con->events.io_out);

	int err = SSL_accept(con->transport.tls.ssl);
	connection_tls_error(con);
	if ( err != 1 )
	{
		g_debug("setting connection_tls_accept_again_timeout_cb to %f",con->events.handshake_timeout.repeat);
		ev_timer_again(EV_A_ &con->events.handshake_timeout);

		int action = SSL_get_error(con->transport.tls.ssl, err);
		g_debug("SSL_accept failed %i %i read:%i write:%i", err, action, SSL_ERROR_WANT_READ, SSL_ERROR_WANT_WRITE);

		connection_tls_error(con);
		switch ( action )
		{
//		default:
		

		case SSL_ERROR_NONE:
			g_debug("%s:%i", __FILE__,  __LINE__);
			break;
		case SSL_ERROR_ZERO_RETURN:
			g_debug("%s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_WANT_READ:
			g_debug("SSL_WANT_READ %s:%i", __FILE__,  __LINE__);
			ev_io_init(&con->events.io_in, connection_tls_accept_again_cb, con->socket, EV_READ);
			ev_io_start(EV_A_ &con->events.io_in);
			break;

		case SSL_ERROR_WANT_WRITE:
			g_debug("SSL_WANT_WRITE %s:%i", __FILE__,  __LINE__);
			ev_io_init(&con->events.io_out, connection_tls_accept_again_cb, con->socket, EV_WRITE);
			ev_io_start(EV_A_ &con->events.io_out);
			break;

		case SSL_ERROR_WANT_ACCEPT:
			g_debug("SSL_ERROR_WANT_ACCEPT%s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_WANT_X509_LOOKUP:
			g_debug("SSL_ERROR_WANT_X509_LOOKUP %s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_SYSCALL:
			g_debug("SSL_ERROR_SYSCALL %s:%i", __FILE__,  __LINE__);
//			connection_tls_disconnect(EV_A_ con);
			break;

		case SSL_ERROR_SSL:
			g_debug("SSL_ERROR_SSL %s:%i", __FILE__,  __LINE__);
			break;

		}
	} else
	{
		g_debug("SSL_accept success");

		ev_timer_stop(EV_A_ &con->events.handshake_timeout);

		ev_timer_init(&con->events.connect_timeout, connection_tls_connect_timeout_cb, 0. ,con->events.connect_timeout.repeat);

		connection_established(con);

	}
}

void connection_tls_accept_again_timeout_cb (EV_P_ struct ev_timer *w, int revents)
{
	g_debug(__PRETTY_FUNCTION__);
	struct connection *con = CONOFF_HANDSHAKE_TIMEOUT(w);

	connection_tls_disconnect(con);
}

void connection_tls_disconnect(struct connection *con)
{
	g_debug("%s con %p",__PRETTY_FUNCTION__, con);

	connection_set_state(con, connection_state_close);

	connection_disconnect(con);

	if ( con->protocol.disconnect != NULL )
	{
		bool reconnect = con->protocol.disconnect(con, con->protocol.ctx);
		g_debug("reconnect is %i", reconnect);
		if ( reconnect == true && con->type == connection_type_connect )
		{
			connection_reconnect(con);
			return;
		}
	}
	connection_free(con);
}

void connection_tls_connecting_cb(EV_P_ struct ev_io *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);

	struct connection *con = CONOFF_IO_OUT(w);

	ev_timer_stop(EV_A_ &con->events.connecting_timeout);

	int socket_error = 0;
	int error_size = sizeof(socket_error);


	int ret = getsockopt(con->socket, SOL_SOCKET, SO_ERROR, &socket_error,(socklen_t *)&error_size);

	if ( ret != 0 || socket_error != 0 )
	{
		errno = socket_error;
		ev_io_stop(EV_A_ &con->events.io_out);
		close(con->socket);
		connection_connect_next_addr(con);
		return;
	}

	connection_node_set_local(con);
	connection_node_set_remote(con);

	g_debug("connection %s -> %s", con->local.node_string, con->remote.node_string);

	con->transport.tls.ssl = SSL_new(con->transport.tls.ctx);
	SSL_set_fd(con->transport.tls.ssl, con->socket);

	ev_timer_init(&con->events.handshake_timeout, connection_tls_connect_again_timeout_cb, 0., con->events.handshake_timeout.repeat);

	connection_set_state(con, connection_state_handshake);
	connection_tls_connect_again_cb(EV_A_ w, revents);
}

void connection_tls_connecting_timeout_cb(EV_P_ struct ev_timer *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);
	struct connection *con = w->data;
	connection_tls_disconnect(con);
}

void connection_tls_connect_again_cb(EV_P_ struct ev_io *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);

	struct connection *con = NULL;
	if ( w->events == EV_READ )
		con = CONOFF_IO_IN(w);
	else
		con = CONOFF_IO_OUT(w);

	ev_io_stop(EV_A_ &con->events.io_in);
	ev_io_stop(EV_A_ &con->events.io_out);


	int err = SSL_connect(con->transport.tls.ssl);
	connection_tls_error(con);
	if ( err != 1 )
	{
		ev_timer_again(EV_A_ &con->events.handshake_timeout);
		int action = SSL_get_error(con->transport.tls.ssl, err);
		connection_tls_error(con);

		switch ( action )
		{
//		default:
		

		case SSL_ERROR_NONE:
			g_debug("%s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_ZERO_RETURN:
			g_debug("%s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_WANT_READ:
			g_debug("SSL_WANT_READ %s:%i", __FILE__,  __LINE__);
			ev_io_init(&con->events.io_in, connection_tls_connect_again_cb, con->socket, EV_READ);
			ev_io_start(EV_A_ &con->events.io_in);
			break;

		case SSL_ERROR_WANT_WRITE:
			g_debug("SSL_WANT_WRITE %s:%i", __FILE__,  __LINE__);
			ev_io_init(&con->events.io_out, connection_tls_connect_again_cb, con->socket, EV_WRITE);
			ev_io_start(EV_A_ &con->events.io_out);
			break;

		case SSL_ERROR_WANT_ACCEPT:
			g_debug("SSL_ERROR_WANT_ACCEPT%s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_WANT_X509_LOOKUP:
			g_debug("SSL_ERROR_WANT_X509_LOOKUP %s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_SYSCALL:
			g_debug("SSL_ERROR_SYSCALL %s:%i", __FILE__,  __LINE__);
			break;

		case SSL_ERROR_SSL:
			g_debug("SSL_ERROR_SSL %s:%i", __FILE__,  __LINE__);
			break;

		}   
	} else
	{
		g_debug("SSL_connect success");
		ev_timer_stop(EV_A_ &con->events.handshake_timeout);
		ev_timer_init(&con->events.connect_timeout, connection_tls_connect_timeout_cb, 0. ,con->events.connect_timeout.repeat);
		connection_established(con);
	}
}

void connection_tls_connect_timeout_cb(EV_P_ struct ev_timer *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);
	
	struct connection *con = CONOFF_CONNECT_TIMEOUT(w);

	if ( con->protocol.timeout == NULL || con->protocol.timeout(con, con->protocol.ctx) == false )
		connection_close(con);
	else
		ev_timer_again(CL, &con->events.connect_timeout);
}

void connection_tls_connect_again_timeout_cb(EV_P_ struct ev_timer *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);
	struct connection *con = CONOFF_HANDSHAKE_TIMEOUT(w);
	connection_tls_disconnect(con);
}



void connection_tls_error(struct connection *con)
{
	con->transport.tls.ssl_error = ERR_get_error();
	ERR_error_string(con->transport.tls.ssl_error, con->transport.tls.ssl_error_string);
	if ( con->transport.tls.ssl_error != 0 )
		g_debug("SSL ERROR %s\t%s", con->transport.tls.ssl_error_string, SSL_state_string_long(con->transport.tls.ssl));
}

void connection_tls_listen_timeout_cb(EV_P_ struct ev_timer *w, int revents)
{
	g_debug(__PRETTY_FUNCTION__);
	struct connection *con = CONOFF_LISTEN_TIMEOUT(w);
	connection_tls_disconnect(con);
}

/*
 *
 * connection udp
 *
 */



void connection_udp_io_in_cb(EV_P_ struct ev_io *w, int revents)
{
	g_debug(__PRETTY_FUNCTION__);
	struct connection *con = CONOFF_IO_IN(w);
	struct sockaddr_storage sa;
	socklen_t sizeof_sa = sizeof(struct sockaddr_storage);
	unsigned char buf[64*1024];
	memset(buf, 0, 64*1024);
	int ret;
	while ( (ret = recvfrom(con->socket, buf, 64*1024, 0,  (struct sockaddr *)&sa, &sizeof_sa)) > 0 )
	{
		memcpy(&con->remote.addr, &sa, sizeof(struct sockaddr_storage));
		node_info_set(&con->remote, &sa);
//		g_debug("%s -> %s %.*s", con->remote.node_string, con->local.node_string, ret, buf);
		con->protocol.io_in(con, con->protocol.ctx, buf, ret);
		memset(buf, 0, 64*1024);
	}
}

void connection_udp_io_out_cb(EV_P_ struct ev_io *w, int revents)
{
	g_debug(__PRETTY_FUNCTION__);
	struct connection *con = CONOFF_IO_OUT(w);

	GList *elem;

//	g_debug("sending ");
	while ( (elem = g_list_first(con->transport.udp.io_out)) != NULL )
	{
//		g_debug(".");
		struct udp_packet *packet = elem->data;

		socklen_t size = ((struct sockaddr *)&packet->to)->sa_family == PF_INET ? sizeof(struct sockaddr_in) : 
			((struct sockaddr *)&packet->to)->sa_family == PF_INET6 ? sizeof(struct sockaddr_in6) : 
				((struct sockaddr *)&packet->to)->sa_family == AF_UNIX ? sizeof(struct sockaddr_un) : -1;

		int ret = sendto(con->socket, packet->data->str, packet->data->len, 0, (struct sockaddr *)&packet->to, size);

		if ( ret == -1 )
		{
			if ( errno != EAGAIN )
			{
				g_debug("domain %i size %i", ((struct sockaddr *)&packet->to)->sa_family, size);
				perror("sendto");
			}
			break;
		} else
			if ( ret == packet->data->len )
		{
			g_string_free(packet->data, TRUE);
			g_free(packet);
			con->transport.udp.io_out = g_list_delete_link(con->transport.udp.io_out, elem);
		} else
		{
			perror("sendto");
		}
	}
//	g_debug(" done");
	if ( g_list_length(con->transport.udp.io_out) > 0 )
	{
		if ( !ev_is_active(&con->events.io_out) )
		{
			ev_io_init(&con->events.io_out, connection_udp_io_out_cb, con->socket, EV_WRITE);
			ev_io_start(EV_A_ &con->events.io_out);
		}
	} else
	{
		ev_io_stop(EV_A_ &con->events.io_out);
	}
	if ( ev_is_active(&con->events.connect_timeout) )
		ev_timer_again(CL, &con->events.connect_timeout);
}

void connection_udp_connect_timeout_cb(EV_P_ struct ev_timer *w, int revents)
{
	g_debug("%s loop %p watcher %p con %p",__PRETTY_FUNCTION__, EV_A_ w, w->data);
	struct connection *con = CONOFF_CONNECT_TIMEOUT(w);

	if ( con->protocol.timeout == NULL || con->protocol.timeout(con, con->protocol.ctx) == false )
	{
		ev_timer_stop(EV_A_ w);
		connection_udp_disconnect(con);
	} else
		ev_timer_again(CL, &con->events.connect_timeout);
}

void connection_udp_disconnect(struct connection *con)
{
	g_debug("%s con %p",__PRETTY_FUNCTION__, con);
	connection_set_state(con, connection_state_close);
	con->protocol.disconnect(con, con->protocol.ctx);
	connection_disconnect(con);
	connection_free(con);
}

/*
 *
 * connection resolve
 *
 */

void connection_dns_resolve_timeout_cb(EV_P_ struct ev_timer *w, int revent)
{
	g_debug("%s loop %p ev_timer %p revent %i", __PRETTY_FUNCTION__ ,EV_A_ w, revent);
	struct connection *con = CONOFF_DNS_TIMEOUT(w);
	if ( con->remote.dns.a != NULL )
		dns_cancel(g_dionaea->dns->dns, con->remote.dns.a);
	if ( con->remote.dns.aaaa != NULL )
		dns_cancel(g_dionaea->dns->dns, con->remote.dns.aaaa);

	con->protocol.error(con, ETIME);
	connection_close(con);

}

void connection_connect_resolve(struct connection *con)
{
	g_debug(__PRETTY_FUNCTION__);

	g_debug("submitting dns %s", con->remote.hostname);

	con->remote.dns.a = dns_submit_p(g_dionaea->dns->dns, 
									 con->remote.hostname, 
									 DNS_C_IN, 
									 DNS_T_A, 
									 0, 
									 dns_parse_a4, 
									 connection_connect_resolve_a_cb, 
									 con);
	con->remote.dns.aaaa = dns_submit_p(g_dionaea->dns->dns, 
										con->remote.hostname, 
										DNS_C_IN, 
										DNS_T_AAAA, 
										0, 
										dns_parse_a6, 
										connection_connect_resolve_aaaa_cb, 
										con);

	connection_set_state(con, connection_state_resolve);
	con->events.dns_timeout.data = con;
	ev_timer_init(&con->events.dns_timeout, connection_dns_resolve_timeout_cb, 10., .0);
	ev_timer_start(g_dionaea->loop, &con->events.dns_timeout);
	return;
}

static inline int ipv6_addr_v4mapped(const struct in6_addr *a)
{
	return ((a->s6_addr32[0] | a->s6_addr32[1]) == 0 &&
		 a->s6_addr32[2] == htonl(0x0000ffff));
}


static int cmp_ip_address_stringp(const void *p1, const void *p2)
{
//	g_debug("%s",__PRETTY_FUNCTION__);
	struct sockaddr_storage sa1, sa2;
	int domain1,domain2;
	socklen_t sizeof_sa1, sizeof_sa2;

	parse_addr(*(const char **)p1, NULL, 0, &sa1, &domain1, &sizeof_sa1);
	parse_addr(*(const char **)p2, NULL, 0, &sa2, &domain2, &sizeof_sa2);

	if(domain1 == domain2)
	{
#define ADDROFFSET(x) \
	((((struct sockaddr *)(x))->sa_family == AF_INET) ?  \
		((void *)(x) + offsetof(struct sockaddr_in, sin_addr)) :  \
		(((struct sockaddr *)(x))->sa_family == AF_INET6) ? \
			((void *)(x) + offsetof(struct sockaddr_in6, sin6_addr)) : \
			NULL)

		void *a = ADDROFFSET(&sa1);
		void *b = ADDROFFSET(&sa2);

#undef ADDROFFSET

		if ( domain1 == PF_INET6)
		{
			if (ipv6_addr_v4mapped(a) && 
				ipv6_addr_v4mapped(b))
				return -memcmp(a, b, sizeof_sa1);

			if ( ipv6_addr_v4mapped(a) )
				return 1;

			if ( ipv6_addr_v4mapped(b) )
				return -1;
		}

		return -memcmp(a, b, sizeof_sa1);

	}else
	if(domain1 > domain2) // domain1 is ipv6
	{
		struct sockaddr_in6 *a = (struct sockaddr_in6 *)&sa1;     
		struct sockaddr_in *b  = (struct sockaddr_in *)&sa2;    
		if ( ipv6_addr_v4mapped(&a->sin6_addr) )
			return -memcmp(&a->sin6_addr.s6_addr32[3], &b->sin_addr.s_addr, sizeof_sa2);
			
		return -1;
	}else				// domain2 is ipv6
	{
		struct sockaddr_in6 *a = (struct sockaddr_in6 *)&sa2;     
		struct sockaddr_in *b  = (struct sockaddr_in *)&sa1;    
		if ( ipv6_addr_v4mapped(&a->sin6_addr) )
			return memcmp(&a->sin6_addr.s6_addr32[3], &b->sin_addr.s_addr, sizeof_sa2);

		return 1;
	}
}

void connection_connect_resolve_action(struct connection *con)
{
	if ( con->remote.dns.a == NULL && con->remote.dns.aaaa == NULL )
	{
		ev_timer_stop(g_dionaea->loop, &con->events.dns_timeout);

		if ( con->remote.dns.resolved_address_count == 0 )
		{
			con->protocol.error(con, EADDRNOTAVAIL);
			connection_close(con);
			return;
		}

		qsort(con->remote.dns.resolved_addresses, con->remote.dns.resolved_address_count, sizeof(char *), cmp_ip_address_stringp);
		int i;
		for(i=0;i<con->remote.dns.resolved_address_count;i++)
		{
			g_debug("node address %s", con->remote.dns.resolved_addresses[i]);
		}
//		return;
		connection_connect_next_addr(con);
	}
}

void connection_connect_resolve_a_cb(struct dns_ctx *ctx, void *result, void *data)
{
	g_debug("%s ctx %p result %p data %p",__PRETTY_FUNCTION__, ctx, result, data);
	struct connection *con = data;

	struct dns_rr_a4 *a4 = result;

	if ( result )
	{
		int i=0;
		for ( i=0;i<a4->dnsa4_nrr; i++ )
		{
			char addr[INET6_ADDRSTRLEN];

			inet_ntop(PF_INET, &a4->dnsa4_addr[i], addr, INET6_ADDRSTRLEN);
			g_debug("\t%s",addr);
			node_info_add_addr(&con->remote, addr);
		}
	}
	con->remote.dns.a = NULL;

	connection_connect_resolve_action(con);
}

void connection_connect_resolve_aaaa_cb(struct dns_ctx *ctx, void *result, void *data)
{
	g_debug("%s ctx %p result %p data %p",__PRETTY_FUNCTION__, ctx, result, data);		struct connection *con = data;

	struct dns_rr_a6 *a6 = result;

	if ( result )
	{
		int i=0;
		for ( i=0;i<a6->dnsa6_nrr; i++ )
		{
			char addr[INET6_ADDRSTRLEN];

			inet_ntop(PF_INET6, &a6->dnsa6_addr[i], addr, INET6_ADDRSTRLEN);
			g_debug("\t%s",addr);
			node_info_add_addr(&con->remote, addr);
		}
	}
	con->remote.dns.aaaa = NULL;

	connection_connect_resolve_action(con);
}


bool connection_transport_from_string(const char *type_str, enum connection_transport *type)
{
	if ( strcmp(type_str, "tcp") == 0 )
		*type = connection_transport_tcp;
	else if ( strcmp(type_str, "udp") == 0 )
		*type = connection_transport_udp;
	else if ( strcmp(type_str, "tls") == 0 )
		*type = connection_transport_tls;
	else
		return false;

	return true;
}

void connection_protocol_set(struct connection *con, struct protocol *proto)
{
	memcpy(&con->protocol, proto, sizeof(struct protocol));
}

void connection_protocol_ctx_set(struct connection *con, void *data)
{
	g_debug("%s con %p data %p", __PRETTY_FUNCTION__, con, data);
	con->protocol.ctx = data;
}

void *connection_protocol_ctx_get(struct connection *con)
{
	g_debug("%s con %p data %p", __PRETTY_FUNCTION__, con, con->protocol.ctx);
	return con->protocol.ctx;
}

const char *connection_type_str[] = 
{
	"none",
	"accept", 
	"bind", 
	"connect", 
	"listen", 

};


void connection_set_type(struct connection *con, enum connection_type type)
{
	enum connection_type old_type;
	old_type = con->type;
	con->type = type;
	g_message("connection %p type %s -> %s %s->%s", con, con->local.node_string, con->remote.node_string, connection_type_str[old_type], connection_type_str[type]);
}

void connection_set_state(struct connection *con, enum connection_state state)
{
	const char *connection_state_str[] = 
	{
		"none",
		"resolve",
		"connecting",
		"handshake",
		"connected",
		"shutdown",
		"close",
		"reconnect"
	};
	enum connection_state old_state;
	old_state = con->state;
	con->state = state;
	g_message("connection %p state %s %s -> %s %s->%s", con, connection_type_str[con->type], con->local.node_string, con->remote.node_string, connection_state_str[old_state], connection_state_str[state]);
}

