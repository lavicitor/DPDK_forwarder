#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <signal.h>
#include <getopt.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_malloc.h>
#include <rte_pdump.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

#ifdef DEBUG
    #define DEBUG_LOG(fmt, ...) printf("DEBUG [%s:%d]: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#else
    #define DEBUG_LOG(fmt, ...) do {} while (0)
#endif

static uint64_t config_export_sec = 5;
static uint64_t config_timeout_sec = 30;
static uint32_t config_hash_entries = 65536;
static bool use_podman_mode = false;
static uint64_t hash_full_dropped_configs[RTE_MAX_LCORE] = {0};
struct rte_ring *rx_to_tx_rings[RTE_MAX_LCORE];

struct flow_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
} __attribute__((packed));

struct flow_stats {
    struct flow_key key;
    uint64_t rx_pkts;
    uint64_t rx_bytes;
    uint64_t tx_pkts;
    uint64_t tx_bytes;
    uint64_t last_seen;
    bool active;
};

// For running in container since RSS is not supported
static struct rte_eth_conf port_conf_podman = {
    .rxmode = {
        .mq_mode = RTE_ETH_MQ_RX_NONE,
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = NULL,
            .rss_hf = 0,
        },
    },
};

static struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .mq_mode = RTE_ETH_MQ_RX_RSS,
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = (uint8_t[]){
                0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
                0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
                0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
                0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
                0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a
            },
            .rss_key_len = 40,
            .rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP,
        },
    },
};

struct rte_hash *flow_tables[RTE_MAX_LCORE];
struct flow_stats *stats_storage[RTE_MAX_LCORE];
uint64_t export_interval_cycles;
uint64_t timeout_cycles;

static void export_stats(unsigned int lcore_id) {
    char filename[64];
    snprintf(filename, sizeof(filename), "logs/flow_stats_core_%u.csv", lcore_id);
    FILE *f = fopen(filename, "a");
    if (!f) return;

    uint32_t iter = 0;
    const void *key_ptr;
    void *data;
    uint64_t now = rte_get_timer_cycles();

    while (rte_hash_iterate(flow_tables[lcore_id], &key_ptr, &data, &iter) >= 0) {
        int32_t slot = (int32_t)(intptr_t)data;
        struct flow_stats *s = &stats_storage[lcore_id][slot];

        if (s->active) {
            if (now - s->last_seen > timeout_cycles) {
                s->active = false;
                rte_hash_del_key(flow_tables[lcore_id], key_ptr);
                continue;
            }

            fprintf(f, "%"PRIu64",%u.%u.%u.%u,%u.%u.%u.%u,%u,%u,%u,%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64"\n",
                now,
                (s->key.src_ip & 0xFF), (s->key.src_ip >> 8) & 0xFF, (s->key.src_ip >> 16) & 0xFF, (s->key.src_ip >> 24) & 0xFF,
                (s->key.dst_ip & 0xFF), (s->key.dst_ip >> 8) & 0xFF, (s->key.dst_ip >> 16) & 0xFF, (s->key.dst_ip >> 24) & 0xFF,
                rte_be_to_cpu_16(s->key.src_port), rte_be_to_cpu_16(s->key.dst_port), s->key.proto,
                s->rx_pkts, s->rx_bytes, s->tx_pkts, s->tx_bytes);
        }
    }
    fclose(f);
}

static void port_init(uint16_t port, struct rte_mempool *mbuf_pool) {
    struct rte_eth_conf port_conf = use_podman_mode ? port_conf_podman : port_conf_default;
    struct rte_eth_dev_info dev_info;
    int ret;
    
    ret = rte_eth_dev_info_get(port, &dev_info);
    if (ret != 0) {
        rte_exit(EXIT_FAILURE, "Error during gets dev info for port %u: %s\n", 
                 port, strerror(-ret));
    }

    if (!use_podman_mode) {
        port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;
    }

    ret = rte_eth_dev_configure(port, 1, 1, &port_conf);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n", ret, port);
    }

    ret = rte_eth_rx_queue_setup(port, 0, RX_RING_SIZE, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "rx_queue_setup failed: err=%d, port=%u\n", ret, port);
    }

    ret = rte_eth_tx_queue_setup(port, 0, TX_RING_SIZE, rte_eth_dev_socket_id(port), NULL);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "tx_queue_setup failed: err=%d, port=%u\n", ret, port);
    }

    ret = rte_eth_dev_start(port);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "rte_eth_dev_start failed: err=%d, port=%u\n", ret, port);
    }
}

