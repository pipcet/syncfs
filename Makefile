all: a.out syncfs_mount c00fs c00gitfs

clean:
	rm -f a.out syncfs_mount c00fs c00gitfs

a.out: syncfs_mount
	cp $< $@

syncfs_mount: syncfs.cc
	g++ -g3 ./syncfs.cc -I/usr/include/fuse3 -lfuse3 -lpthread -ljsoncpp -o syncfs_mount


c00fs: c00fs.cc
	g++ -g3 ./c00fs.cc -I/usr/include/fuse3 -lfuse3 -lpthread -ljsoncpp -o c00fs

c00gitfs: c00gitfs.cc
	g++ -g3 ./c00gitfs.cc -I/usr/include/fuse3 -lfuse3 -lpthread -ljsoncpp -o c00gitfs

