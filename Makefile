# Compilation Flags
all: httpd client

LIBS = -lpthread

httpd: httpd.c
	gcc -g -W -Wall $(LIBS) -o $@ $<

client: simpleclient.c
	gcc -W -Wall -o $@ $<

# Cleanup targets
clean:
	rm -f httpd client













