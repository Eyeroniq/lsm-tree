/*
 * Power-loss simulator for the LSM engine (Linux only).
 *
 * Built as an LD_PRELOAD library, it wraps fsync() for files inside
 * POWERCUT_ROOT and records, in POWERCUT_SHADOW, exactly what a power cut
 * would leave behind under strict POSIX rules:
 *
 *   - fsync(file)       -> that file's current contents become durable;
 *   - fsync(directory)  -> the directory's current names become durable.
 *
 * Nothing else is durable: writes that were never fsynced, and files whose
 * names were never made durable by a directory fsync, are lost. After the
 * writer is killed, the harness rebuilds the database directory from the
 * shadow alone, which is the state a machine would reboot into.
 *
 * File contents are keyed by inode number plus birth time, so an inode
 * number reused by a new file is never confused with the old file.
 *
 * Built with -DPOWERCUT_MAIN, the same file is a helper that records the
 * current directory as fully durable: `powercut_snapshot ROOT SHADOW`.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void die(const char* what, const char* path) {
  fprintf(stderr, "powercut: %s %s: %s\n", what, path, strerror(errno));
  abort();
}

/* Identity of a file: inode plus birth time when the filesystem reports it. */
static int file_key(int dirfd, const char* path, int flags, char* out,
                    size_t size) {
  struct statx info;
  if (statx(dirfd, path, flags, STATX_INO | STATX_BTIME, &info) != 0) {
    return -1;
  }
  if (info.stx_mask & STATX_BTIME) {
    snprintf(out, size, "%llu-%lld-%u", (unsigned long long)info.stx_ino,
             (long long)info.stx_btime.tv_sec, info.stx_btime.tv_nsec);
  } else {
    snprintf(out, size, "%llu", (unsigned long long)info.stx_ino);
  }
  return 0;
}

