/*
 * $ cc -Wall -pedantic damper.c modules.conf.c -o damper -lnetfilter_queue -pthread -lrt
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>

#include <linux/netfilter.h>
#include <libnetfilter_queue/libnetfilter_queue.h>

#include "damper.h"

#define BILLION ((uint64_t)1000000000)

#define NFQ_DEFLEN 10000 /* internal queue length */

enum htb_mode
{
	RED = 0,
	GREEN,
	YELLOW
};

/* indicate termination by signal */
volatile sig_atomic_t damper_done = 0;

/* signal handler */
static void
on_term(int signum)
{
	damper_done = 1;
}


/* convert string with optional suffixes 'k', 'm' or 'g' to bytes */
static uint64_t
str2bytes(const char *l)
{
	char unit;
	size_t len;
	int64_t res;
	uint64_t k;
	char lcopy[LINE_MAX];

	strncpy(lcopy, l, LINE_MAX);
	len = strlen(lcopy);
	if (len == 0) {
		return 0;
	}
	unit = tolower(lcopy[len - 1]);
	if (isdigit(unit)) {
		k = 1;
	} else if (unit == 'k') {
		k = 1000;
	} else if (unit == 'm') {
		k = 1000 * 1000;
	} else if (unit == 'g') {
		k = 1000 * 1000 * 1000;
	} else {
		/* unknown suffix */
		k = 0;
	}

	/* drop suffix */
	if (k != 1) {
		lcopy[len-1] = '\0';
	}

	res = strtoll(lcopy, NULL, 10);
	if ((res == INT64_MIN) || (res == INT64_MAX)) {
		k = 0;
	}

	return res * k;
}

static int
config_read(struct htb_parent *parent, char *confname)
{
	FILE *f;
	char line[LINE_MAX];

	f = fopen(confname, "r");
	if (!f) {
		fprintf(stderr, "Can't open file '%s'\n", confname);
		goto fail_open;
	}

	parent->nfqlen = 0;

	while (fgets(line, sizeof(line), f)) {
		char cmd[LINE_MAX], p1[LINE_MAX], p2[LINE_MAX],  p3[LINE_MAX], p4[LINE_MAX],  p5[LINE_MAX], p6[LINE_MAX],  p7[LINE_MAX], p8[LINE_MAX];
		int scanres;

		scanres = sscanf(line, "%s %s %s %s %s %s %s %s %s", cmd, p1, p2, p3, p4, p5, p6, p7, p8);
		/* comment */
    if ((scanres < 2) || (cmd[0] == '#')) {
			continue;
		}
    /* nfequeue number */
		if (!strcmp(cmd, "queue")) {
			parent->queue = atoi(p1);
		} 
    /* parent rate/ceil */
    else if (!strcmp(cmd, "limit")) {
			if (!strcmp(p1, "no")) {
				parent->limit = UINT64_MAX;
			} else {
				parent->limit = str2bytes(p1)/8;
			}
		}
    /* parent burst*/
    else if (!strcmp(cmd, "burst")) {
      parent->burst = str2bytes(p1);
      parent->tokens = str2bytes(p1);
    }
    /* for htb or tbf*/
    else if (!strcmp(cmd, "htb")) {
      if (!strcmp(p1, "yes")) {
        parent->htb = 1;
      } else {
        parent->htb = 0;
      }
    /* new child node */
		} else if (!strcmp(cmd, "class")){
      if (parent->htb) {
        struct htb_child **tmp;

        /* grow the children array for every new class */
        tmp = realloc(parent->children, (parent->n_children + 1) * sizeof(struct htb_child *));
        if (!tmp) {
          fprintf(stderr, "realloc() failed\n");
          goto fail_open;
        }
        parent->children = tmp;

        parent->children[parent->n_children] = malloc(sizeof(struct htb_child));
        if (!parent->children[parent->n_children]) {
          fprintf(stderr, "malloc(%lu) failed\n", (long)sizeof(struct htb_child));
          goto fail_open;
        }

        parent->children[parent->n_children]->mark = atoi(p2);
        parent->children[parent->n_children]->rate = str2bytes(p4)/8;
        parent->children[parent->n_children]->ceil = str2bytes(p6)/8;
        parent->children[parent->n_children]->burst = str2bytes(p8);
        parent->n_children += 1;
      }
		/* module parameters */
    } else {
			size_t i;
			for (i=0; modules[i].name; i++) {
				if (!strcmp(cmd, modules[i].name) && modules[i].conf) {
					if (!strcmp(p1, "k")) {
						/* multiplicator */
						modules[i].k = atof(p2);
					} else {
						(modules[i].conf)(modules[i].mptr, p1, p2);
					}
					break;
				}
			}
		} 
	}
	fclose(f);
	return 1;

fail_open:
	return 0;
}

