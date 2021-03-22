/* Rewrite. Doesn't use GPL2-only code. */

/* c00fs translates symlinks to ' ' <-> character device 0 0, which
   overlayfs uses as whiteout node. It also eats fsync. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define FUSE_USE_VERSION 35

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <fuse.h>
#include <fuse_lowlevel.h>
#include <inttypes.h>
#include <jsoncpp/json/json.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#include "cxxopts.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <list>
#include <mutex>
#include <thread>

class Path {
public:
  std::string path;
  Path(std::string inpath)
  {
    const char *str = inpath.c_str();
    while (str[0] == '/')
      str++;
    char *newstr = strdup(str[0] ? str : ".");
    if (newstr[0] == '.' && strcmp(newstr, ".") && strcmp(newstr, ".."))
      newstr[0] = ',';
    for (char *p = newstr; *p; p++) {
      if (*p == '/' && p[1] == '.')
	p[1] = ',';
    }

    path = std::string(newstr);
  }

  const char *c_str()
  {
    return path.c_str();
  }
};

struct Errno {
  int error;
  Errno(int error) : error(error) {}
  Errno() : Errno(errno) {}
};

static void fs_delete_recursively_at(int fd, std::string path)
{
  if (::unlinkat(fd, path.c_str(), 0) == 0)
    return;

  int dirfd;
  DIR *dir = fdopendir (dirfd = ::openat (fd, path.c_str(), O_DIRECTORY));
  if (!dir)
    return;
  struct dirent *dirent;
  while ((dirent = readdir(dir))) {
    std::string name(dirent->d_name);
    if (name == "." || name == "..")
      continue;
    fs_delete_recursively_at (dirfd, name);
  }
  closedir (dir);
  ::unlinkat(fd, path.c_str(), AT_REMOVEDIR);
}

static int root_fd;
static char *root_path;

static int fd_from_inode(fuse_ino_t ino)
{
  if (ino == 1)
    return root_fd;
  return (int) ino;
}

static bool is_symlink(Path path)
{
  try {
    struct stat statbuf;
    int res = ::fstatat(root_fd, path.c_str(), &statbuf, AT_SYMLINK_NOFOLLOW);
    if (res || !S_ISLNK(statbuf.st_mode))
      return 0;
    return 1;
  } catch (Errno error) {
    return 0;
  }
}

static bool is_whiteout(Path path)
{
  char link[256];
  if (is_symlink(path)) {
    if (readlinkat(root_fd, path.c_str(), link, sizeof link) < 0)
      return 0;
    link[255] = 0;
    return !strcmp(" ", link);
  }
  return 0;
}

static int c00fs_getattr(const char *path_str,
			 struct stat *statp,
			 fuse_file_info* fi_may_be_null)
{
  try {
    Path path(path_str);
    int ret = fstatat(root_fd, path.c_str(), statp, AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW);
    if (ret < 0)
      throw Errno();
    if (is_whiteout(path)) {
      statp->st_mode &= ~S_IFMT;
      statp->st_mode |= S_IFCHR;
      statp->st_dev = 0;
    }
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_readlink(const char *path_str, char *buf, size_t size)
{
  try {
    Path path(path_str);
    if (is_whiteout(path))
      throw Errno(EINVAL);
    ssize_t ret = ::readlinkat(root_fd, path.c_str(), buf, size);
    if (ret < 0)
      throw Errno();
    if (ret == size)
      throw Errno(ENAMETOOLONG);
    buf[ret] = 0;
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_mknod(const char *path_str, mode_t mode, dev_t dev)
{
  try {
    Path path(path_str);
    if (major(dev) || minor(dev))
      throw Errno(EINVAL);
    if (::symlinkat(" ", root_fd, path.c_str()) < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_mkdir(const char *path_str, mode_t mode)
{
  try {
    Path path(path_str);
    mode |= S_IFDIR;
    int ret = ::mkdirat(root_fd, path.c_str(), mode);
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_rmdir(const char *path_str)
{
  try {
    Path path(path_str);
    int ret = ::unlinkat(root_fd, path.c_str(), AT_REMOVEDIR);
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_unlink(const char *path_str)
{
  try {
    Path path(path_str);
    size_t size = 0;
    int ret = ::unlinkat(root_fd, path.c_str(), 0);
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_chmod(const char *path_str, mode_t mode,
		      fuse_file_info *fi)
{
  try {
    Path path(path_str);
    if (is_symlink(path))
      return 0;
    int res = ::fchmodat(root_fd, path.c_str(), mode, 0);
    if (res < 0)
      throw Errno();
    return res;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_chown(const char *path_str, uid_t uid, gid_t gid,
		      fuse_file_info *fi)
{
  try {
    Path path(path_str);
    if (is_symlink(path))
      return 0;
    int res = ::fchownat(root_fd, path.c_str(), uid, gid, 0);
    if (res < 0)
      throw Errno();
    return res;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_readdir(const char *path_str, void *buf, fuse_fill_dir_t filler,
			off_t offset, fuse_file_info *, enum fuse_readdir_flags)
{
    Path path(path_str);
  DIR *dir = ::fdopendir (openat (root_fd, path.c_str(), O_DIRECTORY));
  if (!dir)
    return -errno;
  struct dirent *dirent;
  while (dirent = ::readdir (dir)) {
    char *name = dirent->d_name;
    struct stat stat;
    ::fstatat(dirfd(dir), name, &stat, 0);
    if (is_whiteout(path)) {
      stat.st_mode &= ~S_IFMT;
      stat.st_mode |= S_IFCHR;
      stat.st_dev = 0;
    }
    if (name[0] == ',')
      name[0] = '.';
    else if (name[0] == '.' && (strcmp(name, ".")) && (strcmp(name, "..")))
      continue;
    if (filler (buf, name, &stat, 0, (fuse_fill_dir_flags)0))
      break;
  }
  closedir(dir);

  return 0;
}

static int c00fs_create(const char *path_str, mode_t mode, fuse_file_info *fi)
{
  try {
    Path path(path_str);
    size_t size = 0;
    int fd = ::openat(root_fd, path.c_str(), O_CREAT|fi->flags, mode);
    if (fd < 0)
      throw Errno();
    fi->fh = fd;
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_open(const char *path_str, fuse_file_info *fi)
{
    Path path(path_str);
  int fd = ::openat(root_fd, path.c_str(), fi->flags);
  if (fd < 0)
    return -errno;
  fi->fh = fd;
  return 0;
}

static int c00fs_utimens(const char *path_str, const struct timespec tv[2],
			struct fuse_file_info *fi)
{
  Path path(path_str);
  if (is_symlink(path))
    return 0;
  int fd = ::utimensat(root_fd, path.c_str(), tv, 0);
  if (fd < 0)
    return -errno;
  return 0;
}

static int c00fs_fsync(const char *path_str, int datasync,
		      struct fuse_file_info *fi)
{
  try {
    Path path(path_str);
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_setxattr(const char *path_str, const char *name,
			 const char *value, size_t size, int flags)
{
  try {
    Path path(path_str);
    const char *prefix = "syncfs.user.";
    char *prefixed_name =
      (char *)malloc (strlen(prefix) + strlen(name) + 1);
    char *real_path =
      (char *)malloc(strlen(root_path) + strlen(path.c_str()) + 1);
    sprintf(real_path, "%s%s", root_path, path.c_str());
    sprintf(prefixed_name, "%s%s", prefix, name);
    int ret = setxattr(real_path, prefixed_name, value, size, flags);
    free (real_path);
    free (prefixed_name);
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_removexattr(const char *path_str, const char *name)
{
  try {
    Path path(path_str);
    const char *prefix = "syncfs.user.";
    char *prefixed_name =
      (char *)malloc (strlen(prefix) + strlen(name) + 1);
    char *real_path =
      (char *)malloc(strlen(root_path) + strlen(path.c_str()) + 1);
    sprintf(real_path, "%s%s", root_path, path.c_str());
    sprintf(prefixed_name, "%s%s", prefix, name);
    int ret = removexattr(real_path, prefixed_name);
    free (real_path);
    free (prefixed_name);
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_getxattr(const char *path_str, const char *name,
			 char *value, size_t size)
{
  try {
    Path path(path_str);
    const char *prefix = "syncfs.user.";
    char *prefixed_name =
      (char *)malloc (strlen(prefix) + strlen(name) + 1);
    char *real_path =
      (char *)malloc(strlen(root_path) + strlen(path.c_str()) + 1);
    sprintf(real_path, "%s%s", root_path, path.c_str());
    sprintf(prefixed_name, "%s%s", prefix, name);
    int ret = getxattr(real_path, prefixed_name, value, size);
    free (real_path);
    free (prefixed_name);
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_listxattr(const char *path_str, char *buf, size_t size)
{
  try {
    Path path(path_str);
    const char *prefix = "syncfs.user.";
    char *real_path =
      (char *)malloc(strlen(root_path) + strlen(path.c_str()) + 1);
    sprintf(real_path, "%s%s", root_path, path.c_str());
    size_t bufsize = 2 * size;
    char *mybuf = (char *)malloc (bufsize);
    ssize_t ret;
  again:
    ret = listxattr(real_path, mybuf, 2 * size);
    if (ret < 0)
      throw Errno();
    if (ret == bufsize) {
      bufsize *= 2;
      mybuf = (char *)realloc(mybuf, bufsize);
      goto again;
    }
    mybuf[ret] = 0;

    for (char *p = mybuf, *q = buf; ;)
      {
	if (p == mybuf + ret)
	  break;
	if (strncmp(p, prefix, strlen(prefix) == 0)) {
	  p += strlen(prefix);
	  ssize_t len = snprintf (q, buf + size - q, "%s", p);
	  q += len;
	  p += len;
	  *q++ = 0;
	  p++;
	} else {
	  p += strlen(p);
	}
	if (q == buf + size)
	  break;
      }
    free (mybuf);
    free (real_path);
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_fsyncdir(const char *path_str, int datasync,
			 struct fuse_file_info *fi)
{
  try {
    Path path(path_str);
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_read_buf(const char *path_str, struct fuse_bufvec **out_buf,
			 size_t size, off_t off, fuse_file_info *fi)
{
  try {
    Path path(path_str);
    int fd = fi->fh;
    fuse_bufvec buf = FUSE_BUFVEC_INIT(size);
    buf.buf[0].flags = static_cast<fuse_buf_flags>(FUSE_BUF_IS_FD|FUSE_BUF_FD_SEEK);
    buf.buf[0].fd = fd;
    buf.buf[0].pos = off;
    *out_buf = (struct fuse_bufvec *)malloc(sizeof **out_buf);
    **out_buf = buf;
    int res = 0;
    if (res < 0)
      throw Errno(res);
    return res;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_write_buf(const char *path_str, struct fuse_bufvec *in_buf,
			  off_t off, fuse_file_info *fi)
{
  try {
    Path path(path_str);
    size_t size = fuse_buf_size(in_buf);
    int fd = fi->fh;
    fuse_bufvec buf = FUSE_BUFVEC_INIT(size);
    buf.buf[0].flags = static_cast<fuse_buf_flags>(FUSE_BUF_IS_FD|FUSE_BUF_FD_SEEK);
    buf.buf[0].fd = fd;
    buf.buf[0].pos = off;
    ssize_t res = fuse_buf_copy(&buf, in_buf, fuse_buf_copy_flags());
    if (res < 0)
      throw Errno(res);
    return res;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_release(const char *path_str, fuse_file_info *fi)
{
  ::close (fi->fh);
  return 0;
}

static int c00fs_symlink(const char *target, const char *path_str)
{
  try {
    Path path(path_str);
    int ret = ::symlinkat(target, root_fd, path.c_str());
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_link(const char *path1, const char *path_str)
{
  try {
    Path path(path_str);
    while (path1[0] == '/')
      path1++;
    int ret = ::linkat(root_fd, path1,
		       AT_FDCWD, path.c_str(), 0);
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00fs_rename(const char *path_str, const char *path2_str,
		       unsigned int flags)
{
  try {
    Path path(path_str);
    Path path2(path2_str);
    size_t size = 0;
    int ret = ::renameat2(root_fd, path.c_str(),
			  root_fd, path2.c_str(),
			  0);
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static struct fuse_operations c00fs_operations = {
  .getattr = c00fs_getattr,
  .readlink = c00fs_readlink,
  .mknod = c00fs_mknod,
  .mkdir = c00fs_mkdir,
  .unlink = c00fs_unlink,
  .rmdir = c00fs_rmdir,
  .symlink = c00fs_symlink,
  .rename = c00fs_rename,
  .link = c00fs_link,
  .chmod = c00fs_chmod,
  .chown = c00fs_chown,
  .open = c00fs_open,
  .fsync = c00fs_fsync,
  //.setxattr = syncfs_setxattr,
  //.getxattr = syncfs_getxattr,
  //.listxattr = syncfs_listxattr,
  //.removexattr = syncfs_removexattr,
  .readdir = c00fs_readdir,
  .fsyncdir = c00fs_fsyncdir,
  .create = c00fs_create,
  .utimens = c00fs_utimens,
  .write_buf = c00fs_write_buf,
  .read_buf = c00fs_read_buf,
};

int main(int argc, char **argv)
{
  int lower_fd = open(argv[1], O_RDONLY|O_PATH);
  if (lower_fd < 0)
    abort();
  root_fd = lower_fd;
  root_path = strdup(argv[1]);
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
    limit.rlim_cur = limit.rlim_max;
    setrlimit(RLIMIT_NOFILE, &limit);
  }
  fuse_args args = FUSE_ARGS_INIT(0, nullptr);
  if (fuse_opt_add_arg(&args, argv[0]))
    abort();
  if (fuse_opt_add_arg(&args, "-o"))
    abort();
  if (fuse_opt_add_arg(&args, "fsname=c00fs"))
    abort();
  //if (fuse_opt_add_arg(&args, "-odebug"))
  //  abort();
  fuse *fuse = fuse_new (&args, &c00fs_operations, sizeof(c00fs_operations),
			 nullptr);
  fuse_mount(fuse, argv[2]);

  fuse_loop(fuse);
  //fuse_set_signal_handlers(session);

  umask(0);

  //if (fuse_session_mount(session, argv[2]))
  //abort();

  //fuse_remove_signal_handlers(session);
  //fuse_session_destroy(session);

  return 0;
}
