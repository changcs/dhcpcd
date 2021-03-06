/* 
 * dhcpcd - DHCP client daemon
 * Copyright 2006-2008 Roy Marples <roy@marples.name>
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <netinet/in.h>
#include <net/if_arp.h>

#include <arpa/inet.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#include "common.h"
#include "dhcpcd.h"
#include "dhcp.h"
#include "interface.h"
#include "logger.h"
#include "socket.h"

#ifndef STAILQ_CONCAT
#define	STAILQ_CONCAT(head1, head2) do {				\
	if (!STAILQ_EMPTY((head2))) {					\
		*(head1)->stqh_last = (head2)->stqh_first;		\
		(head1)->stqh_last = (head2)->stqh_last;		\
		STAILQ_INIT((head2));					\
	}								\
} while (0)
#endif

typedef struct message {
	int value;
	const char *name;
} dhcp_message_t;

static dhcp_message_t dhcp_messages[] = {
	{ DHCP_DISCOVER, "DHCP_DISCOVER" },
	{ DHCP_OFFER,    "DHCP_OFFER" },
	{ DHCP_REQUEST,  "DHCP_REQUEST" },
	{ DHCP_DECLINE,  "DHCP_DECLINE" },
	{ DHCP_ACK,      "DHCP_ACK" },
	{ DHCP_NAK,      "DHCP_NAK" },
	{ DHCP_RELEASE,  "DHCP_RELEASE" },
	{ DHCP_INFORM,   "DHCP_INFORM" },
	{ -1, NULL }
};

static const char *dhcp_message (int type)
{
	dhcp_message_t *d;
	for (d = dhcp_messages; d->name; d++)
		if (d->value == type)
			return (d->name);

	return (NULL);
}

ssize_t send_message (const interface_t *iface, const dhcp_t *dhcp,
		      uint32_t xid, char type, const options_t *options)
{
	struct udp_dhcp_packet *packet;
	dhcpmessage_t *message;
	unsigned char *m;
	unsigned char *p;
	unsigned char *n_params = NULL;
	size_t l;
	struct in_addr from;
	struct in_addr to;
	time_t up = uptime() - iface->start_uptime;
	uint32_t ul;
	uint16_t sz;
	size_t message_length;
	ssize_t retval;

	if (!iface || !options || !dhcp)
		return -1;

	memset (&from, 0, sizeof (from));
	memset (&to, 0, sizeof (to));

	if (type == DHCP_RELEASE)
		to.s_addr = dhcp->serveraddress.s_addr;

	message = xzalloc (sizeof (*message));
	m = (unsigned char *) message;
	p = (unsigned char *) &message->options;

	if ((type == DHCP_INFORM ||
	     type == DHCP_RELEASE ||
	     type == DHCP_REQUEST) &&
	    ! IN_LINKLOCAL (ntohl (iface->previous_address.s_addr)))
	{
		message->ciaddr = iface->previous_address.s_addr;
		from.s_addr = iface->previous_address.s_addr;

		/* Just incase we haven't actually configured the address yet */
		if (type == DHCP_INFORM && iface->previous_address.s_addr == 0)
			message->ciaddr = dhcp->address.s_addr;

		/* Zero the address if we're currently on a different subnet */
		if (type == DHCP_REQUEST &&
		    iface->previous_netmask.s_addr != dhcp->netmask.s_addr)
			message->ciaddr = from.s_addr = 0;

		if (from.s_addr != 0)
			to.s_addr = dhcp->serveraddress.s_addr;
	}

	message->op = DHCP_BOOTREQUEST;
	message->hwtype = iface->family;
	switch (iface->family) {
		case ARPHRD_ETHER:
		case ARPHRD_IEEE802:
			message->hwlen = ETHER_ADDR_LEN;
			memcpy (&message->chaddr, &iface->hwaddr,
				ETHER_ADDR_LEN);
			break;
		case ARPHRD_IEEE1394:
		case ARPHRD_INFINIBAND:
			message->hwlen = 0;
			if (message->ciaddr == 0)
				message->flags = htons (BROADCAST_FLAG);
			break;
		default:
			logger (LOG_ERR, "dhcp: unknown hardware type %d",
				iface->family);
	}

	if (up < 0 || up > (time_t) UINT16_MAX)
		message->secs = htons ((uint16_t) UINT16_MAX);
	else
		message->secs = htons (up);
	message->xid = xid;
	message->cookie = htonl (MAGIC_COOKIE);

	*p++ = DHCP_MESSAGETYPE; 
	*p++ = 1;
	*p++ = type;

	if (type == DHCP_REQUEST) {
		*p++ = DHCP_MAXMESSAGESIZE;
		*p++ = 2;
		sz = get_mtu (iface->name);
		if (sz < MTU_MIN) {
			if (set_mtu (iface->name, MTU_MIN) == 0)
				sz = MTU_MIN;
		}
		else if(sz > (sizeof(dhcpmessage_t) + DHCP_UDP_LEN)) {
			/*
			 * bigger dhcp messages are not supported,
			 * so don't indicate more (full MTU) ...
			 */
			sz = sizeof(dhcpmessage_t) + DHCP_UDP_LEN;
		}
		sz = htons (sz);
		memcpy (p, &sz, 2);
		p += 2;
	}

	*p++ = DHCP_CLIENTID;
	*p++ = iface->clientid_len;
	memcpy (p, iface->clientid, iface->clientid_len);
	p+= iface->clientid_len;

	if (type != DHCP_DECLINE && type != DHCP_RELEASE) {
		if (options->userclass_len > 0) {
			*p++ = DHCP_USERCLASS;
			*p++ = options->userclass_len;
			memcpy (p, &options->userclass, options->userclass_len);
			p += options->userclass_len;
		}

		if (*options->classid > 0) {
			*p++ = DHCP_CLASSID;
			*p++ = l = strlen (options->classid);
			memcpy (p, options->classid, l);
			p += l;
		}
	}

	if (type == DHCP_DISCOVER || type == DHCP_REQUEST) {
#define PUTADDR(_type, _val) { \
	*p++ = _type; \
	*p++ = 4; \
	memcpy (p, &_val.s_addr, 4); \
	p += 4; \
}
		if (IN_LINKLOCAL (ntohl (dhcp->address.s_addr)))
			logger (LOG_ERR,
				"cannot request a link local address");
		else {
			if (dhcp->address.s_addr &&
			    dhcp->address.s_addr !=
			    iface->previous_address.s_addr)
			{
				PUTADDR (DHCP_ADDRESS, dhcp->address);
				if (dhcp->serveraddress.s_addr)
					PUTADDR (DHCP_SERVERIDENTIFIER,
						 dhcp->serveraddress);
			}
		}