static struct htb_parent *
tree_init(char *confname)
{
  size_t i, j;
  uint64_t temp_burst;
  struct htb_parent *parent;

  parent = malloc(sizeof(struct htb_parent));
	if (!parent) {
		fprintf(stderr, "malloc(%lu) failed\n", (long)sizeof(struct htb_parent));
		goto fail_parent;
	}

  parent->queue = 0;
  parent->nfqlen = 0;
  parent->limit = 0;
  parent->tokens = 0;
  parent->burst = 0;
  parent->htb = 0;
  parent->n_children = 0;

  clock_gettime(CLOCK_MONOTONIC, &parent->old_time);

  parent->children = malloc(sizeof(struct htb_child *));
	if (!parent->children) {
		fprintf(stderr, "malloc(%lu) failed\n", (long)sizeof(struct htb_child *));
		free(parent);
		return NULL;
	}

  /* init modules */
	for (i=0; modules[i].name; i++) {
		if (modules[i].init) {
			/* default multiplicator */
			modules[i].k = 1.0f;

			modules[i].mptr = (modules[i].init)(i);
		} else {
			modules[i].mptr = NULL;
		}
	}

  /* New config_read() for the the parent node e the N child node */
	if (!config_read(parent, confname)) {
		goto fail_conf;
	}

  if (parent->limit == 0) {
		fprintf(stderr, "Something is wrong with limit, all traffic will be blocked\n");
	}

	if (parent->nfqlen == 0) {
		parent->nfqlen = NFQ_DEFLEN;
	}
  
  /* no class or no htb, then tbf */
  if (parent->n_children == 0) {
    parent->n_children += 1;
    
    /* using one child node for tbf */
    parent->children[0] = malloc(sizeof(struct htb_child));
    if (!parent->children[0]) {
		    fprintf(stderr, "malloc(%lu) failed\n", (long)sizeof(struct htb_child));
		    goto fail_conf;
	  }

    /* using parent value */
    parent->children[0]->rate = parent->limit;
    parent->children[0]->ceil = parent->limit;
    parent->children[0]->burst = parent->burst;
    parent->children[0]->ceil_burst = parent->burst;
  }
  
  /* other params of the children */
  for (i=0; i < parent->n_children; i++) {
    parent->children[i]->tokens = parent->children[i]->burst;
    
    /* cburst mantain the proportion between ceil and rate */
    temp_burst = (uint64_t) ((double) parent->children[i]->burst * parent->children[i]->ceil / parent->children[i]->rate);
    parent->children[i]->ceil_burst = temp_burst;
    parent->children[i]->ceil_tokens = temp_burst;
    
    clock_gettime(CLOCK_MONOTONIC, &parent->children[i]->old_time);
    clock_gettime(CLOCK_MONOTONIC, &parent->children[i]->ceil_old_time);


    /* priority queue len set at 100 by default */
    parent->children[i]->qlen = 100;

	  /* reserve memory for packets in queue */
    parent->children[i]->packets = malloc(parent->children[i]->qlen * sizeof(struct mpacket));
	  if (!parent->children[i]->packets) {
		  fprintf(stderr, "malloc(%lu) failed\n", (long)parent->children[i]->qlen * sizeof(struct mpacket));
		  goto fail_packets;
	  }

    /* create priority array - simplified implementation of priority queue */
	  parent->children[i]->prioarray = malloc(parent->children[i]->qlen  * sizeof(double));
	  if (!parent->children[i]->prioarray) {
		  fprintf(stderr, "malloc(%lu) failed\n", (long)parent->children[i]->qlen  * sizeof(double));
		  goto fail_prio_array;
	  }
	  /* fill priority array with minimal possible values */
	  for (j=0; j < parent->children[i]->qlen; j++) {
		  parent->children[i]->prioarray[j] = DBL_MIN;
	  }
  }

  /* init mutex */
	pthread_mutex_init(&parent->lock, NULL);

	/* configuration done, notify modules */
	for (i=0; modules[i].name; i++) {
		if (modules[i].postconf) {
			int mres;

			mres = (modules[i].postconf)(modules[i].mptr);
			if (!mres) {
				/* disable module */
				fprintf(stderr, "Module '%s': initialization failed, disabling\n", modules[i].name);
				modules[i].enabled = 0;
			} else {
				modules[i].enabled = 1;
			}
		}
	}

  return parent;

fail_prio_array:
  /* delete priority queues */
	free(parent->children[i]->packets);
  for (j=0; j < i; j++){
    free(parent->children[j]->prioarray);
    free(parent->children[j]->packets);
  }
fail_packets:
fail_conf:
	free(parent->children);
fail_child:
  free(parent);
fail_parent:
	return NULL;
}

