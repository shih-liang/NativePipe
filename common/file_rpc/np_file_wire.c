#include "np_file_rpc.h"

uint32_t np_file_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint64_t np_file_u64(const unsigned char *p) {
    return np_file_u32(p) | (uint64_t)np_file_u32(p + 4) << 32;
}
void np_file_put32(unsigned char *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(n >> (8 * i));
}
void np_file_put64(unsigned char *p, uint64_t n) {
    np_file_put32(p, (uint32_t)n); np_file_put32(p + 4, (uint32_t)(n >> 32));
}
