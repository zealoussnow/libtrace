/* Show the top 'n' flows from a libtrace source
 *
 */
#define __STDC_FORMAT_MACROS 1
#include "libtrace.h"
#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <map>
#include <queue>
#include <inttypes.h>
#include <ncurses.h>
#include <sys/socket.h>
#include <netdb.h>
#include "config.h"
#ifdef HAVE_NETPACKET_PACKET_H
#include <sys/socket.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#endif

typedef enum { BITS_PER_SEC, BYTES } display_t;
display_t display_as = BYTES;

int cmp_sockaddr_in6(const struct sockaddr_in6 *a, const struct sockaddr_in6 *b)
{
	if (a->sin6_port != b->sin6_port)
		return a->sin6_port - b->sin6_port;
	return memcmp(a->sin6_addr.s6_addr,b->sin6_addr.s6_addr,sizeof(a->sin6_addr.s6_addr));
}

int cmp_sockaddr_in(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	if (a->sin_port != b->sin_port)
		return a->sin_port - b->sin_port;
	return a->sin_addr.s_addr - b->sin_addr.s_addr;
}

#ifdef HAVE_NETPACKET_PACKET_H
int cmp_sockaddr_ll(const struct sockaddr_ll *a, const struct sockaddr_ll *b)
{
	return memcmp(a->sll_addr, b->sll_addr, b->sll_halen);
}
#endif

int cmp_sockaddr(const struct sockaddr *a, const struct sockaddr *b)
{
	if (a->sa_family != b->sa_family) {
		return a->sa_family - b->sa_family;
	}
	switch (a->sa_family) {
		case AF_INET:
			return cmp_sockaddr_in((struct sockaddr_in *)a,(struct sockaddr_in*)b);
		case AF_INET6:
			return cmp_sockaddr_in6((struct sockaddr_in6 *)a,(struct sockaddr_in6*)b);
#ifdef HAVE_NETPACKET_PACKET_H
		case AF_PACKET:
			return cmp_sockaddr_ll((struct sockaddr_ll *)a,(struct sockaddr_ll*)b);
#endif
		case AF_UNSPEC:
			return 0; /* Can't compare UNSPEC's! */
		default:
			fprintf(stderr,"Don't know how to compare family %d\n",a->sa_family);
			abort();
	}
}

char *trace_sockaddr2string(const struct sockaddr *a, socklen_t salen, char *buffer, size_t bufflen)
{
	static char intbuffer[NI_MAXHOST];
	char *mybuf = buffer ? buffer : intbuffer;
	size_t mybufflen = buffer ? bufflen : sizeof(intbuffer);
	int err;
	switch (a->sa_family) {
		case AF_INET:
		case AF_INET6:
			if ((err=getnameinfo(a, salen, mybuf, mybufflen, NULL, 0, NI_NUMERICHOST))!=0) {
				strncpy(mybuf,gai_strerror(err),mybufflen);
			}
			break;
#ifdef HAVE_NETPACKET_PACKET_H
		case AF_PACKET:
			trace_ether_ntoa(((struct sockaddr_ll*)a)->sll_addr, mybuf);
			break;
#endif
		default:
			snprintf(mybuf,mybufflen,"Unknown family %d",a->sa_family);
	}
	return mybuf;
}

struct flowkey_t {
	struct sockaddr_storage sip;
	struct sockaddr_storage dip;
	uint16_t sport;
	uint16_t dport;
	uint8_t protocol;

	bool operator <(const flowkey_t &b) const {
		int c;

		c = cmp_sockaddr((struct sockaddr*)&sip,(struct sockaddr*)&b.sip);
		if (c != 0) return c<0;
		c = cmp_sockaddr((struct sockaddr*)&dip,(struct sockaddr*)&b.dip);
		if (c != 0) return c<0;

		if (sport != b.sport) return sport < b.sport;
		if (dport != b.dport) return dport < b.dport;

		return protocol < b.protocol;
	}
};

struct flowdata_t {
	uint64_t packets;
	uint64_t bytes;
};

typedef std::map<flowkey_t,flowdata_t> flows_t;

flows_t flows;

static void per_packet(libtrace_packet_t *packet)
{
	flowkey_t flowkey;
	flows_t::iterator it;

	if (trace_get_source_address(packet,(struct sockaddr*)&flowkey.sip)==NULL)
		flowkey.sip.ss_family = AF_UNSPEC;
	if (trace_get_destination_address(packet,(struct sockaddr*)&flowkey.dip)==NULL)
		flowkey.dip.ss_family = AF_UNSPEC;
	if (trace_get_transport(packet,&flowkey.protocol, NULL) == NULL)
		flowkey.protocol = NULL;
	flowkey.sport = trace_get_source_port(packet);
	flowkey.dport = trace_get_destination_port(packet);

	it = flows.find(flowkey);
	if (it == flows.end()) {
		flowdata_t flowdata = { 0, 0 };
		flows_t::value_type insdata(flowkey,flowdata);
		std::pair<flows_t::iterator,bool> ins= flows.insert(insdata);
		it = ins.first;
	}

	++it->second.packets;
	it->second.bytes+=trace_get_capture_length(packet);

}

