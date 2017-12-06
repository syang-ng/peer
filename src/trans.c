//
// Created by syang on 2017/11/30.
//
#include <stdio.h>
#include <malloc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "chunk.h"
#include "packet.h"
#include "trans.h"
extern bt_config_t config;
extern queue *has_chunks;

queue *init_chunk_file(const char*chunk_file) {
    FILE *fp = fopen(chunk_file, "r");
    queue *chunks = make_queue();
    char read_buf[64];
    char sha_buf[2 * SHA1_HASH_SIZE];
    uint8_t *chunk;
    int i;
    while (fgets(read_buf, 64, fp) != NULL) {
        sscanf(read_buf, "%d %s", &i, sha_buf);
        chunk = malloc(SHA1_HASH_SIZE);
        hex2binary(sha_buf, 2 * SHA1_HASH_SIZE, chunk);
        enqueue(chunks, chunk);
    }
    fclose(fp);
    return chunks;
}

void init_has_chunks(const char *has_chunk_file) {
    has_chunks = init_chunk_file(has_chunk_file);
}

int check_i_have(uint8_t *hash_wanted) {
    if (has_chunks == NULL) {
        init_has_chunks(config.has_chunk_file);
    }
    if (has_chunks->n == 0) {
        return -1;
    }
    node *current = has_chunks->head;
    uint8_t *hash_value;
    while (current != NULL) {
        hash_value = current->data;
        if (memcmp(hash_wanted, hash_value, SHA1_HASH_SIZE) == 0) {
            return 1;
        }
        current = current->next;
    }
    return 0;
}

queue *which_i_have(queue *wanted) {
    queue *result = make_queue();
    uint8_t *chunk;
    while ((chunk = dequeue(wanted)) != NULL) {
        int ret = check_i_have(chunk);
        if (ret == 1) {
            enqueue(result, chunk);
        } else {
            free(chunk);
        }

    }
    if(!result->n) {
        free_queue(result, 0);
        result = NULL;
    }
    // free(wanted); //as wanted is empty simply free it
    return result;
}

/* take method as an arg to simplify the code */
queue *init_chunk_packet_queue(queue *chunks, data_packet_t *(*make_chunk_packet)(short, void *)) {
    queue *pkts = make_queue();
    uint8_t *chunk;
    int i;
    while (chunks->n) {
        size_t pkt_num = (size_t) (chunks->n < MAX_CHANK_NUM ? chunks->n : MAX_CHANK_NUM);
        size_t data_len = pkt_num * SHA1_HASH_SIZE + 4;
        char *data_t = malloc(data_len);
        *(int *) data_t = pkt_num;
        i = 0;
        while ((chunk = dequeue(chunks)) != NULL) {
            memcpy(data_t + 4 + i * SHA1_HASH_SIZE, chunk, SHA1_HASH_SIZE);
            free(chunk);
            i++;
        }
        data_packet_t *pkt = make_chunk_packet((short) (data_len+HEADERLEN), data_t);
        enqueue(pkts, pkt);
    }
    free_queue(chunks, 0); // no need to free recursively
    return pkts;
}

queue *init_whohas_queue(const char *chunk_file) {
    queue *whohas_chunks = init_chunk_file(chunk_file);
    return init_chunk_packet_queue(whohas_chunks, make_whohas_packet);
}

queue *init_ihave_queue(queue *chunks) {
    return init_chunk_packet_queue(chunks, make_ihave_packet);
}

queue *data2chunks_queue(void *data) {
    queue *chunks = make_queue();
    int num = *(char *) data;
    char *ptr = data + 4;
    for (int i = 0; i < num; i++) {
        char *chunk = malloc(SHA1_HASH_SIZE);
        memcpy(chunk, ptr + i * SHA1_HASH_SIZE, SHA1_HASH_SIZE);
        enqueue(chunks, chunk);
    }
    return chunks;
}

data_packet_t **init_data_array(uint8_t *sha) {
    FILE *fp = fopen(config.chunk_file, "r");
    if (fp == NULL) {
        fprintf(stderr, "Error!: file %s doesn't exist!", config.chunk_file);
        return NULL;
    }
    char buf[BT_FILENAME_LEN+8]; // File: file_name ***** \n\r
    char master_data_file[BT_FILENAME_LEN];
    char sha_buf[2*SHA1_HASH_SIZE];
    // get the master data file name
    fgets(buf, BT_FILENAME_LEN+8, fp);
    sscanf(master_data_file, "File: %s\n", buf);
    // skip next line
    fgets(buf, BT_FILENAME_LEN, fp);
    // find the correct chunk
    int i = -1;
    uint8_t sha_binary_buf[SHA1_HASH_SIZE];
    while (fgets(buf, BT_FILENAME_LEN, fp)!=NULL) {
        sscanf(buf, "%d %s", &i, sha_buf);
        hex2binary(sha_buf, 2*SHA1_HASH_SIZE, sha_binary_buf);
        if(memcmp(sha, sha_binary_buf, SHA1_HASH_SIZE)==0){
            break;
        }
    }
    fclose(fp);
    if(i==-1) {
        fprintf(stderr, "Error!: No such chunk: %s", sha_buf);
        return NULL;
    }
    // map the file to memory
    int data_fd = open(master_data_file, O_RDONLY);
    struct stat master_data_stat;
    fstat(data_fd, &master_data_stat);
    char *master_data = mmap(0, (size_t)master_data_stat.st_size, PROT_READ, MAP_SHARED, data_fd, 0);
    close(data_fd);
    // make the data pkts
    data_packet_t **data_pkts = malloc(BT_CHUNK_KSIZE*sizeof(data_packet_t *));
    data_packet_t *pkt;
    for(uint j = 0; j < BT_CHUNK_KSIZE; j++){
        char *data = master_data+i*BT_CHUNK_SIZE+j*1024;
        pkt = make_data_packet(HEADERLEN+DATA_PACKET_DATA_LEN, 0, j, data);
        data_pkts[j] = pkt;
    }
    munmap(master_data, (size_t)master_data_stat.st_size); // munmap the memory
    return data_pkts;
}

/* 发出队列中的所有包 */
void send_pkts(int sock, struct sockaddr *dst, queue *pkts) {
    data_packet_t *pkt;
    while ((pkt = dequeue(pkts)) != NULL) {
        fflush(stdout);
        send_packet(sock, pkt, dst);
        free_packet(pkt); // free the data of the node
    }
}

/* 遍历询问网络中的所有节点 */
void flood_whohas(int sock, queue *pkts) {
    data_packet_t *pkt;
    while ((pkt = dequeue(pkts)) != NULL) { // can use the send_pkts() because we can't free the pkt here
        bt_peer_t *current = config.peers;
        while (current != NULL) {
            if (current->id != config.identity) {
                send_packet(sock, pkt, (struct sockaddr *) &current->addr);
            }
            current = current->next;
        }
        free_packet(pkt); // free the data of the node
    }
}

/* 发送data数据包 */
void send_data_packets(up_conn_t *conn, int sock, struct sockaddr *dst) {
    while(conn->last_sent<conn->available && conn->last_sent<BT_CHUNK_KSIZE) {
        send_packet(sock, conn->pkts[conn->last_sent], dst);
        conn->last_sent++;
    }
}
