#ifndef __SDS_H
#define __SDS_H
#include <sys/types.h>

typedef char *sds;
struct sdshdr {
    int len;
    int free;
    char buf[];
};

sds sdsempty(void);
sds sdscatlen(sds s, const void *t, size_t len);
void sdsfree(sds s);
void sdsclear(sds s);
#endif