#undef PUTADDR

		if (options->leasetime != 0) {
			*p++ = DHCP_LEASETIME;
			*p++ = 4;
			ul = htonl (options->leasetime);
			memcpy (p, &ul, 4);
			p += 4;
		}
	}

	if (type == DHCP_DISCOVER ||
	    type == DHCP_INFORM ||
	    type == DHCP_REQUEST)
	{
		if (options->hostname[0]) {
			if (options->fqdn == FQDN_DISABLE) {
				*p++ = DHCP_HOSTNAME;
				*p++ = l = strlen (options->hostname);
				memcpy (p, options->hostname, l);
				p += l;
			} else {
				/* Draft IETF DHC-FQDN option (81) */
				*p++ = DHCP_FQDN;
				*p++ = (l = strlen (options->hostname)) + 3;
				/* Flags: 0000NEOS
				 * S: 1 => Client requests Server to update
				 *         a RR in DNS as well as PTR
				 * O: 1 => Server indicates to client that
				 *         DNS has been updated
				 * E: 1 => Name data is DNS format
				 * N: 1 => Client requests Server to not
				 *         update DNS
				 */
				*p++ = options->fqdn & 0x9;
				*p++ = 0; /* from server for PTR RR */
				*p++ = 0; /* from server for A RR if S=1 */
				memcpy (p, options->hostname, l);
				p += l;
			}
		}

		*p++ = DHCP_PARAMETERREQUESTLIST;
		n_params = p;
		*p++ = 0;
		/* Only request DNSSERVER in discover to keep the packets small.
		 * RFC2131 Section 3.5 states that the REQUEST must include the
		 * list from the DISCOVER message, so I think this is ok. */

		if (type == DHCP_DISCOVER && ! options->test)
			*p++ = DHCP_DNSSERVER;
		else {
			if (type != DHCP_INFORM) {
				*p++ = DHCP_RENEWALTIME;
				*p++ = DHCP_REBINDTIME;
			}
			*p++ = DHCP_NETMASK;
			*p++ = DHCP_BROADCAST;

			/* -S means request CSR and MSCSR
			 * -SS means only request MSCSR incase DHCP message
			 *  is too big */
			if (options->domscsr < 2)
				*p++ = DHCP_CSR;
			if (options->domscsr > 0)
				*p++ = DHCP_MSCSR;
			/* RFC 3442 states classless static routes should be
			 * before routers and static routes as classless static
			 * routes override them both */
			*p++ = DHCP_STATICROUTE;
			*p++ = DHCP_ROUTERS;
			*p++ = DHCP_HOSTNAME;
			*p++ = DHCP_DNSSEARCH;
			*p++ = DHCP_DNSDOMAIN;
			*p++ = DHCP_DNSSERVER;
#ifdef ENABLE_NIS
			*p++ = DHCP_NISDOMAIN;
			*p++ = DHCP_NISSERVER;
#endif
#ifdef ENABLE_NTP
			*p++ = DHCP_NTPSERVER;
#endif
			*p++ = DHCP_MTU;
#ifdef ENABLE_INFO
			*p++ = DHCP_ROOTPATH;
			*p++ = DHCP_SIPSERVER;
#endif
		}

		*n_params = p - n_params - 1;
	}
	*p++ = DHCP_END;

