/* Rewrite. Doesn't use GPL2-only code. */
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
    if (newstr[0] && newstr[1]) {
      for (char *p = newstr + 2; *p; p++)
	{
	  if (*p == '/' && p[1] == '.')
	    p[1] = ',';
	}
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

class FIFO {
public:
  int infd;
  int outfd;
  FIFO(int infd, int outfd) : infd (infd), outfd (outfd) {}
  ~FIFO() { close (infd); close (outfd); }

  void request(std::string type, Path path, size_t size = 0)
  {
    fuse_context *context = fuse_get_context();
    Json::StreamWriterBuilder swb;
    swb["indentation"] = "";
    Json::StreamWriter *swriter = swb.newStreamWriter();
    Json::Value val(Json::objectValue);
    val["command"] = Json::Value(type.c_str());
    val["path"] = Json::Value(path.c_str());
    val["size"] = Json::Value(Json::UInt64(size));
    val["pid"] = Json::Value(Json::UInt64(context->pid));
    val["uid"] = Json::Value(Json::UInt64(context->uid));
    val["gid"] = Json::Value(Json::UInt64(context->gid));
    std::ostringstream os;
    swriter->write(val, &os);
    write (infd, os.str().c_str(), strlen(os.str().c_str()));
    write (infd, "\n", 1);
    char buf[4096];
    ssize_t len = read (outfd, buf, 1);
    if (len > 0 && buf[0] == '\n')
      return;

    throw Errno(EIO);
  }
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

static int fd_from_inode(fuse_ino_t ino)
{
  if (ino == 1)
    return root_fd;
  return (int) ino;
}

static int fuse_getattr(const char *path_str,
			struct stat *statp,
			fuse_file_info* fi_may_be_null)
{
  try {
    Path path(path_str);
    FIFO *fifo = (FIFO *)fuse_get_context()->private_data;
    int ret = fstatat(root_fd, path.c_str(), statp, AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW);
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int fuse_readlink(const char *path_str, char *buf, size_t size)
{
  try {
    Path path(path_str);
    int ret = ::readlinkat(root_fd, path.c_str(), buf, size);
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int fuse_mkdir(const char *path_str, mode_t mode)
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

static int fuse_rmdir(const char *path_str)
{
  try {
    Path path(path_str);
    if (::unlinkat(root_fd, path.c_str(), AT_REMOVEDIR) < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int fuse_unlink(const char *path_str)
{
  try {
    Path path(path_str);
    FIFO *fifo = (FIFO *)fuse_get_context()->private_data;
    size_t size = 0;
    fifo->request("pre-unlink", path, size);
    if (::unlinkat(root_fd, path.c_str(), 0) < 0)
      throw Errno();
    fifo->request("post-unlink", path, size);
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
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

static int fuse_chmod(const char *path_str, mode_t mode,
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

static int fuse_chown(const char *path_str, uid_t uid, gid_t gid,
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

static int fuse_readdir(const char *path_str, void *buf, fuse_fill_dir_t filler,
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

static int fuse_create(const char *path_str, mode_t mode, fuse_file_info *fi)
{
  try {
    Path path(path_str);
    FIFO *fifo = (FIFO *)fuse_get_context()->private_data;
    size_t size = 0;
    fifo->request("pre-create", path, size);
    int fd = ::openat(root_fd, path.c_str(), O_CREAT|fi->flags, mode);
    if (fd < 0)
      throw Errno();
    fi->fh = fd;
    fifo->request("post-create", path, size);
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int fuse_open(const char *path_str, fuse_file_info *fi)
{
    Path path(path_str);
  int fd = ::openat(root_fd, path.c_str(), fi->flags);
  if (fd < 0)
    return -errno;
  fi->fh = fd;
  return 0;
}

static int fuse_utimens(const char *path_str, const struct timespec tv[2],
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

static int fuse_fsync(const char *path_str, int datasync,
		      struct fuse_file_info *fi)
{
  try {
    Path path(path_str);
    FIFO *fifo = (FIFO *)fuse_get_context()->private_data;
    fifo->request("sync", path, 0);
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int fuse_fsyncdir(const char *path_str, int datasync,
			 struct fuse_file_info *fi)
{
  try {
    Path path(path_str);
    FIFO *fifo = (FIFO *)fuse_get_context()->private_data;
    fifo->request("sync", path, 0);
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int fuse_read_buf(const char *path_str, struct fuse_bufvec **out_buf,
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

static int fuse_write_buf(const char *path_str, struct fuse_bufvec *in_buf,
			  off_t off, fuse_file_info *fi)
{
  try {
    Path path(path_str);
    FIFO *fifo = (FIFO *)fuse_get_context()->private_data;
    size_t size = fuse_buf_size(in_buf);
    fifo->request("pre-write", path, size);
    int fd = fi->fh;
    fuse_bufvec buf = FUSE_BUFVEC_INIT(size);
    buf.buf[0].flags = static_cast<fuse_buf_flags>(FUSE_BUF_IS_FD|FUSE_BUF_FD_SEEK);
    buf.buf[0].fd = fd;
    buf.buf[0].pos = off;
    ssize_t res = fuse_buf_copy(&buf, in_buf, fuse_buf_copy_flags());
    if (res < 0)
      throw Errno(res);
    fifo->request("post-write", path, size);
    return res;
  } catch (Errno error) {
    return -error.error;
  }
}

static int fuse_release(const char *path_str, fuse_file_info *fi)
{
  ::close (fi->fh);
  return 0;
}

static int fuse_symlink(const char *path1, const char *path_str)
{
  try {
    Path path(path_str);
    int ret = ::symlinkat(path1, root_fd, path.c_str());
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static int fuse_link(const char *path1, const char *path_str)
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

static int fuse_rename(const char *path_str, const char *path2_str,
		       unsigned int flags)
{
  try {
    Path path(path_str);
    Path path2(path2_str);
    FIFO *fifo = (FIFO *)fuse_get_context()->private_data;
    size_t size = 0;
    fifo->request("pre-rename", path, strlen(path.c_str()));
    int ret = ::renameat2(root_fd, path.c_str(),
			  root_fd, path2.c_str(),
			  0);
    fifo->request("post-rename", path2, strlen(path2.c_str()));
    if (ret < 0)
      throw Errno();
    return 0;
  } catch (Errno error) {
    return -error.error;
  }
}

static struct fuse_operations fuse_operations = {
  .getattr = fuse_getattr,
  .readlink = fuse_readlink,
  .mkdir = fuse_mkdir,
  .unlink = fuse_unlink,
  .rmdir = fuse_rmdir,
  .symlink = fuse_symlink,
  .rename = fuse_rename,
  .link = fuse_link,
  .chmod = fuse_chmod,
  .chown = fuse_chown,
  .open = fuse_open,
  .fsync = fuse_fsync,
  .readdir = fuse_readdir,
  .fsyncdir = fuse_fsyncdir,
  .create = fuse_create,
  .utimens = fuse_utimens,
  .write_buf = fuse_write_buf,
  .read_buf = fuse_read_buf,
};

int main(int argc, char **argv)
{
  const char *fifo_in_name = strdup(argv[3]);
  int fifo_in_fd = open(fifo_in_name, O_WRONLY);
  const char *fifo_out_name = strdup(argv[4]);
  int fifo_out_fd = open(fifo_out_name, O_RDONLY);
  FIFO *fifo = new FIFO(fifo_in_fd, fifo_out_fd);
  int lower_fd = open(argv[1], O_RDONLY|O_PATH);
  if (lower_fd < 0)
    abort();
  root_fd = lower_fd;
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
  if (fuse_opt_add_arg(&args, "fsname=syncfs"))
    abort();
  //if (fuse_opt_add_arg(&args, "-odebug"))
  //  abort();
  fuse *fuse = fuse_new (&args, &fuse_operations, sizeof(fuse_operations),
			 fifo);
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
