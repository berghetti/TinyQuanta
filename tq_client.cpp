#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_udp.h>
#include <iostream>
#include <time.h>

#define RX_RING_SIZE 128
#define TX_RING_SIZE 128

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32
#define MAX_CORES 64
#define UDP_MAX_PAYLOAD 1472
#define MAX_SAMPLES (20*1000*1000)

#define FULL_MASK 0xFFFFFFFF
#define EMPTY_MASK 0x0

#ifndef BASE_CPU
#define BASE_CPU 0
#endif

/* offload checksum calculations */
// static const struct rte_eth_conf port_conf_default = {
// 	.rxmode = {
// 		.offloads = DEV_RX_OFFLOAD_IPV4_CKSUM,
// 	},
// 	.txmode = {
// 		.offloads = DEV_TX_OFFLOAD_IPV4_CKSUM | DEV_TX_OFFLOAD_UDP_CKSUM,
// 	},
// };

enum {
	MODE_UDP_CLIENT = 0,
	MODE_UDP_SERVER,
};

// job type
typedef enum job_type {
    ROCKSDB_GET,
    ROCKSDB_PUT,
    ROCKSDB_SCAN,
    NUM_JOB_TYPES
} job_type_t;

#define MAKE_IP_ADDR(a, b, c, d)			\
	(((uint32_t) a << 24) | ((uint32_t) b << 16) |	\
	 ((uint32_t) c << 8) | (uint32_t) d)

static unsigned int dpdk_port = 1;
static uint8_t mode;
struct rte_mempool *rx_mbuf_pool;
struct rte_mempool *tx_mbuf_pool;
static struct rte_ether_addr my_eth;
static uint32_t my_ip;
static uint32_t server_ip;
struct rte_ether_addr zero_mac = {
		.addr_bytes = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0}
};
struct rte_ether_addr broadcast_mac = {
		.addr_bytes = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
};
struct rte_ether_addr static_server_eth;
static uint64_t snd_times[MAX_SAMPLES] = {0} ;
static uint64_t rcv_times[MAX_SAMPLES] = {0} ;

/* parameters */
static int seconds = 10/*20*/;
static size_t payload_len = 22; /* total packet size of 64 bytes */
static unsigned int client_port = 50000;
static unsigned int server_port = 8001;
static unsigned int num_queues = 1;

static bool stop_exp = false;
static uint64_t reqs = 0;
static uint64_t start_time, end_time;

/* dpdk_echo.c: simple application to echo packets using DPDK */

static int str_to_ip(const char *str, uint32_t *addr)
{
	uint8_t a, b, c, d;
	if(sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) {
		return -EINVAL;
	}

	*addr = MAKE_IP_ADDR(a, b, c, d);
	return 0;
}

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool, unsigned int n_queues)
{
	//struct rte_eth_conf port_conf = port_conf_default;
	struct rte_eth_conf port_conf = {};
	port_conf.rxmode.offloads = DEV_RX_OFFLOAD_IPV4_CKSUM;
	port_conf.txmode.offloads = DEV_TX_OFFLOAD_IPV4_CKSUM | DEV_TX_OFFLOAD_UDP_CKSUM;

	const uint16_t rx_rings = n_queues, tx_rings = n_queues;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;

	printf("initializing with %u queues\n", n_queues);

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                                        rte_eth_dev_socket_id(port), NULL,
                                        mbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Enable TX offloading */
	rte_eth_dev_info_get(0, &dev_info);
	txconf = &dev_info.default_txconf;

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
                                        rte_eth_dev_socket_id(port), txconf);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	rte_eth_macaddr_get(port, &my_eth);
	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			(unsigned)port,
			my_eth.addr_bytes[0], my_eth.addr_bytes[1],
			my_eth.addr_bytes[2], my_eth.addr_bytes[3],
			my_eth.addr_bytes[4], my_eth.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	rte_eth_promiscuous_enable(port);

	return 0;
}

/*
 * Validate this ethernet header. Return true if this packet is for higher
 * layers, false otherwise.
 */
