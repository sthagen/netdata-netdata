// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_RECEIVER_H
#define NETDATA_RECEIVER_H

#include "libnetdata/libnetdata.h"
#include "database/rrd.h"

struct parser;

struct receiver_state {
    RRDHOST *host;
    pid_t tid;
    ND_THREAD *thread;
    int fd;
    char *key;
    char *hostname;
    char *registry_hostname;
    char *machine_guid;
    char *os;
    char *timezone;         // Unused?
    char *abbrev_timezone;
    int32_t utc_offset;
    char *client_ip;        // Duplicated in pluginsd
    char *client_port;        // Duplicated in pluginsd
    char *program_name;        // Duplicated in pluginsd
    char *program_version;
    struct rrdhost_system_info *system_info;
    STREAM_CAPABILITIES capabilities;
    time_t last_msg_t;
    time_t connected_since_s;

    struct buffered_reader reader;

    uint16_t hops;

    struct {
        bool shutdown;      // signal the streaming parser to exit
        STREAM_HANDSHAKE reason;
    } exit;

    struct {
        RRD_MEMORY_MODE mode;
        int history;
        int update_every;
        int health_enabled; // CONFIG_BOOLEAN_YES, CONFIG_BOOLEAN_NO, CONFIG_BOOLEAN_AUTO
        time_t alarms_delay;
        uint32_t alarms_history;
        int rrdpush_enabled;
        const char *rrdpush_api_key; // DONT FREE - it is allocated in appconfig
        const char *rrdpush_send_charts_matching; // DONT FREE - it is allocated in appconfig
        bool rrdpush_enable_replication;
        time_t rrdpush_seconds_to_replicate;
        time_t rrdpush_replication_step;
        const char *rrdpush_destination;  // DONT FREE - it is allocated in appconfig
        unsigned int rrdpush_compression;
        STREAM_CAPABILITIES compression_priorities[COMPRESSION_ALGORITHM_MAX];
    } config;

    NETDATA_SSL ssl;

    time_t replication_first_time_t;

    struct decompressor_state decompressor;
    /*
    struct {
        uint32_t count;
        STREAM_NODE_INSTANCE *array;
    } instances;
*/

    // The parser pointer is safe to read and use, only when having the host receiver lock.
    // Without this lock, the data pointed by the pointer may vanish randomly.
    // Also, since the receiver sets it when it starts, it should be read with
    // an atomic read.
    struct parser *parser;

#ifdef ENABLE_H2O
    void *h2o_ctx;
#endif
};

#ifdef ENABLE_H2O
#define is_h2o_rrdpush(x) ((x)->h2o_ctx != NULL)
#define unless_h2o_rrdpush(x) if(!is_h2o_rrdpush(x))
#endif

int rrdpush_receiver_thread_spawn(struct web_client *w, char *decoded_query_string, void *h2o_ctx);

void receiver_state_free(struct receiver_state *rpt);
bool stop_streaming_receiver(RRDHOST *host, STREAM_HANDSHAKE reason);

#endif //NETDATA_RECEIVER_H