#ifdef BOOTP_MESSAGE_LENTH_MIN
	/* Some crappy DHCP servers think they have to obey the BOOTP minimum
	 * message length.
	 * They are wrong, but we should still cater for them. */
	while (p - m < BOOTP_MESSAGE_LENTH_MIN)
		*p++ = DHCP_PAD;
#endif

	message_length = p - m;
	
#if __linux__
	/* send_packet() always sends to the hardware broadcast address, which is
	   broken when the client and server are in different broadcast domains
	   and using a dhcp-helper and a renewal is in progress, since the client 
	   sends direct to the server and the intermediate router will likely drop 
	   broadcast addresses. 

	   This fix only works under Linux, where we have a UDP socket suitabley 
	   set up which we can use to send in that case. */

	if (to.s_addr != 0 && iface->listen_fd != -1)
	  {
	    struct sockaddr_in dest;
	    dest.sin_family = AF_INET;
	    dest.sin_port = htons (DHCP_SERVER_PORT);
	    dest.sin_addr = to;

	    logger (LOG_DEBUG, "sending %s with xid 0x%x over UDP socket",
		    dhcp_message (type), xid);
	    
	    retval = sendto(iface->listen_fd, message, message_length, 0 , 
			    (struct sockaddr *)&dest, sizeof(dest));
	    free(message);

	    return retval;
	  }
#endif

	packet = xzalloc (sizeof (*packet));
	make_dhcp_packet (packet, (unsigned char *) message, message_length,
			  from, to);
	free (message);

	logger (LOG_DEBUG, "sending %s with xid 0x%x",
		dhcp_message (type), xid);
	retval = send_packet (iface, ETHERTYPE_IP, (unsigned char *) packet,
			      message_length +
			      sizeof (packet->ip) + sizeof (packet->udp));
	free (packet);
	return (retval);
}

/* Decode an RFC3397 DNS search order option into a space
 * seperated string. Returns length of string (including 
 * terminating zero) or zero on error. out may be NULL
 * to just determine output length. */