static bool check_eth_hdr(struct rte_mbuf *buf)
{
	struct rte_ether_hdr *ptr_mac_hdr;

	ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
	if (!rte_is_same_ether_addr(&ptr_mac_hdr->dst_addr, &my_eth)) {
		/* packet not to our ethernet addr */
		return false;
	}

	if (ptr_mac_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
		/* packet not IPv4 */
		return false;

	return true;
}

/*
 * Return true if this IP packet is to us and contains a UDP packet,
 * false otherwise.
 */
static bool check_ip_hdr(struct rte_mbuf *buf)
{
	struct rte_ipv4_hdr *ipv4_hdr;

	ipv4_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_ipv4_hdr *,
			RTE_ETHER_HDR_LEN);
	if (ipv4_hdr->dst_addr != rte_cpu_to_be_32(my_ip) || ipv4_hdr->next_proto_id != IPPROTO_UDP)
		return false;

	return true;
}

/* generate a TX packet according to seq_num, jtype and key_val */

static void populate_packet(struct rte_mbuf *buf, uint32_t seq_num, uint16_t jtype, int key_val) 
{
	char *buf_ptr;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *rte_udp_hdr;

	/* ethernet header */
	buf_ptr = rte_pktmbuf_append(buf, RTE_ETHER_HDR_LEN);
	eth_hdr = (struct rte_ether_hdr *) buf_ptr;

	rte_ether_addr_copy(&my_eth, &eth_hdr->src_addr);
	rte_ether_addr_copy(&static_server_eth, &eth_hdr->dst_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	/* IPv4 header */
	buf_ptr = rte_pktmbuf_append(buf, sizeof(struct rte_ipv4_hdr));
	ipv4_hdr = (struct rte_ipv4_hdr *) buf_ptr;
	ipv4_hdr->version_ihl = 0x45;
	ipv4_hdr->type_of_service = 0;
	ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + payload_len);
	ipv4_hdr->packet_id = 0;
	ipv4_hdr->fragment_offset = 0;
	ipv4_hdr->time_to_live = 64;
	ipv4_hdr->next_proto_id = IPPROTO_UDP;
	ipv4_hdr->hdr_checksum = 0;
	ipv4_hdr->src_addr = rte_cpu_to_be_32(my_ip);
	ipv4_hdr->dst_addr = rte_cpu_to_be_32(server_ip);

	/* UDP header + fake data */
	buf_ptr = rte_pktmbuf_append(buf, sizeof(struct rte_udp_hdr) + payload_len);
	rte_udp_hdr = (struct rte_udp_hdr *) buf_ptr;
	rte_udp_hdr->src_port = rte_cpu_to_be_16(client_port);
	rte_udp_hdr->dst_port = rte_cpu_to_be_16(server_port);
	rte_udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + payload_len);
	rte_udp_hdr->dgram_cksum = 0;

	/* sequence number */
	*(uint32_t*)((char*)buf_ptr + sizeof(struct rte_udp_hdr)) = seq_num;
	/* Job type */
	*(uint16_t*)((char*)buf_ptr + sizeof(struct rte_udp_hdr) + sizeof(uint32_t)) = jtype;
	/* Key */
	char key[10];
	snprintf(key, 10, "key%d", key_val);
	rte_memcpy(buf_ptr + sizeof(struct rte_udp_hdr) + sizeof(uint32_t) + sizeof(uint16_t), key, 10);
	/* data filler */
	memset(buf_ptr + sizeof(struct rte_udp_hdr) + sizeof(uint32_t) + sizeof(uint16_t) + 10, 0xAB, payload_len - sizeof(uint32_t) - sizeof(uint16_t) - 10);

	// memset(buf_ptr + sizeof(struct rte_udp_hdr), 0xAB, payload_len);

	buf->l2_len = RTE_ETHER_HDR_LEN;
	buf->l3_len = sizeof(struct rte_ipv4_hdr);
	buf->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4;
}

