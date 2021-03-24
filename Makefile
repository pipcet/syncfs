all: syncfs c00fs c00gitfs

clean:
	rm -f syncfs c00fs c00gitfs

syncfs: syncfs.cc
	g++ -g3 ./syncfs.cc -I/usr/include/fuse3 -lfuse3 -lpthread -ljsoncpp -o syncfs


c00fs: c00fs.cc
	g++ -g3 ./c00fs.cc -I/usr/include/fuse3 -lfuse3 -lpthread -ljsoncpp -o c00fs

c00gitfs: c00gitfs.cc
	g++ -g3 ./c00gitfs.cc -I/usr/include/fuse3 -lfuse3 -lpthread -ljsoncpp -o c00gitfs

