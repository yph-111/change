#include <stdlib.h>
#include <string.h>
#include "sds.h"

sds sdsempty(void) {
    struct sdshdr *sh = malloc(sizeof(struct sdshdr)+1);
    sh->len = 0; sh->free = 0; sh->buf[0] = '\0';
    return (char*)sh->buf;
}

sds sdscatlen(sds s, const void *t, size_t len) {
// 【指针逆向偏移】
    // sds 指针实际上指向的是 Header 之后的存储区。
    // 为了访问 Header 里的 len 和 free 等元数据，必须将地址向左回退一个结构体的大小。
    // 这种“隐藏头部”的设计，让 sds 能直接兼容标准 C 库函数（如 printf）
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
// 【物理内存重分配】
    // 申请新空间 = Header大小 + 旧数据长度 + 待追加长度 + 1字节 '\0'。
    // 调用 realloc 是：如果原地址后方空间不足，OS 会自动在堆区找新位置并搬运数据
    sh = realloc(sh, sizeof(struct sdshdr)+sh->len+len+1);
// 将新数据 t 精准拷贝到原数据末尾（sh->buf + sh->len）
    memcpy(sh->buf+sh->len, t, len);
// 更新 Header 中的长度属性，并手动维护 '\0'，确保二进制安全且向下兼容
    sh->len += len; sh->buf[sh->len] = '\0';
// 返回数据区的首地址。注意：如果 realloc 导致了地址迁移，这里的 sh 指针是更新后的
    return (char*)sh->buf;
}

void sdsfree(sds s) {
    if (s == NULL) return;
// 【闭环释放】
    // 绝对不能直接调用 free(s)，因为那样会漏掉 Header 部分的内存。
    // 必须通过指针运算回溯到最初 malloc 申请的起始地址，完成堆空间的彻底清理
    free(s-sizeof(struct sdshdr));
}

void sdsclear(sds s) {
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
// 【懒释放】
    // 仅仅将长度标为 0，并不真正触发系统级的 free 操作。
    // 这样在 Web 服务器处理后续请求时，可以重复利用这块已经申请好的堆内存，
    // 极大减少了频繁触发 realloc 带来的 CPU 性能损耗。
    sh->len = 0; sh->buf[0] = '\0';
}