struct flow_data_t {
	uint64_t bytes;
	uint64_t packets;
	struct sockaddr_storage sip;
	struct sockaddr_storage dip;
	uint16_t sport;
	uint16_t dport;
	uint8_t protocol;

	bool operator< (const flow_data_t &b) const {
		if (bytes != b.bytes) return bytes < b.bytes;
		return packets < b.packets;
	}
};

static void do_report()
{
	typedef  std::priority_queue<flow_data_t> pq_t;
	int row,col;
	pq_t pq;
	for(flows_t::const_iterator it=flows.begin();it!=flows.end();++it) {
		flow_data_t data;
		data.bytes = it->second.bytes,
		data.packets = it->second.packets,
		data.sip = it->first.sip;
		data.dip = it->first.dip;
		data.sport = it->first.sport;
		data.dport = it->first.dport;
		data.protocol = it->first.protocol;
		pq.push(data);
	}
	getmaxyx(stdscr,row,col);
	attrset(A_REVERSE);
	mvprintw(0,0,"%15s:%s\t%15s:%s\tproto\tbytes\tpackets\n",
		"sip","sport",
		"dip","dport"
		);
	attrset(A_NORMAL);
	char sipstr[1024];
	char dipstr[1024];
	for(int i=0; i<row-2 && !pq.empty(); ++i) {
		mvprintw(i+1,0,"%s/%d\t%s/%d\t%d\t%"PRIu64"\t%"PRIu64"\n",
				trace_sockaddr2string((struct sockaddr*)&pq.top().sip,
					sizeof(struct sockaddr_storage),
					sipstr,sizeof(sipstr)), 
				pq.top().sport,
				trace_sockaddr2string((struct sockaddr*)&pq.top().dip,
					sizeof(struct sockaddr_storage),
					dipstr,sizeof(dipstr)), 
				pq.top().dport,
				pq.top().protocol,
				pq.top().bytes,
				pq.top().packets);
		pq.pop();
	}
	flows.clear();

	clrtobot();
	refresh();
}

static void usage(char *argv0)
{
	fprintf(stderr,"usage: %s [ --filter | -f bpfexp ]  [ --snaplen | -s snap ]\n\t\t[ --promisc | -p flag] [ --help | -h ] [ --libtrace-help | -H ] libtraceuri...\n",argv0);
}

int main(int argc, char *argv[])
{
	libtrace_t *trace;
	libtrace_packet_t *packet;
	libtrace_filter_t *filter=NULL;
	int snaplen=-1;
	int promisc=-1;
	double last_report=0;

	while(1) {
		int option_index;
		struct option long_options[] = {
			{ "filter",		1, 0, 'f' },
			{ "snaplen",		1, 0, 's' },
			{ "promisc",		1, 0, 'p' },
			{ "help",		0, 0, 'h' },
			{ "libtrace-help",	0, 0, 'H' },
			{ NULL,			0, 0, 0 }
		};

		int c= getopt_long(argc, argv, "f:s:p:hH",
				long_options, &option_index);

		if (c==-1)
			break;

		switch (c) {
			case 'f':
				filter=trace_create_filter(optarg);
				break;
			case 's':
				snaplen=atoi(optarg);
				break;
			case 'p':
				promisc=atoi(optarg);
				break;
			case 'H':
				trace_help();
				return 1;
			default:
				fprintf(stderr,"Unknown option: %c\n",c);
				/* FALL THRU */
			case 'h':
				usage(argv[0]);
				return 1;
		}
	}

	if (optind>=argc) {
		fprintf(stderr,"Missing input uri\n");
		usage(argv[0]);
		return 1;
	}

	initscr(); cbreak(); noecho();

	while (optind<argc) {
		trace = trace_create(argv[optind]);
		++optind;

		if (trace_is_err(trace)) {
			endwin();
			trace_perror(trace,"Opening trace file");
			return 1;
		}

		if (snaplen>0)
			if (trace_config(trace,TRACE_OPTION_SNAPLEN,&snaplen)) {
				trace_perror(trace,"ignoring: ");
			}
		if (filter)
			if (trace_config(trace,TRACE_OPTION_FILTER,filter)) {
				trace_perror(trace,"ignoring: ");
			}
		if (promisc!=-1) {
			if (trace_config(trace,TRACE_OPTION_PROMISC,&promisc)) {
				trace_perror(trace,"ignoring: ");
			}
		}

		if (trace_start(trace)) {
			endwin();
			trace_perror(trace,"Starting trace");
			trace_destroy(trace);
			return 1;
		}

		packet = trace_create_packet();

		while (trace_read_packet(trace,packet)>0) {
			if (trace_get_seconds(packet) - last_report > 1) {
				do_report();
					
				last_report=trace_get_seconds(packet);
			}
			per_packet(packet);
		}

		trace_destroy_packet(packet);

		if (trace_is_err(trace)) {
			trace_perror(trace,"Reading packets");
		}

		trace_destroy(trace);
	}

	endwin();

	return 0;
}