static void
tree_destroy(struct htb_parent *parent)
{
	size_t i;

	for (i=0; modules[i].name; i++) {
		if (modules[i].done) {
			(modules[i].done)(modules[i].mptr);
		}
		if (modules[i].statf) {
			fclose(modules[i].statf);
		}
	}

	pthread_mutex_destroy(&parent->lock);

  for (i=0; i < parent->n_children; i++) {
	  free(parent->children[i]->prioarray);
    free(parent->children[i]->packets);
  }
	free(parent->children);
  free(parent);
}

/* CHANGE ME */
/* How i want this function to work: TODO
    1) First htb_refill() for the new generated tokens;
    2.1) Send e sub the tokens used;
    2.2) If there aren't enough tokens, borrow from parent is possible (htb_borrow() maybe?), then return to point 2.1;
    2.3) No borrow and no send then go to the next node;
    3) Borrow with prio or round robin (easier for the moment);
    4) How to manage more node borrowign and how to stop borrowing.
 

    Trash that can work:
      1) htb_refill()
      2) htb_sub()
      3) htb_check()
 */


static enum htb_mode
htb_check(struct htb_parent *parent, struct htb_child *child,
	int64_t size)
{
	if ((child->tokens >= size)) {
		return GREEN;
	}

	if ((parent->limit != UINT64_MAX) &&
	    (parent->tokens < size)) {
		return RED;
	}

	if (child->ceil_tokens >= size) {
		return YELLOW;
	}

	return RED;
}

static void
htb_refill(struct timespec *old_time, int64_t *tokens,
	int64_t burst, uint64_t rate)
{
	struct timespec now;
	int64_t ns, add;

	/* bucket always full */
	if (rate == UINT64_MAX) {
		*tokens = burst;
		return;
	}

	/* no token generated */
	if (rate == 0) {
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &now);

  /* time passed in nano second */
	ns = (now.tv_sec - old_time->tv_sec) * (int64_t)BILLION + (now.tv_nsec - old_time->tv_nsec);
	if (ns <= 0) {
		return;
	}

	/* bucket already full */
	if (*tokens >= burst) {
		*old_time = now;
		return;
	}

  /* token generated */
	add = (ns / (int64_t)BILLION) * (int64_t)rate + ((ns % (int64_t)BILLION) * (int64_t)rate) / (int64_t)BILLION;

  /* for token overflow */
	*tokens += add;
	if (*tokens > burst) {
		*tokens = burst;
	}

	*old_time = now;
}

/* search best packet in a child to send */
static ssize_t
search_best_packet(struct htb_child *child)
{
	size_t i;
	double max = DBL_MIN;
	ssize_t idx = -1;

	for (i = 0; i < child->qlen; i++) {
		if (child->prioarray[i] > max) {
			max = child->prioarray[i];
			idx = (ssize_t)i;
		}
	}

	return idx;
}