static void pin_to_cpu(int cpu_id) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
	cpu_id = cpu_id + BASE_CPU;
        CPU_SET(cpu_id, &cpuset);
        pthread_t thread = pthread_self();
        int ret = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
        if(ret == 0)
            std::cout << "Successfuly pin the current thread to cpu " << cpu_id << std::endl;
}

// implement for us
static void rte_delay_ns_block(unsigned int ns)
{
	const uint64_t start = rte_get_timer_cycles();
	const uint64_t ticks = (uint64_t)ns * rte_get_timer_hz() / 1E9;
	while ((rte_get_timer_cycles() - start) < ticks)
		rte_pause();
}

static void* client_rx(void* arg)
{
	rte_thread_register();
	pin_to_cpu(1);

	struct rte_mbuf *bufs[BURST_SIZE];
	struct rte_mbuf *buf;
	uint32_t i, nb_rx;
	uint8_t port = dpdk_port;
	struct rte_udp_hdr *rte_udp_hdr;
	uint32_t seq_num;
	uint64_t time_received;

	nb_rx = 0;
	while (!stop_exp) {
		nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
		time_received = rte_get_timer_cycles();
		if (nb_rx == 0)
			continue;

		for (i = 0; i < nb_rx; i++) {
			buf = bufs[i];

			if (!check_eth_hdr(buf))
				goto no_match;

			/* this packet is IPv4, check IP header */
			if (!check_ip_hdr(buf))
				goto no_match;

			/* check UDP header */
			rte_udp_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_udp_hdr *, RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr));
			if (rte_udp_hdr->src_port != rte_cpu_to_be_16(server_port) || rte_udp_hdr->dst_port != rte_cpu_to_be_16(client_port))
				goto no_match;

			seq_num = *(rte_pktmbuf_mtod_offset(buf, uint32_t *, RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr)));

			// std::cout << seq_num << std::endl;
			/* packet matches */
			rte_pktmbuf_free(buf);
			assert(rcv_times[seq_num] == 0);
			rcv_times[seq_num] = time_received;
			continue;

			no_match:
			{
				/* packet isn't what we're looking for, free it and rx again */
				rte_pktmbuf_free(buf);
				std::cout << "No match!" << std::endl;
			}

		}
	}
}

