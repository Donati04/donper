#define TCP_PROTO_NUM 6
#define UDP_PROTO_NUM 17


struct entflow
{
	uint32_t saddr, daddr;
	uint16_t proto;
	uint16_t sport, dport;
	uint32_t stream_len;
	uint32_t map[256]; /* symbols map */
} __attribute__((packed));

struct entropy
{
	size_t module_number;

	struct entflow *recent_flows;
	int nflows;
	int currflow;     /* pointer to current entry in flows circular buffer */
	int sport, dport; /* override packet values with values from config */

	int debug;
	pthread_t debug_tid;
	pthread_mutex_t lock;

};

/* Shannon's entropy calculation */
static double
entropy_calc(struct entflow *e)
{
	double m;
	int i;

	m = 0.0f;

	/* we can get (artificial) packet with empty payload */
	if (e->stream_len == 0) {
		return m;
	}

	for (i=0; i<256; i++) {
		double freq;

		if (e->map[i] == 0) continue;
		freq = (double)e->map[i] / e->stream_len;
		m += freq * log2(freq);
	}
	m = -m;

	if (m > DBL_EPSILON) {
		m = 1.0f / m;
	}

	return m;
}

void *
entropy_init(size_t n)
{
	struct entropy *data;

	data = malloc(sizeof(struct entropy));
	if (!data) {
		fprintf(stderr, "Module %s: malloc(%lu) failed\n",
			modules[n].name,
			(long)sizeof(struct entropy));
		goto fail_alloc;
	}

	data->nflows = 0;
	data->currflow = 0;

	data->debug = 0;
	data->module_number = n;
	data->sport = data->dport = -1;
	pthread_mutex_init(&data->lock, NULL);

	return data;

fail_alloc:
	return NULL;
}

void
entropy_conf(void *arg, char *param1, char *param2)
{
	struct entropy *data = arg;

	if (!strcmp(param1, "nrecent")) {
		data->nflows = atoi(param2);
	} else if (!strcmp(param1, "sport")) {
		data->sport = atoi(param2);
	} else if (!strcmp(param1, "dport")) {
		data->dport = atoi(param2);
	} else {
		fprintf(stderr, "Module %s: unknown config parameter '%s'\n",
			modules[data->module_number].name, param1);
	}
}

int
entropy_postconf(void *arg)
{
	struct entropy *data = arg;

	if (data->nflows < 1) {
		fprintf(stderr, "Module %s: incorrect value %d for number of recent flows\n",
			modules[data->module_number].name, data->nflows);
		goto fail;
	}

	/* create array of recent flows */
	data->recent_flows = malloc(data->nflows * sizeof(struct entflow));
	if (!data->recent_flows) {
		fprintf(stderr, "Module %s: malloc() failed for %d recent flows\n",
			modules[data->module_number].name, data->nflows);
		goto fail;
	}
	memset(data->recent_flows, 0, data->nflows * sizeof(struct entflow));

	return 1;

fail:
	return 0;
}

void
entropy_free(void *arg)
{
	struct entropy *data = arg;

	free(data->recent_flows);
	free(data);
}

double
entropy_weight(void *arg, char *packet, int packetlen, int mark)
{
	double m;
	unsigned int i;
	uint32_t saddr, daddr;
	int proto, sport, dport;
	struct damper_ip_header *ip;
	int ip_hdrlen;
	char *payload;
	int found = 0;
	struct entropy *data = arg;

	ip = (struct damper_ip_header *)packet;
	saddr = ip->ip_src.s_addr;
	daddr = ip->ip_dst.s_addr;
	proto = ip->ip_p;

	ip_hdrlen = (ip->ip_vhl & 0x0f) * 4;
	if ((proto == TCP_PROTO_NUM) || (proto == UDP_PROTO_NUM)) {
		char *tcp_udp;

		tcp_udp = packet + ip_hdrlen;
		sport = (data->sport == -1) ? ntohs(*((uint16_t *)(tcp_udp)))
			: data->sport;
		dport = (data->dport == -1) ? ntohs(*((uint16_t *)(tcp_udp + sizeof(uint16_t))))
			: data->dport;

		if (proto == TCP_PROTO_NUM) {
			payload = tcp_udp + 20; /* incorrect, payload may include tcp options */
		} else {
			payload = tcp_udp + 8;
		}
	} else {
		sport = 0;
		dport = 0;
		payload = packet + ip_hdrlen;
	}

	for (i=0; i<data->nflows; i++) {
		if ((saddr == data->recent_flows[i].saddr) && (daddr == data->recent_flows[i].daddr)
			&& (proto == data->recent_flows[i].proto)
			&& (sport == data->recent_flows[i].sport) && (dport == data->recent_flows[i].dport)) {

			data->recent_flows[i].stream_len += packetlen - (payload - packet);
			found = 1;
			break;
		}
	}

	if (!found) {
		i = data->currflow;

		data->recent_flows[i].saddr = saddr;
		data->recent_flows[i].daddr = daddr;
		data->recent_flows[i].proto = proto;
		data->recent_flows[i].sport = sport;
		data->recent_flows[i].dport = dport;
		memset(data->recent_flows[i].map, 0, sizeof(uint32_t) * 256);
		data->recent_flows[i].stream_len = packetlen - (payload - packet);

		data->currflow++;
		if (data->currflow >= data->nflows) {
			data->currflow = 0;
		}
	}

	/* update symbols map */
	while ((payload - packet) < packetlen) {
		data->recent_flows[i].map[(unsigned char)(*payload)]++;
		payload++;
	}

	/* and calculate entropy */
	m = entropy_calc(&data->recent_flows[i]);

	return m;
}