static int lcore_tx(__rte_unused void *arg) {
    unsigned int lcore_id = rte_lcore_id();
    struct rte_mbuf *bufs[BURST_SIZE];
    
    printf("Core %u starting TX worker...\n", lcore_id);

    while (1) {
        for (int i = 0; i < RTE_MAX_LCORE; i++) {
            if (rx_to_tx_rings[i] == NULL) continue;

            uint16_t nb_rx = rte_ring_dequeue_burst(rx_to_tx_rings[i], (void *)bufs, BURST_SIZE, NULL);
            if (nb_rx == 0) continue;

            uint16_t nb_tx = rte_eth_tx_burst(bufs[0]->port, 0, bufs, nb_rx);
            
            if (nb_tx < nb_rx) {
                for (uint16_t buf = nb_tx; buf < nb_rx; buf++) {
                    rte_pktmbuf_free(bufs[buf]);
                }
            }
        }
    }
    return 0;
}

static int lcore_main(__rte_unused void *arg) {
    unsigned int lcore_id = rte_lcore_id();
    uint16_t port;
    struct rte_mbuf *bufs[BURST_SIZE];
    uint64_t last_export = rte_get_timer_cycles();
    struct rte_ether_addr my_mac;
    struct rte_ether_addr other_mac;

    printf("Core %u forwarding packets...\n", lcore_id);

    while (1) {
        uint64_t now = rte_get_timer_cycles();
        if (now - last_export > export_interval_cycles) {
            export_stats(lcore_id);
            last_export = now;
        }

        RTE_ETH_FOREACH_DEV(port) {
            if (port > 1) continue;
            rte_eth_macaddr_get(port, &my_mac);
            rte_eth_macaddr_get(port ^ 1, &other_mac);

            uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
            if (nb_rx == 0) continue;

            for (int i = 0; i < nb_rx; i++) {
                struct rte_mbuf *m = bufs[i];
                struct rte_ether_hdr *eth = rte_pktmbuf_mtod(bufs[i], struct rte_ether_hdr *);
                
                if (rte_be_to_cpu_16(eth->ether_type) == RTE_ETHER_TYPE_ARP) {
                    DEBUG_LOG("ARP Packet received on Port %u\n", port);
                    struct rte_arp_hdr *arp = rte_pktmbuf_mtod_offset(bufs[i], struct rte_arp_hdr *, sizeof(struct rte_ether_hdr));

                    if (rte_be_to_cpu_16(arp->arp_opcode) == RTE_ARP_OP_REQUEST) {
                        DEBUG_LOG("Valid ARP Request for IP: %u.%u.%u.%u\n", 
                                (arp->arp_data.arp_tip & 0xFF), (arp->arp_data.arp_tip >> 8) & 0xFF, 
                                (arp->arp_data.arp_tip >> 16) & 0xFF, (arp->arp_data.arp_tip >> 24) & 0xFF);
                        rte_ether_addr_copy(&eth->src_addr, &eth->dst_addr);
                        rte_ether_addr_copy(&my_mac, &eth->src_addr);

                        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);
                        arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);

                        uint32_t req_ip = arp->arp_data.arp_sip;
                        uint32_t target_ip = arp->arp_data.arp_tip;
                        
                        arp->arp_data.arp_sip = target_ip;
                        arp->arp_data.arp_tip = req_ip;

                        rte_ether_addr_copy(&arp->arp_data.arp_sha, &arp->arp_data.arp_tha);
                        rte_ether_addr_copy(&my_mac, &arp->arp_data.arp_sha);

                        m->port = port;
                        if (rte_ring_enqueue(rx_to_tx_rings[lcore_id], m) < 0) {
                            rte_pktmbuf_free(m);
                        }
                        
                        continue;
                    }
                }

                if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4) {
                    rte_pktmbuf_free(bufs[i]);
                    continue;
                }

                struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
                struct flow_key key = {
                    .src_ip = ip->src_addr,
                    .dst_ip = ip->dst_addr,
                    .proto = ip->next_proto_id
                };

                if (key.proto == IPPROTO_TCP) {
                    struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)((unsigned char *)ip + sizeof(struct rte_ipv4_hdr));
                    key.src_port = tcp->src_port;
                    key.dst_port = tcp->dst_port;
                } else if (key.proto == IPPROTO_UDP) {
                    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((unsigned char *)ip + sizeof(struct rte_ipv4_hdr));
                    key.src_port = udp->src_port;
                    key.dst_port = udp->dst_port;
                }

                rte_ether_addr_copy(&other_mac, &eth->src_addr);

                int ret = rte_hash_add_key(flow_tables[lcore_id], &key);
                if (ret >= 0) {
                    stats_storage[lcore_id][ret].key = key;
                    stats_storage[lcore_id][ret].rx_pkts++;
                    stats_storage[lcore_id][ret].rx_bytes += bufs[i]->pkt_len;
                    stats_storage[lcore_id][ret].last_seen = now;
                    stats_storage[lcore_id][ret].active = true;
                } else {
                    hash_full_dropped_configs[lcore_id]++;
                    if (unlikely(hash_full_dropped_configs[lcore_id] % 100000 == 1)) {
                        printf("Core %u: Flow table full, cannot track new flows.\n", lcore_id);
                    }
                }   

                m->port = port ^ 1;
                if (rte_ring_enqueue(rx_to_tx_rings[lcore_id], m) == 0) {
                    stats_storage[lcore_id][ret].tx_pkts++;
                    stats_storage[lcore_id][ret].tx_bytes += bufs[i]->pkt_len;
                } else {
                    rte_pktmbuf_free(m);
                }
            }
        }
    }
    return 0;
}

