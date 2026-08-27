#ifndef damper_h_included
#define damper_h_included

#include <stdint.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <float.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>

/* IP header */
struct damper_ip_header
{
	uint8_t  ip_vhl;                /* version << 4 | header length >> 2 */
	uint8_t  ip_tos;                /* type of service */
	uint16_t ip_len;                /* total length */
	uint16_t ip_id;                 /* identification */
	uint16_t ip_off;                /* fragment offset field */
	#define IP_RF 0x8000            /* reserved fragment flag */
	#define IP_DF 0x4000            /* dont fragment flag */
	#define IP_MF 0x2000            /* more fragments flag */
	#define IP_OFFMASK 0x1fff       /* mask for fragmenting bits */
	uint8_t  ip_ttl;                /* time to live */
	uint8_t  ip_p;                  /* protocol */
	uint16_t ip_sum;                /* checksum */
	struct  in_addr ip_src,ip_dst;  /* source and dest address */
} __attribute__((packed));

#define DAMPER_MAX_PACKET_SIZE 0xffff

struct mpacket
{
	int id; /* ID assigned to packet by netfilter */
	int size;
	unsigned char packet[DAMPER_MAX_PACKET_SIZE];
};

/* 
 * Need to study how to resolve:
 *  - donper.conf structure (maybe copy tc command)
 *  - Borrowing and priotirty of the children
 *  - Generation of new tokens
 *  - Same struct for parent and children? For now no.
 *  - Queue for each node
 */

/* child node */
struct htb_child {
    uint32_t mark;

    uint64_t rate;         /* min, byte/s */
    uint64_t ceil;         /* max with borrowing, byte/s */

    int64_t tokens;        /* number of token in the bucket */
    int64_t burst;         /* bucket size */
    int64_t ceil_tokens;   /* number of token for borrowing */
    int64_t ceil_burst;    /* burst for borrowing */

    struct timespec old_time;  /* old time to generate new tokens */
    struct timespec ceil_old_time; 

    /* priotirty queue */
    struct mpacket *packets;
    double *prioarray;
    size_t qlen;
};

 /* parent node */
struct htb_parent {
    int queue;                /* nfequeue queue id */
	  struct nfq_q_handle *qh;  /* queue handle */
	  int nfqlen;               /* internal queue length */
    
    uint64_t limit;   /* or rate, bit/s */
    int64_t tokens;   /* token = byte */
    int64_t burst;    /* bucket size */
    
    struct timespec old_time;  /* old time to generate new tokens */

    pthread_t sender_tid;
	  pthread_mutex_t lock;

    int htb;                       /* 1 for htb, 0 for tbf */
    struct htb_child **children;   /* array of the children */
    size_t n_children;             /* number of children node */
};


/* modules */

typedef void * (*module_init_func)    (size_t n);
typedef void   (*module_conf_func)    (void *, char *param1, char *param2);
typedef int    (*module_postconf_func)(void *);
typedef double (*module_weight_func)  (void *, char *packet, int packetlen, int mark);
typedef void   (*module_done_func)    (void *);

struct module_info
{
	char *name;
	module_init_func init;
	module_conf_func conf;
	module_postconf_func postconf;
	module_weight_func weight;
	module_done_func done;

	double k; /* multiplicator */

	void *mptr;
	int enabled;

	FILE *statf;
	double stw;           /* sum of weights per second */
	double nw;            /* number of weight samples per second */
};

extern struct module_info modules[];

#endif

