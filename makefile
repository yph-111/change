# 统一协议定义
CC = gcc
CFLAGS = -g -W -Wall
# 关键修复：显式链接多线程库，终结依赖地狱风险
LIBS = -lpthread

# 目标管理：同步维护服务器与客户端
all: httpd client

# 编译服务器逻辑
httpd: httpd.c
	$(CC) $(CFLAGS) -o httpd httpd.c $(LIBS)

# 编译客户端原型
client: simpleclient.c
	$(CC) $(CFLAGS) -o client simpleclient.c

# 严谨的物理清理指令：实现物理文件与逻辑的解耦
clean:
	rm -f httpd client