static void *
sender_thread(void *arg)
{
  struct htb_parent *parent = arg;
	struct timespec ts;
	size_t rr = 0;

	for (;;) {
    if (damper_done) {
      break;
    }

    int sent = 0;
		size_t i;

		pthread_mutex_lock(&parent->lock);

		/* refill parent node */
		htb_refill(&parent->old_time, &parent->tokens,
			parent->burst, parent->limit);

    for (i = 0; i < parent->n_children; i++) {
			size_t node_idx = (rr + i) % parent->n_children;
      ssize_t idx;
			struct htb_child *child = parent->children[node_idx];
			enum htb_mode mode;
			int vres;
			int64_t size;

      /* refill child node */
		  htb_refill(&child->old_time, &child->tokens, child->burst, child->rate);
		  htb_refill(&child->ceil_old_time, &child->ceil_tokens, child->ceil_burst, child->ceil);

			idx = search_best_packet(child);
			if (idx < 0) {
				/* empty */
				continue;
			}

			size = child->packets[idx].size;

			mode = htb_check(parent, child, size);
			if (mode == RED) {
				/* no send */
        continue;
			}

			vres = nfq_set_verdict(parent->qh, child->packets[idx].id, NF_ACCEPT, 0, NULL);
			if (vres < 0) {
				fprintf(stderr, "nfq_set_verdict() failed, %s\n", strerror(errno));
			}

			child->tokens -= size;
			parent->tokens -= size;
			if (mode == YELLOW) {
				child->ceil_tokens -= size;
			}

      child->prioarray[idx] = DBL_MIN;

			rr = (node_idx + 1) % parent->n_children;
			sent = 1;
			break;
		}

		pthread_mutex_unlock(&parent->lock);

		if (!sent) {	
			ts.tv_sec = 0;
			ts.tv_nsec = 1000000; /* 1 ms */
			clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
		}

	}

	return NULL;
}

/* search the node with the class mark equal to the packet iptables mark */
static int
search_node(struct htb_parent *parent, uint32_t mark)
{
  int i;
  
  if (!parent->htb) {
    return 0;
  }

  for (i=0; i<parent->n_children; i++) {
    if (parent->children[i]->mark == mark)
      return i;
  }

  return -1;
}

static void
add_to_queue(struct htb_parent *parent, char *packet, int id,
	int plen, double prio, uint32_t mark)
{
	size_t i, idx = 0;
  int node_idx;
	double min = DBL_MAX;

  /* correct node id */
  node_idx = search_node(parent, mark);
  if (node_idx < 0) {
    nfq_set_verdict(parent->qh, id, NF_DROP, 0, NULL);
    return;
  }

	/* search for packet with minimum priority */
	for (i=0; i<parent->children[node_idx]->qlen; i++) {
		if (min > parent->children[node_idx]->prioarray[i]) {
			min = parent->children[node_idx]->prioarray[i];
			idx = i;
		}
	}

	/* and replace it with new packet */
	if (min < prio) {
		if (min != DBL_MIN) {
			int vres;

			/* drop packet */
			vres = nfq_set_verdict(parent->qh, parent->children[node_idx]->packets[idx].id, NF_DROP, 0, NULL);
			if (vres < 0) {
				fprintf(stderr, "nfq_set_verdict() failed, %s\n", strerror(errno));
			}
		}

    /* add to queue */
		parent->children[node_idx]->prioarray[idx] = prio;
		parent->children[node_idx]->packets[idx].size = plen;
		parent->children[node_idx]->packets[idx].id = id;
		memcpy(parent->children[node_idx]->packets[idx].packet, packet, plen);
	}
  else {
		nfq_set_verdict(parent->qh, id, NF_DROP, 0, NULL);
	}
}


/* drop packet or accept packet based on weight and limit */
static int
on_packet(struct nfq_q_handle *qh,
		struct nfgenmsg *nfmsg,
		struct nfq_data *nfad, void *data)
{
	int plen, id;
	char *p;
	uint32_t mark;
	struct htb_parent *parent;
	double weight = DBL_EPSILON;
	size_t i;

	struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr(nfad);
	if (ph) {
		id = ntohl(ph->packet_id);
	} else {
		return -1;
	}

	if ((plen = nfq_get_payload(nfad, (unsigned char **)&p)) < 0) {
		return -1;
	}

	parent = data;

	/* limit == 0 (traffic disabled) */
	pthread_mutex_lock(&parent->lock);
	if (parent->limit == 0) {
		/* drop packet */
		nfq_set_verdict(parent->qh, id, NF_DROP, 0, NULL);
	  pthread_mutex_unlock(&parent->lock);
    return 1;
  }
	pthread_mutex_unlock(&parent->lock);

	mark = nfq_get_nfmark(nfad);

	/* calculate weight for each enabled module */
	for (i=0; modules[i].name; i++) {
		if (modules[i].weight && modules[i].enabled) {
			double mweight = (modules[i].weight)(modules[i].mptr, p, plen, mark);

			if (mweight < 0.0) {
				weight = mweight;
				break;
			}

			mweight *= modules[i].k;	
			weight += mweight;
		}
	}

	pthread_mutex_lock(&parent->lock);
	if (weight < 0) {
		/* drop packet with with negative weight */
		nfq_set_verdict(parent->qh, id, NF_DROP, 0, NULL);
	} else {
		/* add to queue with positive weight */
		add_to_queue(parent, p, id, plen, weight, mark);
	}
	pthread_mutex_unlock(&parent->lock);

	return 1;
}

/* CHANGE ME */
int
main(int argc, char *argv[])
{
	struct nfq_handle *h;
	int fd;
	char buf[0xffff];
	int r = EXIT_FAILURE;
	struct htb_parent *parent;
	struct sigaction action;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s config.cfg\n", argv[0]);
		return EXIT_FAILURE;
	}

	parent = tree_init(argv[1]);
  if (!parent) {
    return EXIT_FAILURE;
  }

	h = nfq_open();
	if (!h) {
		fprintf(stderr, "nfq_open() failed\n");
		return EXIT_FAILURE;
	}

	if (nfq_unbind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "nfq_unbind_pf() failed\n");
		goto fail_unbind;
	}

	if (nfq_bind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "nfq_bind_pf() failed\n");
		goto fail_bind;
	}

	parent->qh = nfq_create_queue(h, parent->queue, &on_packet, parent);
	if (!parent->qh) {
		fprintf(stderr, "nfq_create_queue() with queue %d failed\n", parent->queue);
		goto fail_queue;
	}

	if (nfq_set_mode(parent->qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
		fprintf(stderr, "nfq_set_mode() failed\n");
		goto fail_mode;
	}

	if (nfq_set_queue_maxlen(parent->qh, parent->nfqlen) < 0) {
		fprintf(stderr, "nfq_set_queue_maxlen() failed with qlen=%d\n", parent->nfqlen);
		goto fail_mode;
	}

	/* handle term and int signals */
	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = on_term;
	sigaction(SIGTERM, &action, NULL);

	action.sa_handler = on_term;
	sigaction(SIGINT, &action, NULL);

	/* create sending thread */
	pthread_create(&parent->sender_tid, NULL, &sender_thread, parent);

	fd = nfq_fd(h);
	for (;;) {
		int rv;

		rv = recv(fd, buf, sizeof(buf), 0);
		if ((rv < 0) && (errno != EINTR)) {
			fprintf(stderr, "recv() on queue returned %d (%s)\n", rv, strerror(errno));
			fprintf(stderr, "Queue full? Current queue size %d, you can increase 'nfqlen' parameter in damper.conf\n",
				parent->nfqlen);
			continue; /* don't stop after error */
		}

		if (damper_done) {
			break;
		}

		nfq_handle_packet(h, buf, rv);
	}

	r = EXIT_SUCCESS;

fail_mode:
	nfq_destroy_queue(parent->qh);

fail_queue:
fail_bind:
fail_unbind:
	nfq_close(h);
  
	tree_destroy(parent);

	return r;
}