static unsigned int decode_search (const unsigned char *p, int len, char *out)
{
	const char *start;
	const unsigned char *r, *q = p;
	unsigned int count = 0, l, hops;

	start = out;
	while (q - p < len) {
		r = NULL;
		hops = 0;
		while ((l = *q++) && q - p < len) {
			unsigned int label_type = l & 0xc0;
			if (label_type == 0x80 || label_type == 0x40)
				return 0;
			else if (label_type == 0xc0) { /* pointer */
				l = (l & 0x3f) << 8;
				l |= *q++;

				/* save source of first jump. */
				if (!r)
					r = q;

				hops++;
				if (hops > 255)
					return 0;

				q = p + l;
				if (q - p >= len)
					return 0;
			} else {
				/* straightforward name segment, add with '.' */
				count += l + 1;
				if (out) {
					memcpy (out, q, l);
					out += l;
					*out++ = '.';
				}
				q += l;
			}
		}

		/* change last dot to space */
		if (out && out != start)
			*(out - 1) = ' ';

		if (r)
			q = r;
	}

	/* change last space to zero terminator */
	if (out) {
		if (out != start)
			*(out - 1) = '\0';
		else if (len > 0)
			*out = '\0';
	}

	return count;  
}

/* Add our classless static routes to the routes variable
 * and return the last route set */
static struct route_head *decode_CSR (const unsigned char *p, int len)
{
	const unsigned char *q = p;
	unsigned int cidr;
	unsigned int ocets;
	struct route_head *routes = NULL;
	route_t *route;

	/* Minimum is 5 -first is CIDR and a router length of 4 */
	if (len < 5)
		return NULL;

	while (q - p < len) {
		if (! routes) {
			routes = xmalloc (sizeof (*routes));
			STAILQ_INIT (routes);
		}

		route = xzalloc (sizeof (*route));

		cidr = *q++;
		if (cidr > 32) {
			logger (LOG_ERR,
				"invalid CIDR of %d in classless static route",
				cidr);
			free_route (routes);
			return (NULL);
		}
		ocets = (cidr + 7) / 8;

		if (ocets > 0) {
			memcpy (&route->destination.s_addr, q, (size_t) ocets);
			q += ocets;
		}

		/* Now enter the netmask */
		if (ocets > 0) {
			memset (&route->netmask.s_addr, 255, (size_t) ocets - 1);
			memset ((unsigned char *) &route->netmask.s_addr +
				(ocets - 1),
				(256 - (1 << (32 - cidr) % 8)), 1);
		}

		/* Finally, snag the router */
		memcpy (&route->gateway.s_addr, q, 4);
		q += 4;

		STAILQ_INSERT_TAIL (routes, route, entries);
	}

	return (routes);
}

void free_dhcp (dhcp_t *dhcp)
{
	if (! dhcp)
		return;

	free_route (dhcp->routes);
	free (dhcp->hostname);
	free_address (dhcp->dnsservers);
	free (dhcp->dnsdomain);
	free (dhcp->dnssearch);
	free_address (dhcp->ntpservers);
	free (dhcp->nisdomain);
	free_address (dhcp->nisservers);
	free (dhcp->rootpath);
	free (dhcp->sipservers);
	if (dhcp->fqdn) {
		free (dhcp->fqdn->name);
		free (dhcp->fqdn);
	}
}

static bool dhcp_add_address (struct address_head **addresses,
			      const unsigned char *data,
			      int length)
{
	int i;
	address_t *address;

	for (i = 0; i < length; i += 4) {
		/* Sanity check */
		if (i + 4 > length) {
			logger (LOG_ERR, "invalid address length");
			return (false);
		}

		if (*addresses == NULL) {
			*addresses = xmalloc (sizeof (**addresses));
			STAILQ_INIT (*addresses);
		}
		address = xzalloc (sizeof (*address));
		memcpy (&address->address.s_addr, data + i, 4);
		STAILQ_INSERT_TAIL (*addresses, address, entries);
	}

	return (true);
}

