ARCH = $(shell uname -m)

all: $(ARCH)/syncfs $(ARCH)/c00fs $(ARCH)/c00gitfs

clean:
	rm -f $(ARCH)/syncfs $(ARCH)/c00fs $(ARCH)/c00gitfs

$(ARCH)/syncfs: syncfs.cc
	mkdir -p $(ARCH)
	g++ -g3 ./syncfs.cc -I/usr/include/fuse3 -lfuse3 -lpthread -ljsoncpp -o $@

$(ARCH)/c00fs: c00fs.cc
	mkdir -p $(ARCH)
	g++ -g3 ./c00fs.cc -I/usr/include/fuse3 -lfuse3 -lpthread -ljsoncpp -o $@

$(ARCH)/c00gitfs: c00gitfs.cc
	mkdir -p $(ARCH)
	g++ -g3 ./c00gitfs.cc -I/usr/include/fuse3 -lfuse3 -lpthread -ljsoncpp -o $@