/* Copies everything readable from `source` into `dest`, atomically. */
static void copy_to(int source, const char* dest) {
  char temporary[PATH_MAX];
  snprintf(temporary, sizeof temporary, "%s.tmp", dest);
  const int out = open(temporary, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
  if (out < 0) {
    die("create", temporary);
  }
  char buffer[65536];
  off_t offset = 0;
  for (;;) {
    const ssize_t count = pread(source, buffer, sizeof buffer, offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      die("read for", dest);
    }
    if (count == 0) {
      break;
    }
    ssize_t written = 0;
    while (written < count) {
      const ssize_t step = write(out, buffer + written, (size_t)(count - written));
      if (step < 0) {
        die("write", temporary);
      }
      written += step;
    }
    offset += count;
  }
  if (close(out) != 0 || rename(temporary, dest) != 0) {
    die("install", dest);
  }
}

/* Records the open file `fd` as durable with its current contents. */
static void record_contents(int fd, const char* shadow) {
  char key[128];
  if (file_key(fd, "", AT_EMPTY_PATH, key, sizeof key) != 0) {
    die("statx", "file");
  }
  /* Reopen through /proc: works for write-only and already-deleted files. */
  char link[64];
  snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
  const int source = open(link, O_RDONLY | O_CLOEXEC);
  if (source < 0) {
    die("reopen", link);
  }
  char dest[PATH_MAX];
  snprintf(dest, sizeof dest, "%s/inodes/%s", shadow, key);
  copy_to(source, dest);
  close(source);
}

/* Records the names currently in `root` as durable. */
static void record_names(const char* root, const char* shadow) {
  DIR* dir = opendir(root);
  if (dir == NULL) {
    die("opendir", root);
  }
  char temporary[PATH_MAX];
  char names[PATH_MAX];
  snprintf(temporary, sizeof temporary, "%s/names.tmp", shadow);
  snprintf(names, sizeof names, "%s/names", shadow);
  FILE* out = fopen(temporary, "we");
  if (out == NULL) {
    die("create", temporary);
  }
  const struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    struct stat info;
    if (fstatat(dirfd(dir), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(info.st_mode)) {
      continue;
    }
    char key[128];
    if (file_key(dirfd(dir), entry->d_name, AT_SYMLINK_NOFOLLOW, key,
                 sizeof key) == 0) {
      fprintf(out, "%s\t%s\n", entry->d_name, key);
    }
  }
  closedir(dir);
  if (fclose(out) != 0 || rename(temporary, names) != 0) {
    die("install", names);
  }
}

#ifndef POWERCUT_MAIN

int fsync(int fd) {
  static int (*real_fsync)(int) = NULL;
  if (real_fsync == NULL) {
    real_fsync = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
  }
  const int result = real_fsync(fd);
  const char* root = getenv("POWERCUT_ROOT");
  const char* shadow = getenv("POWERCUT_SHADOW");
  if (result != 0 || root == NULL || shadow == NULL) {
    return result;
  }
  /* The engine fsyncs from several threads (writers rotating the WAL, the
   * background thread flushing), so recording is serialized: two threads
   * must never share the shadow's temporary files. */
  static pthread_mutex_t recording = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&recording);
  const int saved = errno;
  char link[64];
  char path[PATH_MAX];
  snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
  const ssize_t length = readlink(link, path, sizeof path - 1);
  struct stat info;
  int recorded = 0;
  if (length > 0 && fstat(fd, &info) == 0) {
    path[length] = '\0';
    const size_t root_length = strlen(root);
    if (S_ISDIR(info.st_mode) && strcmp(path, root) == 0) {
      record_names(root, shadow);
      recorded = 1;
    } else if (S_ISREG(info.st_mode) && strncmp(path, root, root_length) == 0 &&
               path[root_length] == '/') {
      record_contents(fd, shadow);
      recorded = 1;
    }
  }
  /* POWERCUT_AFTER_FSYNCS=N cuts power the instant the Nth durable step
   * completes, so every step is hit directly instead of by timing luck.
   * POWERCUT_KIND narrows the count to one kind of step -- "dir", "wal",
   * "table" or "manifest" -- so rare but critical steps such as a
   * compaction's MANIFEST commit are reached often. */
  static long completed = 0;
  const char* after = getenv("POWERCUT_AFTER_FSYNCS");
  if (recorded && after != NULL) {
    const char* wanted = getenv("POWERCUT_KIND");
    const char* name = strrchr(path, '/');
    name = name == NULL ? path : name + 1;
    const char* kind = S_ISDIR(info.st_mode)                 ? "dir"
                       : strncmp(name, "MANIFEST", 8) == 0   ? "manifest"
                       : strncmp(name, "sst-", 4) == 0       ? "table"
                                                             : "wal";
    if ((wanted == NULL || strcmp(wanted, "any") == 0 ||
         strcmp(wanted, kind) == 0) &&
        ++completed == atol(after)) {
      kill(getpid(), SIGKILL);
    }
  }
  pthread_mutex_unlock(&recording);
  errno = saved;
  return result;
}

#else

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: powercut_snapshot ROOT SHADOW\n");
    return 2;
  }
  const char* root = argv[1];
  const char* shadow = argv[2];
  char inodes[PATH_MAX];
  snprintf(inodes, sizeof inodes, "%s/inodes", shadow);
  mkdir(shadow, 0755);
  mkdir(inodes, 0755);
  /* Forget contents from earlier rounds, then record every file. */
  DIR* old = opendir(inodes);
  if (old == NULL) {
    die("opendir", inodes);
  }
  const struct dirent* entry;
  while ((entry = readdir(old)) != NULL) {
    if (entry->d_name[0] != '.') {
      unlinkat(dirfd(old), entry->d_name, 0);
    }
  }
  closedir(old);
  DIR* dir = opendir(root);
  if (dir == NULL) {
    die("opendir", root);
  }
  while ((entry = readdir(dir)) != NULL) {
    struct stat info;
    if (fstatat(dirfd(dir), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(info.st_mode)) {
      continue;
    }
    const int fd = openat(dirfd(dir), entry->d_name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      die("open", entry->d_name);
    }
    record_contents(fd, shadow);
    close(fd);
  }
  closedir(dir);
  record_names(root, shadow);
  return 0;
}

#endif