static uint64_t rdtsc_w_lfence(){
    unsigned int lo,hi;
    __asm__ __volatile__ ("lfence\n\t" "rdtsc": "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Run an echo client
 */
static void run_client()
{
	pin_to_cpu(2);

	struct rte_mbuf *bufs[BURST_SIZE];
	struct rte_mbuf *buf;
	struct rte_ether_hdr *ptr_mac_hdr;
	uint32_t nb_tx, i;
	struct rte_ether_addr server_eth;
	char mac_buf[64];
	uint8_t port = dpdk_port;

	uint32_t sleep_ns = 550/*2*//*8*/;
	uint32_t seq_num = 0;
	uint16_t jtype = ROCKSDB_GET;
	int key_val = 0;
	uint64_t packet_start_time, packet_end_time, total_packet_cycles = 0, num_packets_sent = 0;
	bool first_hundred_packets = true;

	/* Verify that we have enough space for all the datapoints, assuming
	   an RTT of at least 4 us */
	uint32_t samples = seconds / ((float) sleep_ns / (1000 * 1000 * 1000));
	if (samples > MAX_SAMPLES)
		rte_exit(EXIT_FAILURE, "Too many samples: %d\n", samples);

	printf("\nCore %u running in client mode. [Ctrl+C to quit]\n",
			rte_lcore_id());

	rte_ether_format_addr(&mac_buf[0], 64, &static_server_eth);
	printf("Using static server MAC addr: %s\n", &mac_buf[0]);

	/* run for specified amount of time */
	start_time = rte_get_timer_cycles();
	while (rte_get_timer_cycles() < start_time + seconds * rte_get_timer_hz()) {

		if(first_hundred_packets)
			rte_delay_us_block(1000);
		rte_delay_ns_block(sleep_ns);
		packet_start_time = rdtsc_w_lfence();

		buf = rte_pktmbuf_alloc(tx_mbuf_pool);
		if (buf == NULL)
			printf("error allocating tx mbuf\n");

		populate_packet(buf, seq_num, jtype, key_val);

		packet_end_time = rdtsc_w_lfence();

		nb_tx = rte_eth_tx_burst(port, 0, &buf, 1);

		/* send packet */
		snd_times[seq_num++] = rte_get_timer_cycles();
		key_val = rand() % 5000;
		if(seq_num > 100)
			first_hundred_packets = false;
		
		if (unlikely(nb_tx != 1)) {
			printf("error: could not send packet\n");
		}

		// packet_end_time = rdtsc_w_lfence();
		total_packet_cycles += packet_end_time - packet_start_time;
		num_packets_sent ++;
	}
	end_time = rte_get_timer_cycles();
	rte_delay_us_block(1000);
	stop_exp = true;
	reqs = num_packets_sent;
	std::cout << "Sending a packet takes: " << (float) total_packet_cycles * 1000 * 1000 / (num_packets_sent * rte_get_timer_hz())  << " us" << std::endl;
}

/*
 * Initialize dpdk.
 */
static int dpdk_init(int argc, char *argv[])
{
	int args_parsed;

	/* Initialize the Environment Abstraction Layer (EAL). */
	args_parsed = rte_eal_init(argc, argv);
	if (args_parsed < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	/* Check that there is a port to send/receive on. */
	if (!rte_eth_dev_is_valid_port(dpdk_port))
		rte_exit(EXIT_FAILURE, "Error: port is not available\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	rx_mbuf_pool = rte_pktmbuf_pool_create("MBUF_RX_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (rx_mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create rx mbuf pool\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	tx_mbuf_pool = rte_pktmbuf_pool_create("MBUF_TX_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (tx_mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create tx mbuf pool\n");

	return args_parsed;
}

static int parse_args(int argc, char *argv[])
{
	/* argv[0] is still the program name */
	if (argc != 4 ) {
		printf("invalid number of arguments: %d\n", argc);
		return -EINVAL;
	}
	str_to_ip(argv[1], &my_ip);
	str_to_ip(argv[2], &server_ip);
	rte_ether_unformat_addr(argv[3], &static_server_eth);
	return 0;
}

static void process_data()
{
	uint64_t total_cycles = 0;
	uint64_t included_samples = 0;
	uint64_t i;
	uint64_t num_response = 0; 
	for (i = reqs * 0.1; i < reqs * 0.9; i++) {
		if(rcv_times[i] > 0) {
			total_cycles += rcv_times[i] - snd_times[i];
			included_samples++;
		}
	}
	for (i = 0; i < reqs; i ++ ) {
		if(rcv_times[i] > 0) {
			num_response++;
		}
	}

	// printf("ran for %f seconds, completed %"PRIu64" echos\n",
	// 		(float) (end_time - start_time) / rte_get_timer_hz(), reqs);
	printf("client reqs/s: %f\n",
			(float) (reqs * rte_get_timer_hz()) / (end_time - start_time));
	printf("server response/s: %f\n",
			(float) (num_response * rte_get_timer_hz()) / (end_time - start_time));
	if (included_samples > 0)
	  printf("mean latency (us): %f\n", (float) total_cycles * 1000 * 1000 / (included_samples * rte_get_timer_hz()));
}

/*
 * The main function, which does initialization and starts the client or server.
 */
int
main(int argc, char *argv[])
{
	int args_parsed, res;

	/* Initialize dpdk. */
	args_parsed = dpdk_init(argc, argv);

	/* initialize our arguments */
	argc -= args_parsed;
	argv += args_parsed;
	res = parse_args(argc, argv);
	if (res < 0)
		return 0;

	if (port_init(dpdk_port, rx_mbuf_pool, num_queues) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %d\n", dpdk_port);

	pthread_t rx_tid;
	pthread_create(&rx_tid, nullptr, *client_rx, nullptr);
	// wait a bit
	rte_delay_us_block(1000);
	run_client();
	process_data();

	return 0;
}