#ifdef ENABLE_INFO
static char *decode_sipservers (const unsigned char *data, int length)
{
	char *sip = NULL;
	char *p;
	const char encoding = *data++;
	struct in_addr addr;
	size_t len;

	length--;

	switch (encoding) {
		case 0:
			if ((len = decode_search (data, length, NULL)) > 0) {
				sip = xmalloc (len);
				decode_search (data, length, sip);
			}
			break;

		case 1:
			if (length == 0 || length % 4 != 0) {
				logger (LOG_ERR,
					"invalid length %d for option 120",
					length + 1);
				break;
			}
			len = ((length / 4) * (4 * 4)) + 1;
			sip = p = xmalloc (len);
			while (length != 0) {
				memcpy (&addr.s_addr, data, 4);
				data += 4;
				p += snprintf (p, len - (p - sip),
					       "%s ", inet_ntoa (addr));
				length -= 4;
			}
			*--p = '\0';
			break;

		default:
			logger (LOG_ERR, "unknown sip encoding %d", encoding);
			break;
	}

	return (sip);
}
#endif

/* This calculates the netmask that we should use for static routes.
 * This IS different from the calculation used to calculate the netmask
 * for an interface address. */
static uint32_t route_netmask (uint32_t ip_in)
{
	/* used to be unsigned long - check if error */
	uint32_t p = ntohl (ip_in);
	uint32_t t;

	if (IN_CLASSA (p))
		t = ~IN_CLASSA_NET;
	else {
		if (IN_CLASSB (p))
			t = ~IN_CLASSB_NET;
		else {
			if (IN_CLASSC (p))
				t = ~IN_CLASSC_NET;
			else
				t = 0;
		}
	}

	while (t & p)
		t >>= 1;

	return (htonl (~t));
}

static struct route_head *decode_routes (const unsigned char *data, int length)
{
	int i;
	struct route_head *head = NULL;
	route_t *route;
	
	for (i = 0; i < length; i += 8) {
		if (! head) {
			head = xmalloc (sizeof (*head));
			STAILQ_INIT (head);
		}
		route = xzalloc (sizeof (*route));
		memcpy (&route->destination.s_addr, data + i, 4);
		memcpy (&route->gateway.s_addr, data + i + 4, 4);
		route->netmask.s_addr =
			route_netmask (route->destination.s_addr);
		STAILQ_INSERT_TAIL (head, route, entries);
	}

	return (head);
}

static struct route_head *decode_routers (const unsigned char *data, int length)
{
	int i;
	struct route_head *head = NULL;
	route_t *route = NULL;

	for (i = 0; i < length; i += 4) {
		if (! head) {
			head = xmalloc (sizeof (*head));
			STAILQ_INIT (head);
		}
		route = xzalloc (sizeof (*route));
		memcpy (&route->gateway.s_addr, data + i, 4);
		STAILQ_INSERT_TAIL (head, route, entries);
	}

	return (head);
}