static void print_usage(const char *prgname) {
    printf("%s [EAL options] -- --export-interval SEC --timeout SEC --hash-entries INT [--podman-mode]\n", prgname);
}

int main(int argc, char *argv[]) {
    DEBUG_LOG("--- DEBUG MODE ACTIVE ---");
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) rte_exit(EXIT_FAILURE, "EAL init failed\n");

    argc -= ret;
    argv += ret;

    static struct option long_options[] = {
        {"export-interval", required_argument, 0, 'e'},
        {"timeout",         required_argument, 0, 't'},
        {"podman-mode",     no_argument,       0, 'p'},
        {"hash-entries",    required_argument, 0, 's'},
        {0, 0, 0, 0}
    };

    int opt, opt_idx = 0;
    while ((opt = getopt_long(argc, argv, "e:t:s:p", long_options, &opt_idx)) != -1) {
        switch (opt) {
            case 'e': config_export_sec = strtoull(optarg, NULL, 10); break;
            case 't': config_timeout_sec = strtoull(optarg, NULL, 10); break;
            case 's': config_hash_entries = strtoull(optarg, NULL, 10); break;
            case 'p': use_podman_mode = true; break;
            default: print_usage(argv[0]); return -1;
        }
    }

    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, 
        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    
    if (!mbuf_pool) rte_exit(EXIT_FAILURE, "Mempool creation failed\n");

    export_interval_cycles = rte_get_timer_hz() * config_export_sec;
    timeout_cycles = rte_get_timer_hz() * config_timeout_sec;

    printf("Starting Forwarder (Export: %lus, Timeout: %lus, Podman: %s)\n", 
           config_export_sec, config_timeout_sec, use_podman_mode ? "ON" : "OFF");

    if (rte_eth_dev_count_avail() < 2) rte_exit(EXIT_FAILURE, "Need at least 2 ports\n");

    uint16_t portid;
    RTE_ETH_FOREACH_DEV(portid) {
        port_init(portid, mbuf_pool);
    }

    if (rte_pdump_init() < 0) {
    fprintf(stderr, "Pdump init failed: %s\n", rte_strerror(rte_errno));
    } else {
        printf("Pdump initialized successfully.\n");
    }

    unsigned int lcore_id;
    RTE_LCORE_FOREACH(lcore_id) {
        char hash_name[32];
        snprintf(hash_name, sizeof(hash_name), "flow_table_%u", lcore_id);

        struct rte_hash_parameters hash_params = {
            .name = hash_name, 
            .entries = config_hash_entries, 
            .key_len = sizeof(struct flow_key),
            .hash_func = rte_jhash, 
            .socket_id = rte_socket_id(),
            .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY,
        };
        
        flow_tables[lcore_id] = rte_hash_create(&hash_params);

        stats_storage[lcore_id] = rte_zmalloc_socket("stats", 
            sizeof(struct flow_stats) * config_hash_entries, 
            RTE_CACHE_LINE_SIZE, rte_socket_id());

        char ring_name[32];
        snprintf(ring_name, sizeof(ring_name), "rx_to_tx_ring_%u", lcore_id);
        
        rx_to_tx_rings[lcore_id] = rte_ring_create(ring_name, 
            4096,
            rte_socket_id(), 
            RING_F_SP_ENQ | RING_F_SC_DEQ);
        
        if (!flow_tables[lcore_id] || !stats_storage[lcore_id] || !rx_to_tx_rings[lcore_id]) {
            rte_exit(EXIT_FAILURE, "Resource allocation failed on core %u (Hash, Stats, or Ring)\n", lcore_id);
        }
    }

    unsigned int tx_lcore = rte_get_next_lcore(rte_get_main_lcore(), 1, 0);
    if (tx_lcore == RTE_MAX_LCORE) {
        rte_exit(EXIT_FAILURE, "Not enough cores. Need at least 2.\n");
    }

    rte_eal_remote_launch(lcore_tx, NULL, tx_lcore);

    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (lcore_id == tx_lcore) continue;
        rte_eal_remote_launch(lcore_main, NULL, lcore_id);
    }
    rte_eal_mp_wait_lcore();
    return 0;
}