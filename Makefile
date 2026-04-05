# Compilation Flags
all: httpd client

LIBS = -lpthread

httpd: httpd.c sds.c thpool.c logger.c
	gcc -g -W -Wall -o httpd httpd.c sds.c thpool.c logger.c $(LIBS)

client: simpleclient.c
	gcc -W -Wall -o $@ $<

# Cleanup targets
clean:
	rm -f httpd client