int parse_dhcpmessage (dhcp_t *dhcp, const dhcpmessage_t *message)
{
	const unsigned char *p = message->options;
	const unsigned char *end = p; /* Add size later for gcc-3 issue */
	unsigned char option;
	unsigned char length;
	unsigned int len = 0;
	int retval = -1;
	struct timeval tv;
	struct route_head *routers = NULL;
	struct route_head *routes = NULL;
	struct route_head *csr = NULL;
	struct route_head *mscsr = NULL;
	bool in_overload = false;
	bool parse_sname = false;
	bool parse_file = false;

	end += sizeof (message->options);

	if (gettimeofday (&tv, NULL) == -1) {
		logger (LOG_ERR, "gettimeofday: %s", strerror (errno));
		return (-1);
	}

	dhcp->address.s_addr = message->yiaddr;
	dhcp->leasedfrom = tv.tv_sec;
	dhcp->frominfo = false;
	dhcp->address.s_addr = message->yiaddr;
	strlcpy (dhcp->servername, (char *) message->servername,
		 sizeof (dhcp->servername));

#define LEN_ERR \
	{ \
		logger (LOG_ERR, "invalid length %d for option %d", \
			length, option); \
		p += length; \
		continue; \
	}

parse_start:
	while (p < end) {
		option = *p++;
		if (! option)
			continue;

		if (option == DHCP_END)
			goto eexit;

		length = *p++;

		if (option != DHCP_PAD && length == 0) {
			logger (LOG_ERR, "option %d has zero length", option);
			retval = -1;
			goto eexit;
		}

		if (p + length >= end) {
			logger (LOG_ERR, "dhcp option exceeds message length");
			retval = -1;
			goto eexit;
		}

		switch (option) {
			case DHCP_MESSAGETYPE:
				retval = (int) *p;
				p += length;
				continue;

			default:
				if (length == 0) {
					logger (LOG_DEBUG,
						"option %d has zero length, skipping",
						option);
					continue;
				}
		}

#define LENGTH(_length) \
		if (length != _length) \
		LEN_ERR;
#define MIN_LENGTH(_length) \
		if (length < _length) \
		LEN_ERR;
#define MULT_LENGTH(_mult) \
		if (length % _mult != 0) \
		LEN_ERR;
#define GET_UINT8(_val) \
		LENGTH (sizeof (uint8_t)); \
		memcpy (&_val, p, sizeof (uint8_t));
#define GET_UINT16(_val) \
		LENGTH (sizeof (uint16_t)); \
		memcpy (&_val, p, sizeof (uint16_t));
#define GET_UINT32(_val) \
		LENGTH (sizeof (uint32_t)); \
		memcpy (&_val, p, sizeof (uint32_t));
#define GET_UINT16_H(_val) \
		GET_UINT16 (_val); \
		_val = ntohs (_val);
#define GET_UINT32_H(_val) \
		GET_UINT32 (_val); \
		_val = ntohl (_val);

		switch (option) {
			case DHCP_ADDRESS:
				GET_UINT32 (dhcp->address.s_addr);
				break;
			case DHCP_NETMASK:
				GET_UINT32 (dhcp->netmask.s_addr);
				break;
			case DHCP_BROADCAST:
				GET_UINT32 (dhcp->broadcast.s_addr);
				break;
			case DHCP_SERVERIDENTIFIER:
				GET_UINT32 (dhcp->serveraddress.s_addr);
				break;
			case DHCP_LEASETIME:
				GET_UINT32_H (dhcp->leasetime);
				break;
			case DHCP_RENEWALTIME:
				GET_UINT32_H (dhcp->renewaltime);
				break;
			case DHCP_REBINDTIME:
				GET_UINT32_H (dhcp->rebindtime);
				break;
			case DHCP_MTU:
				GET_UINT16_H (dhcp->mtu);
				/* Minimum legal mtu is 68 accoridng to
				 * RFC 2132. In practise it's 576 which is the
				 * minimum maximum message size. */
				if (dhcp->mtu < MTU_MIN) {
					logger (LOG_DEBUG,
						"MTU %d is too low, minimum is %d; ignoring",
						dhcp->mtu, MTU_MIN);
					dhcp->mtu = 0;
				}
				break;

#undef GET_UINT32_H
#undef GET_UINT32
#undef GET_UINT16_H
#undef GET_UINT16
#undef GET_UINT8

#define GETSTR(_var) { \
	MIN_LENGTH (sizeof (char)); \
	if (_var) free (_var); \
	_var = xmalloc ((size_t) length + 1); \
	memcpy (_var, p, (size_t) length); \
	memset (_var + length, 0, 1); \
}
			case DHCP_HOSTNAME:
				GETSTR (dhcp->hostname);
				break;
			case DHCP_DNSDOMAIN:
				GETSTR (dhcp->dnsdomain);
				break;
			case DHCP_MESSAGE:
				GETSTR (dhcp->message);
				break;
#ifdef ENABLE_INFO
			case DHCP_ROOTPATH:
				GETSTR (dhcp->rootpath);
				break;
#endif
#ifdef ENABLE_NIS
			case DHCP_NISDOMAIN:
				GETSTR (dhcp->nisdomain);
				break;
#endif
#undef GETSTR

#define GETADDR(_var) \
				MULT_LENGTH (4); \
				if (! dhcp_add_address (&_var, p, length)) \
				{ \
					retval = -1; \
					goto eexit; \
				}
			case DHCP_DNSSERVER:
				GETADDR (dhcp->dnsservers);
				break;
#ifdef ENABLE_NTP
			case DHCP_NTPSERVER:
				GETADDR (dhcp->ntpservers);
				break;
#endif
#ifdef ENABLE_NIS
			case DHCP_NISSERVER:
				GETADDR (dhcp->nisservers);
				break;
#endif
#undef GETADDR

			case DHCP_DNSSEARCH:
				MIN_LENGTH (1);
				len = decode_search (p, length, NULL);
				if (len > 0) {
					free (dhcp->dnssearch);
					dhcp->dnssearch = xmalloc (len);
					decode_search (p, length,
						       dhcp->dnssearch);
				}
				break;

			case DHCP_CSR:
				MIN_LENGTH (5);
				free_route (csr);
				csr = decode_CSR (p, length);
				break;

			case DHCP_MSCSR:
				MIN_LENGTH (5);
				free_route (mscsr);
				mscsr = decode_CSR (p, length);
				break;

#ifdef ENABLE_INFO
			case DHCP_SIPSERVER:
				free (dhcp->sipservers);
				dhcp->sipservers = decode_sipservers (p,length);
				break;
#endif

			case DHCP_STATICROUTE:
				MULT_LENGTH (8);
				free_route (routes);
				routes = decode_routes (p, length);
				break;

			case DHCP_ROUTERS:
				MULT_LENGTH (4);
				free_route (routers);
				routers = decode_routers (p, length);
				break;

			case DHCP_OPTIONSOVERLOADED:
				LENGTH (1);
				/* The overloaded option in an overloaded option
				 * should be ignored, overwise we may get an
				 * infinite loop */
				if (! in_overload) {
					if (*p & 1)
						parse_file = true;
					if (*p & 2)
						parse_sname = true;
				}
				break;

			case DHCP_FQDN:
				/* We ignore replies about FQDN */
				break;

#undef LENGTH
#undef MIN_LENGTH
#undef MULT_LENGTH

			default:
				logger (LOG_DEBUG,
				       	"no facility to parse DHCP code %u",
					option);
				break;
		}

		p += length;
	}

