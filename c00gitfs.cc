/* Rewrite. Doesn't use GPL2-only code. */

/* c00gitfs translates symlinks to ' ' <-> character device 0 0, which
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

#define WHITEOUT_CHAR ' '

class Path {
public:
  std::string path;
  Path(std::string dir, std::string base)
  {
    const char *str = dir.c_str();
    while (str[0] == '/')
      str++;
    char *newstr =
      (char *)malloc(strlen(str) + 1 + strlen(base.c_str()) + 1);
    sprintf(newstr, "%s/%s", str, base.c_str());
    path = std::string(newstr);
  }

  Path(std::string inpath)
  {
    const char *str = inpath.c_str();
    while (str[0] == '/')
      str++;
    char *newstr = strdup(str[0] ? str : ".");

    path = std::string(newstr);
  }

  ~Path()
  {
    //free((void *)const_cast<char *>(path.c_str()));
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

static int root_fd_reading;
static int root_fd_writing;
static int root_fd_restoring;
static int root_fd_metadata;
static int root_fd_metadata_wowo;
static int root_fd_metadata_lower;
static char *root_path_reading;
static char *root_path_writing;
static char *root_path_restoring;

static bool is_symlink(Path path)
{
  try {
    struct stat statbuf;
    int res = ::fstatat(root_fd_writing, path.c_str(), &statbuf,
			AT_SYMLINK_NOFOLLOW);
    if (res || !S_ISLNK(statbuf.st_mode))
      return 0;
    return 1;
  } catch (Errno error) {
    return 0;
  }
}

static bool is_whiteout(struct stat *stat)
{
  return S_ISCHR(stat->st_mode) && stat->st_rdev == 0;
}

static bool is_whiteout(Path path)
{
  char link[256];
  struct stat statbuf;
  int res = ::fstatat(root_fd_reading, path.c_str(), &statbuf,
		      AT_SYMLINK_NOFOLLOW);
  if (res < 0)
    return 0;
  return is_whiteout(&statbuf);
}

static bool is_whiteout_whiteout(Path path)
{
  size_t n = strlen(path.c_str());
  char str[strlen(".wowo/") + n + strlen("/.wo") + 1];
  sprintf(str, ".wowo/%s/.wo", path.c_str());
  char *p = strrchr(str, '/');
  do {
    sprintf(p, "/.wo");
    int fd = ::openat(root_fd_writing, str, O_RDONLY);
    if (fd > 0) {
      ::close(fd);
      return true;
    }
    *p = 0;
    p = strrchr(str, '/');
  } while (p);
  return false;
}

static void clear_whiteout_whiteout(Path path)
{
  char str[strlen(".wowo/") + strlen(path.c_str()) + strlen("/.wo") + 1];
  sprintf(str, ".wowo/%s/.wo", path.c_str());
  char *p = strrchr(str, '/');
  do {
    sprintf(p, "/.wo");
    ::unlinkat(root_fd_writing, str, 0);
    *p = 0;
    p = strrchr(str, '/');
  } while (p);
}

static void create_whiteout_whiteout(Path path)
{
  char str[strlen(".wowo/") + strlen(path.c_str()) + strlen("/.wo") + 1];
  sprintf(str, ".wowo/%s", path.c_str());
  for (char *p = str; *p; p++) {
    if (*p == '/') {
      *p = 0;
      ::mkdirat(root_fd_writing, str, 0770);
      *p = '/';
    }
  }
  ::mkdirat(root_fd_writing, str, 0770);
  sprintf(str, ".wowo/%s/.wo", path.c_str());
  ::close(::openat(root_fd_writing, str, O_CREAT, 0660));
}

static int c00gitfs_getattr(const char *path_str,
			    struct stat *statp,
			    fuse_file_info* fi_may_be_null)
{
  try {
    Path path(path_str);
    char str[strlen(path.c_str()) + 1];
    strcpy(str, path.c_str());
    int ret = fstatat(root_fd_reading, path.c_str(), statp,
		      AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW);
    if (ret < 0)
      throw Errno();
    if (is_whiteout_whiteout(path))
      throw Errno(ENOENT);
    if (is_whiteout(statp)) {
      statp->st_mode &= ~S_IFMT;
      statp->st_mode |= S_IFLNK;
      statp->st_mode |= 0777;
      statp->st_size = 1;
      statp->st_uid = getuid();
      statp->st_gid = getgid();
    }
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00gitfs_readlink(const char *path_str, char *buf, size_t size)
{
  try {
    Path path(path_str);
    if (is_whiteout(path)) {
      if (is_whiteout_whiteout(path))
	throw Errno(ENOENT);
      if (size < 2)
	throw Errno(ENAMETOOLONG);
      buf[0] = WHITEOUT_CHAR;
      buf[1] = 0;
      return 0;
    }
    ssize_t ret = ::readlinkat(root_fd_reading, path.c_str(), buf, size);
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

static int c00gitfs_mkdir(const char *path_str, mode_t mode)
{
  try {
    Path path(path_str);
    mode |= S_IFDIR;
    int ret = ::mkdirat(root_fd_writing, path.c_str(), mode);
    if (ret < 0) {
      if (errno == EEXIST) {
	clear_whiteout_whiteout(path);
	Path dotdir(path_str, ".dir");
	::close(::openat(root_fd_writing, dotdir.c_str(), O_CREAT|O_RDWR,
			 0660));
	return 0;
      }
      throw Errno();
    }
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00gitfs_unlink(const char *path_str)
{
  try {
    Path path(path_str);
    if (is_whiteout(path) && is_whiteout_whiteout(path))
      throw Errno(ENOENT);
    create_whiteout_whiteout(path);
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00gitfs_rmdir(const char *path_str)
{
  Path path(path_str);
  DIR *dir = ::fdopendir (openat (root_fd_reading, path.c_str(), O_DIRECTORY));
  if (!dir)
    return -errno;
  struct dirent *dirent;
  while (dirent = ::readdir (dir)) {
    char *name = dirent->d_name;
    if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".dir"))
      continue;
    if (is_whiteout_whiteout(Path(path_str, dirent->d_name)))
      continue;
    closedir(dir);
    return -EEXIST;
  }
  closedir(dir);
  return c00gitfs_unlink(path_str);
}

static int c00gitfs_chmod(const char *path_str, mode_t mode,
		      fuse_file_info *fi)
{
  try {
    Path path(path_str);
    if (is_symlink(path))
      return 0;
    int res = ::fchmodat(root_fd_writing, path.c_str(), mode, 0);
    if (res < 0)
      throw Errno();
    return res;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00gitfs_chown(const char *path_str, uid_t uid, gid_t gid,
		      fuse_file_info *fi)
{
  try {
    Path path(path_str);
    if (is_symlink(path))
      return 0;
    int res = ::fchownat(root_fd_writing, path.c_str(), uid, gid, 0);
    if (res < 0)
      throw Errno();
    return res;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00gitfs_readdir(const char *path_str, void *buf, fuse_fill_dir_t filler,
			off_t offset, fuse_file_info *, enum fuse_readdir_flags)
{
  Path path(path_str);
  DIR *dir = ::fdopendir (openat (root_fd_reading, path.c_str(), O_DIRECTORY));
  if (!dir)
    return -errno;
  struct dirent *dirent;
  while (dirent = ::readdir (dir)) {
    char *name = dirent->d_name;
    if (!strcmp(name, ".wowo") || !strcmp(name, ".dir"))
      continue;
    struct stat stat;
    ::fstatat(dirfd(dir), name, &stat, AT_SYMLINK_NOFOLLOW);
    if (is_whiteout_whiteout(Path(path_str, dirent->d_name)))
      continue;
    if (is_whiteout(&stat)) {
      stat.st_mode &= ~S_IFMT;
      stat.st_mode |= S_IFLNK;
      stat.st_mode |= 0777;
      stat.st_size = 1;
      stat.st_uid = getuid();
      stat.st_gid = getgid();
    }
    if (filler (buf, name, &stat, 0, (fuse_fill_dir_flags)FUSE_FILL_DIR_PLUS))
      break;
  }
  closedir(dir);

  return 0;
}

static int c00gitfs_create(const char *path_str, mode_t mode, fuse_file_info *fi)
{
  try {
    Path path(path_str);
    size_t size = 0;
    if (fi->flags & O_EXCL) {
      int fd = ::openat(root_fd_reading, path.c_str(), O_RDONLY);
      if (fd > 0) {
	::close(fd);
	if (!is_whiteout_whiteout(path))
	  throw Errno(EEXIST);
	else
	  fi->flags |= O_TRUNC;
      }
    }
    fi->flags &= ~O_EXCL;
    int fd = ::openat(root_fd_writing, path.c_str(), O_CREAT|fi->flags, mode);
    if (fd < 0)
      throw Errno();
    clear_whiteout_whiteout(path);
    fi->fh = fd;
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00gitfs_open(const char *path_str, fuse_file_info *fi)
{
  try {
    Path path(path_str);
    int rfd = root_fd_reading;
    if ((fi->flags|O_RDWR) == fi->flags ||
	(fi->flags|O_WRONLY) == fi->flags)
      rfd = root_fd_writing;
    else if (is_whiteout_whiteout(path))
      throw Errno(ENOENT);
    if (fi->flags & O_EXCL) {
      int fd = ::openat(root_fd_reading, path.c_str(), O_RDONLY);
      if (fd > 0) {
	::close(fd);
	if (!is_whiteout_whiteout(path))
	  throw Errno(EEXIST);
	else
	  fi->flags |= O_TRUNC;
      }
    }
    fi->flags &= ~O_EXCL;
    int fd = ::openat(rfd, path.c_str(), fi->flags);
    if (fd < 0)
      throw Errno();
    clear_whiteout_whiteout(path);
    fi->fh = fd;
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00gitfs_utimens(const char *path_str, const struct timespec tv[2],
			struct fuse_file_info *fi)
{
  Path path(path_str);
  if (is_symlink(path))
    return 0;
  int fd = ::utimensat(root_fd_writing, path.c_str(), tv, AT_SYMLINK_NOFOLLOW);
  if (fd < 0)
    return -errno;
  return 0;
}

static int c00gitfs_fsync(const char *path_str, int datasync,
		      struct fuse_file_info *fi)
{
  try {
    Path path(path_str);
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00gitfs_setxattr(const char *path_str, const char *name,
			 const char *value, size_t size, int flags)
{
  try {
    Path path(path_str);
    if (!strcmp(name, "syncfs.detach-for-lowering")) {
      char *str = (char *)malloc(size + 1);
      memcpy(str, value, size);
      str[size] = 0;
      int fd = open(str, O_RDWR|O_CREAT, 0666);
      fprintf (stderr, "detaching for lowering operation, token %s\n",
	       str);
      close(root_fd_reading);
      close(root_fd_writing);
      while (fd >= 0) {
	close(fd);
	sleep(1);
	fd = open(str, O_RDWR, 0666);
      }
      root_fd_reading = open(root_path_reading, O_RDONLY|O_PATH);
      if (root_fd_reading < 0)
	abort();
      root_fd_writing = open(root_path_writing, O_RDONLY|O_PATH);
      if (root_fd_writing < 0)
	abort();
      fprintf (stderr, "reattached, token %s\n",
	       str);
      return 0;
    }
    const char *prefix = "syncfs.user.";
    char *prefixed_name =
      (char *)malloc (strlen(prefix) + strlen(name) + 1);
    char *real_path =
      (char *)malloc(strlen(root_path_writing) + strlen(path.c_str()) + 1);
    sprintf(real_path, "%s%s", root_path_writing, path.c_str());
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

static int c00gitfs_removexattr(const char *path_str, const char *name)
{
  try {
    Path path(path_str);
    const char *prefix = "syncfs.user.";
    char *prefixed_name =
      (char *)malloc (strlen(prefix) + strlen(name) + 1);
    char *real_path =
      (char *)malloc(strlen(root_path_writing) + strlen(path.c_str()) + 1);
    sprintf(real_path, "%s%s", root_path_writing, path.c_str());
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

static int c00gitfs_getxattr(const char *path_str, const char *name,
			 char *value, size_t size)
{
  try {
    Path path(path_str);
    const char *prefix = "syncfs.user.";
    char *prefixed_name =
      (char *)malloc (strlen(prefix) + strlen(name) + 1);
    char *real_path =
      (char *)malloc(strlen(root_path_reading) + strlen(path.c_str()) + 1);
    sprintf(real_path, "%s%s", root_path_reading, path.c_str());
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

static int c00gitfs_listxattr(const char *path_str, char *buf, size_t size)
{
  try {
    Path path(path_str);
    const char *prefix = "syncfs.user.";
    char *real_path =
      (char *)malloc(strlen(root_path_reading) + strlen(path.c_str()) + 1);
    sprintf(real_path, "%s%s", root_path_reading, path.c_str());
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

static int c00gitfs_fsyncdir(const char *path_str, int datasync,
			 struct fuse_file_info *fi)
{
  try {
    Path path(path_str);
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00gitfs_read_buf(const char *path_str, struct fuse_bufvec **out_buf,
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

static int c00gitfs_write_buf(const char *path_str, struct fuse_bufvec *in_buf,
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

static int c00gitfs_release(const char *path_str, fuse_file_info *fi)
{
  ::close (fi->fh);
  return 0;
}

static int c00gitfs_symlink(const char *target, const char *path_str)
{
  try {
    Path path(path_str);
    int ret;
    if (::unlinkat(root_fd_writing, path.c_str(), 0) < 0 &&
	errno == EISDIR) {
      fs_delete_recursively_at(root_fd_writing, path.c_str());
    }
    clear_whiteout_whiteout(path);
    if (strcmp(target, " ")) {
      ret = ::symlinkat(target, root_fd_writing, path.c_str());
    } else {
      ret = 0;
    }
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00gitfs_link(const char *path1, const char *path_str)
{
  try {
    Path path(path_str);
    while (path1[0] == '/')
      path1++;
    int ret = ::linkat(root_fd_writing, path1,
		       AT_FDCWD, path.c_str(), 0);
    clear_whiteout_whiteout(path);
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int c00gitfs_rename(const char *path_str, const char *path2_str,
		       unsigned int flags)
{
  try {
    Path path(path_str);
    Path path2(path2_str);
    size_t size = 0;
    int ret = ::renameat2(root_fd_writing, path.c_str(),
			  root_fd_writing, path2.c_str(),
			  0);
    clear_whiteout_whiteout(path2);
    create_whiteout_whiteout(path);
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static struct fuse_operations c00gitfs_operations = {
  .getattr = c00gitfs_getattr,
  .readlink = c00gitfs_readlink,
  .mkdir = c00gitfs_mkdir,
  .unlink = c00gitfs_unlink,
  .rmdir = c00gitfs_rmdir,
  .symlink = c00gitfs_symlink,
  .rename = c00gitfs_rename,
  .link = c00gitfs_link,
  .chmod = c00gitfs_chmod,
  .chown = c00gitfs_chown,
  .open = c00gitfs_open,
  .release = c00gitfs_release,
  .fsync = c00gitfs_fsync,
  .setxattr = c00gitfs_setxattr,
  //.getxattr = syncfs_getxattr,
  //.listxattr = syncfs_listxattr,
  //.removexattr = syncfs_removexattr,
  .readdir = c00gitfs_readdir,
  .fsyncdir = c00gitfs_fsyncdir,
  .create = c00gitfs_create,
  .utimens = c00gitfs_utimens,
  .write_buf = c00gitfs_write_buf,
  .read_buf = c00gitfs_read_buf,
};

int main(int argc, char **argv)
{
  root_fd_reading = open(argv[1], O_RDONLY|O_PATH);
  if (root_fd_reading < 0)
    return 1;
  root_fd_writing = open(argv[2], O_RDONLY|O_PATH);
  if (root_fd_writing < 0)
    return 1;
  root_fd_restoring = open(argv[3], O_RDONLY|O_PATH);
  if (root_fd_restoring < 0)
    return 1;
  root_path_reading = strdup(argv[1]);
  root_path_writing = strdup(argv[2]);
  root_path_restoring = strdup(argv[3]);
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
  if (fuse_opt_add_arg(&args, "fsname=c00gitfs"))
    abort();
  //if (fuse_opt_add_arg(&args, "-odebug"))
  //  abort();
  fuse *fuse = fuse_new (&args, &c00gitfs_operations, sizeof(c00gitfs_operations),
			 nullptr);
  fuse_mount(fuse, argv[4]);

  fuse_loop(fuse);
  //fuse_set_signal_handlers(session);

  umask(0);

  //if (fuse_session_mount(session, argv[2]))
  //abort();

  //fuse_remove_signal_handlers(session);
  //fuse_session_destroy(session);

  return 0;
}
