#ifndef RELIABLERC_H_
#define RELIABLERC_H_

// 接收端缓冲管理
struct receive_buffer {
    uint32_t expected_seq;     // 期望的下一个序列号
    uint32_t last_received_seq; // 最后收到的序列号
    struct {
        uint8_t valid;
        uint8_t data[MAX_DATA_SIZE];
        uint16_t len;
        uint32_t timestamp;
    } packets[SEQUENCE_WINDOW];
    uint32_t missing_count;    // 丢包计数
    uint32_t reorder_count;    // 乱序计数
};

#endif