eexit:
	/* We may have options overloaded, so go back and grab them */
	if (parse_file) {
		parse_file = false;
		p = message->bootfile;
		end = p + sizeof (message->bootfile);
		in_overload = true;
		goto parse_start;
	} else if (parse_sname) {
		parse_sname = false;
		p = message->servername;
		end = p + sizeof (message->servername);
		memset (dhcp->servername, 0, sizeof (dhcp->servername));
		in_overload = true;
		goto parse_start;
	}

	/* Fill in any missing fields */
	if (! dhcp->netmask.s_addr)
		dhcp->netmask.s_addr = get_netmask (dhcp->address.s_addr);
	if (! dhcp->broadcast.s_addr)
		dhcp->broadcast.s_addr = dhcp->address.s_addr |
			~dhcp->netmask.s_addr;

	/* If we have classess static routes then we discard
	 * static routes and routers according to RFC 3442 */
	if (csr) {
		dhcp->routes = csr;
		free_route (mscsr);
		free_route (routers);
		free_route (routes);
	} else if (mscsr) {
		dhcp->routes = mscsr;
		free_route (routers);
		free_route (routes);
	} else {
		/* Ensure that we apply static routes before routers */
		if (! routes)
			routes = routers;
		else if (routers)
			STAILQ_CONCAT (routes, routers);
		dhcp->routes = routes;
	}

	return (retval);
}
