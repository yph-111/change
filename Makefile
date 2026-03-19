# Compilation Flags
all: httpd client

LIBS = -lpthread

httpd: httpd.c sds.c thpool.c
	gcc -g -W -Wall -o httpd httpd.c sds.c thpool.c $(LIBS)

client: simpleclient.c
	gcc -W -Wall -o $@ $<

# Cleanup targets
clean:
	rm -f httpd client













