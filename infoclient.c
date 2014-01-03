/*
 *  infoclient.c - Example client program to talk to the "infosvr"
 *  program running on ASUS WL-500g routers.
 *
 *  Copyright (c) 2005 Andy Walls <awalls@md.metrocast.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA
 */
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#define PROGNAME            "infoclient"
#define INFO_SERVER_PORT    (9999)
#define INFO_PACKET_SZ      (512)

typedef struct
{
	u_int8_t  service;
	u_int8_t  cmd_rsp;
	u_int16_t operation;
	u_int32_t id;
} info_packet_header_t;

static u_int32_t next_id = 0;

//#define INFO_SERVICE_LPT_EMULATION (11)
#define INFO_SERVICE_IBOX          (12)

#define INFO_PACKET_COMMAND        (21)
#define INFO_PACKET_RESPONSE       (22)

#define INFO_OPERATION_GETINFO     (31)
#define INFO_OPERATION_MANU_CMD    (51) /* system command, no auth */

int get_udp_socket (void)
{
	int s;
	if ((s = socket (AF_INET, SOCK_DGRAM, 0)) < 0)
	{
		perror (PROGNAME ": get_udp_socket(): socket()");
	}
	return s;
}

// Convert (1234, "1.2.3.4") to (sockaddr_in) {AF_INET, 1234, 1.2.3.4}
int make_sockaddr_in (u_int16_t port, char *address, struct sockaddr *s_addr)
{
	struct sockaddr_in *s_addr_in;

	s_addr_in = (struct sockaddr_in *) s_addr;
	memset (s_addr_in, 0, sizeof (struct sockaddr_in));

	s_addr_in->sin_family = AF_INET;
	s_addr_in->sin_port   = htons (port);
	return inet_aton (address, &(s_addr_in->sin_addr));
}

int bind_socket (int s, u_int16_t port, char *address)
{
	struct sockaddr s_addr;
	int r;

	if ((r = make_sockaddr_in (port, address, &s_addr)) < 0)
	{
		fprintf (stderr, PROGNAME
		                 ": bind_socket(): make_sockaddr_in(): failed");
		return r;
	}

	if ((r = bind (s, &s_addr, (socklen_t) sizeof (s_addr))) < 0)
	{
		perror (PROGNAME ": bind_socket(): bind()");
	}

	return r;
}

int set_broadcast_option (int s, int flag)
{
	int r;
	r = setsockopt (s, SOL_SOCKET, SO_BROADCAST,
	                                         (char *) &flag, sizeof (flag));
	if (r < 0)
	{
		perror (PROGNAME ": set_broadcast_option(): setsockopt()");
	}
	return r;
}

int init_socket (void)
{
	int s, r;

	if ((s = get_udp_socket()) < 0)
	{
		return s;	
	}

	// Prepare to receive all traffic from the infoserver port on
	// all local interfaces (INADDR_ANY)
	if ((r = bind_socket (s, INFO_SERVER_PORT, "0.0.0.0")) < 0)
	{
		close (s);
		return r;
	}

	// Allow socket to transmit to broadcast address
	if ((r = set_broadcast_option (s, 1)) < 0)
	{
		close (s);
		return r;
	}

	return s;
}

void build_getinfo_cmd (char *buffer)
{
	info_packet_header_t *hdr;

	memset (buffer, 0, INFO_PACKET_SZ);
	hdr = (info_packet_header_t *) buffer;

	hdr->service   = INFO_SERVICE_IBOX;
	hdr->cmd_rsp   = INFO_PACKET_COMMAND;
	hdr->operation = INFO_OPERATION_GETINFO;
	hdr->id        = next_id++;
}

int send_cmd (int s, struct sockaddr *s_addr, socklen_t salen, char *buffer)
{
	int r;
	size_t len;
	char *p;
	fd_set rfds, wfds, efds;

	FD_ZERO (&rfds);
	FD_ZERO (&efds);
	for (p = buffer, len = INFO_PACKET_SZ; len > 0; )
	{
		FD_ZERO (&wfds);
		FD_SET  (s, &wfds);
		r = select (s+1, &rfds, &wfds, &efds, NULL);
		if (r == 0)
		{
			// Timeout, retry
			continue;
		}
		else if (r < 0)
		{
			if (errno == EINTR)
			{
				// Signal interrupted us, retry
				continue;
			}
			else
			{
				perror (PROGNAME ": send_cmd(): select()");
				return -1;
			}
		}
		else if (!(FD_ISSET (s, &wfds)))
		{
			// Not ours?  retry.
			continue;
		}

		r = sendto (s, p, len, 0, s_addr, salen);
		if (r < 0)
		{
			if (errno == EINTR)
			{
				// Signal interrupted us, retry
				continue;
			}
			else
			{
				perror (PROGNAME ": send_cmd(): sendto()");
				return -1;
			}
		}
		p += r;
		len -= r;
	}
	return 0;
}

int recv_rsp (int s, struct sockaddr *s_addr, socklen_t *salen, char *buffer)
{
	int r;
	size_t len;
	char *p;
	fd_set rfds, wfds, efds;

	FD_ZERO (&wfds);
	FD_ZERO (&efds);
	for (p = buffer, len = INFO_PACKET_SZ; len > 0; )
	{
		FD_ZERO (&rfds);
		FD_SET  (s, &rfds);
		r = select (s+1, &rfds, &wfds, &efds, NULL);
		if (r == 0)
		{
			// Timeout, retry
			continue;
		}
		else if (r < 0)
		{
			if (errno == EINTR)
			{
				// Signal interrupted us, retry
				continue;
			}
			else
			{
				perror (PROGNAME ": recv_rsp(): select()");
				return -1;
			}
		}
		else if (!(FD_ISSET (s, &rfds)))
		{
			// Not ours?  retry.
			continue;
		}

		r = recvfrom (s, p, len, 0, s_addr, salen);
		if (r < 0)
		{
			if (errno == EINTR)
			{
				// Signal interrupted us, retry
				continue;
			}
			else
			{
				perror (PROGNAME ": recv_rsp(): recvfrom()");
				return -1;
			}
		}
		p += r;
		len -= r;
	}
	return 0;
}

int main (int argc, char *argv[])
{
	int s, r;
	struct sockaddr bcast_addr, resp_addr;
	socklen_t resp_alen;
	char buffer [INFO_PACKET_SZ];
	
	if ((s = init_socket()) < 0)
	{
		exit(1);
	}


	r = make_sockaddr_in (INFO_SERVER_PORT, "255.255.255.255", &bcast_addr);
	if (r < 0)
	{
		fprintf (stderr, PROGNAME
		                 ": main(): make_sockaddr_in(): failed");
		close (s);
		exit (2);
	}

	build_getinfo_cmd (buffer);
	r = send_cmd (s, &bcast_addr, (socklen_t) sizeof (bcast_addr), buffer);
	if (r < 0)
	{
		close (s);
		exit (3);
	}

	r = recv_rsp (s, &resp_addr, &resp_alen, buffer);
	if (r < 0)
	{
		close (s);
		exit (4);
	}
	for (r = 0; r < INFO_PACKET_SZ; r++)
	{
		putchar (buffer[r]);
	} 
	fflush (stdout);

	r = recv_rsp (s, &resp_addr, &resp_alen, buffer);
	if (r < 0)
	{
		close (s);
		exit (5);
	}
	for (r = 0; r < INFO_PACKET_SZ; r++)
	{
		//FIXME fwrite
		putchar (buffer[r]);
	}
	fflush (stdout);

	close (s);
	exit (0);
}
