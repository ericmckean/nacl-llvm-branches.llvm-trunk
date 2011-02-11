/* Copyright 2010 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can
 * be found in the LICENSE file.

 * This file provides wrappers to lseek(2), read(2), etc. that read bytes from
 * an mmap()'ed buffer.  There are three steps required:
 *    1. Use linker aliasing to wrap lseek(), etc.  This is done in the
 *       Makefile using the "-XLinker --wrap -Xlinker lseek" arguments to
 *       nacl-gcc.  Note that this makes *all* calls to things like read() go
 *       through these wrappers, so if you also need to read() from, say, a
 *       socket, this code will not work as-is.
 *    2. Use lseek(), read() etc as you normally would for a file.
 *
 * Note: This code is very temporary and will disappear when the Pepper 2 API
 * is available in Native Client.
 */

#if defined(__native_client__) && defined(NACL_SRPC)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/nacl_syscalls.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <nacl/nacl_srpc.h>

#define MAX_NACL_FILES 256
#define MMAP_PAGE_SIZE 64 * 1024
#define MMAP_ROUND_MASK (MMAP_PAGE_SIZE - 1)

#define printerr(...)                           \
  fprintf(stderr, __VA_ARGS__)

extern "C" int __real_open(const char *pathname, int oflags, int mode);
extern "C" int __wrap_open(const char *pathname, int oflags, int mode);
extern "C" int __real_close(int dd);
extern "C" int __wrap_close(int dd);
extern "C" int __real_read(int dd, void *, size_t);
extern "C" int __wrap_read(int dd, void *, size_t);
extern "C" int __real_write(int dd, const void *, size_t);
extern "C" int __wrap_write(int dd, const void *, size_t);
extern "C" off_t __real_lseek(int dd, off_t offset, int whence);
extern "C" off_t __wrap_lseek(int dd, off_t offset, int whence);
extern int llc_main(int argc, char **argv);

static int nacl_file_initialized = 0;

struct NaCl_file_map {
  char *filename;
  int real_fd;
  int is_reg_file;    /* We use shm for output, and UrlAsNaClDesc supplies
                      * regular files. Differentiate between the two. */
  pthread_mutex_t mu;
  size_t size;        /* Bytes mmap'ed for this file. */
  size_t real_size;   /* Bytes actually written, if it is a shm. */
  struct NaCl_file_map *next;
};

struct NaCl_file_map *nacl_fs = NULL;
static pthread_mutex_t nacl_fs_mu = PTHREAD_MUTEX_INITIALIZER;

struct NaCl_file {
  int mode;
  off_t pos;
  pthread_mutex_t mu;
  struct NaCl_file_map *file_ptr;
};

static struct NaCl_file nacl_files[MAX_NACL_FILES];

/* Check to see the |dd| is a valid NaCl shm file descriptor */
static int IsValidDescriptor(int dd) {
  return nacl_file_initialized && (dd >= 3) && (dd < MAX_NACL_FILES);
}

static size_t roundToNextPageSize(size_t size) {
  size_t count_up = size + (MMAP_ROUND_MASK);
  return (count_up & ~(MMAP_ROUND_MASK));
}

/* Create a new entry representing the shm file descriptor.
 * If real_size_opt is supplied, this means that we know
 * the real size of the shm file. Otherwise, we need to count on fstat.
 * Counting on fstat is fine if fd corresponds to a regular file.
 * Returns 0 on success. */
int NaClFile_fd(char *pathname, int fd,
                int has_real_size, size_t real_size_opt) {
  int i;
  int is_reg;
  struct stat stb;
  struct NaCl_file_map *entry;

  if (0 != fstat(fd, &stb)) {
    errno = EBADF;
    return -1;
  }

  // NOTE: We do not have the S_ISSHM macro in our headers, only S_ISREG.
  mode_t fmt = stb.st_mode & S_IFMT;
  if (S_IFREG == fmt) {
    is_reg = 1;
  } else if (S_IFSHM == fmt) {
    is_reg = 0;
  } else {
    printerr("nacl_file: %d non-shm and non-reg file?!\n", fd);
    return -1;
  }

  entry = (struct NaCl_file_map*)(malloc(sizeof *entry));

  if (NULL == entry) {
    printerr("nacl_file: No memory for file map for %s\n", pathname);
    exit(1);
  }
  if (NULL == (entry->filename = strdup(pathname))) {
    printerr("nacl_file: No memory for file path %s\n", pathname);
    exit(1);
  }
  entry->real_fd = fd;
  if (has_real_size) {
    entry->size = real_size_opt;
    entry->real_size = real_size_opt;
  } else {
    entry->size = stb.st_size;
    entry->real_size = stb.st_size;
  }
  entry->is_reg_file = is_reg;

  pthread_mutex_init(&(entry->mu), NULL);

  pthread_mutex_lock(&nacl_fs_mu);

  entry->next = nacl_fs;
  nacl_fs = entry;

  if (!nacl_file_initialized) {
    for (i = 0; i < MAX_NACL_FILES; ++i) {
      pthread_mutex_init(&nacl_files[i].mu, NULL);
      nacl_files[i].file_ptr = NULL;
    }
    nacl_file_initialized = 1;
  }

  pthread_mutex_unlock(&nacl_fs_mu);

  return 0;
}

/* Create a new file and return the fd for it.
   Returns 0 on success. */
int NaClFile_new(char *pathname) {
  int fd = imc_mem_obj_create(MMAP_PAGE_SIZE);
  if (fd < 0) {
    printerr("nacl_file: imc_mem_obj_create failed %d\n", fd);
    return -1;
  }
  return NaClFile_fd(pathname, fd, 0, 0);
}

int get_real_fd(int dd) {
  int fd;

  if (!IsValidDescriptor(dd)) {
    errno = EBADF;
    return dd;
  }

  pthread_mutex_lock(&nacl_files[dd].mu);

  if (NULL == nacl_files[dd].file_ptr) {
    pthread_mutex_unlock(&nacl_files[dd].mu);
    errno = EBADF;
    return dd;
  }

  pthread_mutex_lock(&nacl_files[dd].file_ptr->mu);

  fd = nacl_files[dd].file_ptr->real_fd;

  pthread_mutex_unlock(&nacl_files[dd].file_ptr->mu);
  pthread_mutex_unlock(&nacl_files[dd].mu);

  return fd;
}

/* NOTE: this is very similar to get_real_fd(). TODO(robertm): refactor. */
int is_reg_file(int dd) {
  int is_reg = 0;

  if (!IsValidDescriptor(dd)) {
    errno = EBADF;
    return -1;
  }

  pthread_mutex_lock(&nacl_files[dd].mu);

  if (NULL == nacl_files[dd].file_ptr) {
    pthread_mutex_unlock(&nacl_files[dd].mu);
    errno = EBADF;
    return -1;
  }

  pthread_mutex_lock(&nacl_files[dd].file_ptr->mu);

  is_reg = nacl_files[dd].file_ptr->is_reg_file;

  pthread_mutex_unlock(&nacl_files[dd].file_ptr->mu);
  pthread_mutex_unlock(&nacl_files[dd].mu);

  return is_reg;
}


int get_real_fd_by_name(const char *pathname) {
  int fd = -1;
  struct NaCl_file_map *entry;

  pthread_mutex_lock(&nacl_fs_mu);

  for (entry = nacl_fs; NULL != entry; entry = entry->next) {
    if (!strcmp(pathname, entry->filename)) {
      fd = entry->real_fd;
      break;
    }
  }

  pthread_mutex_unlock(&nacl_fs_mu);

  if (-1 == fd) {
    errno = EBADF;
  }

  return fd;
}

/* NOTE: this is very similar to get_real_fd_by_name. TODO(robertm): refactor. */
int get_real_size_by_name(const char *pathname) {
  size_t real_size = 0;
  struct NaCl_file_map *entry;

  pthread_mutex_lock(&nacl_fs_mu);

  for (entry = nacl_fs; NULL != entry; entry = entry->next) {
    if (!strcmp(pathname, entry->filename)) {
      real_size = entry->real_size;
      break;
    }
  }

  pthread_mutex_unlock(&nacl_fs_mu);

  return real_size;
}


/* Copy at most a page of data between shm (from_fd to to_fd), starting at
   base_pos for count bytes. */
static int copy_shm_data(int from_fd, int to_fd, off_t base_pos, size_t count) {
  void *from_data = mmap(NULL, MMAP_PAGE_SIZE, PROT_READ, MAP_SHARED,
                         from_fd, base_pos);
  void *to_data = mmap(NULL, MMAP_PAGE_SIZE, PROT_WRITE, MAP_SHARED,
                       to_fd, base_pos);

  if (count > MMAP_PAGE_SIZE) {
    printerr("nacl_file: copy more than MMAP_PAGE_SIZE: %d?\n", count);
    return -1;
  }

  if (NULL != from_data && NULL != to_data) {
    memcpy(to_data, from_data, count);
    munmap(from_data, MMAP_PAGE_SIZE);
    munmap(to_data, MMAP_PAGE_SIZE);
  } else {
    printerr("nacl_file: mmap call failed!\n");
    return -1;
  }
  return 0;
}

/* Adjust the size of a nacl file.
   Changes the real_fd of a file.
   Returns 0 on success. */
static int
adjust_file_size(int dd, size_t new_size) {
  int new_fd = -1;
  off_t base_pos;
  size_t count;
  size_t final_base;
  struct NaCl_file_map *entry;

  if (!IsValidDescriptor(dd)) {
    errno = EBADF;
    return -1;
  }

  /* TODO(abetul): check if caller has already acquired the mutex for file */

  if (NULL == nacl_files[dd].file_ptr) {
    errno = EBADF;
    return -1;
  }
  entry = nacl_files[dd].file_ptr;

  new_fd = imc_mem_obj_create(new_size);
  if (new_fd < 0) {
    printerr("nacl_file: imc_mem_obj_create failed %d\n", new_fd);
    return -1;
  }

  /* copy contents over -- Beginning with MMAP_PAGE_SIZE chunks. */
  final_base = entry->size & (~MMAP_ROUND_MASK);
  for (base_pos = 0; (size_t) base_pos < final_base;
       base_pos += MMAP_PAGE_SIZE) {
    if (copy_shm_data(entry->real_fd, new_fd, base_pos, MMAP_PAGE_SIZE)) {
      printerr("nacl_file: copy_shm_data failed!\n");
      return -1;
    }
  }

  /* Copy the left overs (not a multiple of MMAP_PAGE_SIZE) */
  count = entry->size - final_base;
  if (count > 0) {
    if (copy_shm_data(entry->real_fd, new_fd, base_pos, count)) {
      printerr("nacl_file: copy_shm_data failed!\n");
      return -1;
    }
  }

  if (__real_close(entry->real_fd) < 0) {
    printerr("nacl_file: close in size adjust failed!\n");
    return -1;
  }

  entry->real_fd = new_fd;
  entry->size = new_size;
  /* entry->real_size stays the same, since we haven't written anything new. */

  return 0;
}

int __wrap_open(const char *pathname, int oflags, int mode) {
  int dd = -1;
  int i;
  struct NaCl_file_map *entry;

  for (entry = nacl_fs; NULL != entry; entry = entry->next) {
    if (!strcmp(pathname, entry->filename)) {
      break;
    }
  }

  if (NULL == entry) {
    return __real_open(pathname, oflags, mode);
  }

  for (i = 3; i < MAX_NACL_FILES; i++) {
    if (NULL == nacl_files[i].file_ptr) {
      dd = i;
      break;
    }
  }

  if (-1 == dd) {
    printerr("nacl_file: Max open file count has been reached\n");
    return -1;
  }

  pthread_mutex_lock(&nacl_files[dd].mu);

  nacl_files[dd].pos = 0;
  nacl_files[dd].mode = oflags;
  nacl_files[dd].file_ptr = entry;

  pthread_mutex_unlock(&nacl_files[dd].mu);

  return dd;
}

int __wrap_close(int dd) {

  if (!IsValidDescriptor(dd)) {
    return __real_close(dd);
  }

  pthread_mutex_lock(&nacl_files[dd].mu);

  if (NULL == nacl_files[dd].file_ptr) {
    pthread_mutex_unlock(&nacl_files[dd].mu);
    return __real_close(dd);
  }

  nacl_files[dd].file_ptr = NULL;

  pthread_mutex_unlock(&nacl_files[dd].mu);

  return 0;
}

int __wrap_read(int dd, void *buf, size_t count) {
  int got = 0;
  uint8_t *data;
  off_t base_pos;
  off_t adj;
  size_t count_up;
  struct stat stb;
  mode_t fmt;
  int fd;

  if (!IsValidDescriptor(dd)) {
    return __real_read(dd, buf, count);
  }

  /* Be careful w/ the test... if it !IsValidDescriptor it could return -1 */
  if (1 == is_reg_file(dd)) {
    fd = get_real_fd(dd);
    return __real_read(fd, buf, count);
  }

  pthread_mutex_lock(&nacl_files[dd].mu);

  if (NULL == nacl_files[dd].file_ptr) {
    pthread_mutex_unlock(&nacl_files[dd].mu);
    return __real_read(dd, buf, count);
  }

  if ((nacl_files[dd].mode & !O_RDONLY) != O_RDONLY) {
    printerr("nacl_file: invalid mode %d\n", nacl_files[dd].mode);
    pthread_mutex_unlock(&nacl_files[dd].mu);
    return -1;
  }

  pthread_mutex_lock(&nacl_files[dd].file_ptr->mu);

  /* make sure we don't read beyond end of file */
  if ((nacl_files[dd].pos + count) > nacl_files[dd].file_ptr->size) {
    if ((nacl_files[dd].file_ptr->size - nacl_files[dd].pos) < 0)
      count = 0;
    else
      count = nacl_files[dd].file_ptr->size - nacl_files[dd].pos;
    printerr("nacl_file: warning, attempting read outside of file!\n");
  }

  /* use mmap to read data */
  base_pos = nacl_files[dd].pos & (~(MMAP_ROUND_MASK));
  adj = nacl_files[dd].pos - base_pos;
  /* round count value to next 64KB */
  count_up = roundToNextPageSize(count + adj);
  data = (uint8_t *) mmap(NULL, count_up, PROT_READ, MAP_SHARED,
                         nacl_files[dd].file_ptr->real_fd, base_pos);
  if (NULL != data) {
    memcpy(buf, data + adj, count);
    munmap(data, count_up);
    got = count;
  } else {
    printerr("nacl_file: mmap call failed!\n");
  }

  if (got > 0) {
    nacl_files[dd].pos += got;
  }

  pthread_mutex_unlock(&nacl_files[dd].file_ptr->mu);
  pthread_mutex_unlock(&nacl_files[dd].mu);

  return got;
}

/* Update the file position after a write */
static void nacl_file_update_pos(struct NaCl_file *nf, off_t pos) {
  nf->pos = pos;
  /* Update the real_size of the file, if we've written further in */
  if (nf->file_ptr) {
    nf->file_ptr->real_size = pos > nf->file_ptr->real_size ?
        pos : nf->file_ptr->real_size;
  }
}


int __wrap_write(int dd, const void *buf, size_t count) {
  int got = 0;
  uint8_t *data;
  off_t base_pos;
  off_t adj;
  size_t count_up;
  size_t new_size;

  if (!IsValidDescriptor(dd)) {
    return __real_write(dd, buf, count);
  }

  pthread_mutex_lock(&nacl_files[dd].mu);

  if (NULL == nacl_files[dd].file_ptr) {
    pthread_mutex_unlock(&nacl_files[dd].mu);
    return __real_write(dd, buf, count);
  }

  if ((nacl_files[dd].mode & (O_WRONLY | O_RDWR)) == 0) {
    printerr("nacl_file: invalid mode %d\n", nacl_files[dd].mode);
    pthread_mutex_unlock(&nacl_files[dd].mu);
    return -1;
  }

  pthread_mutex_lock(&nacl_files[dd].file_ptr->mu);

  /* adjust file size if writing past the current end */
  new_size = nacl_files[dd].file_ptr->size;
  while ((nacl_files[dd].pos + count) > new_size) {
    /* double the file size */
    new_size <<= 1;
  }

  if (new_size > nacl_files[dd].file_ptr->size) {
    if (adjust_file_size(dd, new_size) != 0) {
      pthread_mutex_unlock(&nacl_files[dd].file_ptr->mu);
      pthread_mutex_unlock(&nacl_files[dd].mu);
      printerr("nacl_file: failed to adjust file size %d\n", dd);
      return -1;
    }
  }

  /* use mmap to write data */
  base_pos = nacl_files[dd].pos & (~(MMAP_ROUND_MASK));
  adj = nacl_files[dd].pos - base_pos;
  /* round count value to next 64KB */
  count_up = roundToNextPageSize(count + adj);
  data = (uint8_t *) mmap(NULL, count_up, PROT_WRITE, MAP_SHARED,
                         nacl_files[dd].file_ptr->real_fd, base_pos);
  if (NULL != data) {
    memcpy(data + adj, buf, count);
    munmap(data, count_up);
    got = count;
  } else {
    printerr("nacl_file: mmap call failed!\n");
  }

  if (got > 0) {
    nacl_file_update_pos(&nacl_files[dd], nacl_files[dd].pos + got);
  }

  pthread_mutex_unlock(&nacl_files[dd].file_ptr->mu);
  pthread_mutex_unlock(&nacl_files[dd].mu);
  return got;
}

off_t __wrap_lseek(int dd, off_t offset, int whence) {
  int fd;

  if (!IsValidDescriptor(dd)) {
    return __real_lseek(dd, offset, whence);
  }

  /* Be careful w/ the test... if it !IsValidDescriptor it could return -1 */
  if (1 == is_reg_file(dd)) {
    fd = get_real_fd(dd);
    return __real_lseek(fd, offset, whence);
  }

  pthread_mutex_lock(&nacl_files[dd].mu);

  if (NULL == nacl_files[dd].file_ptr) {
    pthread_mutex_unlock(&nacl_files[dd].mu);
    return __real_lseek(dd, offset, whence);
  }

  pthread_mutex_lock(&nacl_files[dd].file_ptr->mu);

  switch (whence) {
    case SEEK_SET:
      break;
    case SEEK_CUR:
      offset = nacl_files[dd].pos + offset;
      break;
    case SEEK_END:
      offset = nacl_files[dd].file_ptr->size + offset;
      break;
  }
  if (offset < 0) {
    offset = -1;
  }
  if (-1 != offset) {
    nacl_files[dd].pos = offset;
  }

  pthread_mutex_unlock(&nacl_files[dd].file_ptr->mu);
  pthread_mutex_unlock(&nacl_files[dd].mu);

  return offset;
}

void
translate(NaClSrpcRpc *rpc,
          NaClSrpcArg **in_args,
          NaClSrpcArg **out_args,
          NaClSrpcClosure *done) {
  /* TODO(robertm): receive command line arguments from SRPC.
     That way, we can supply x86-32 params and ARM params as well. */
  char *argv[] = {"llc", "-march=x86-64", "-mcpu=core2",
                  "-asm-verbose=false", "-filetype=obj",
                  "bitcode_combined", "-o", "obj_combined"};
  int kArgvLength = sizeof argv / sizeof argv[0];
  /* Input bitcode file.
   * Supplied by urlAsNaClDesc, which should get the right
   * size from fstat(). */
  int bitcode_fd = in_args[0]->u.hval;
  NaClFile_fd("bitcode_combined", bitcode_fd, 0, 0);

  /* Define output file. */
  NaClFile_new("obj_combined");

  /* Call main. */
  llc_main(kArgvLength, argv);

  /* Save obj fd for return. */
  out_args[0]->u.hval = get_real_fd_by_name("obj_combined");
  out_args[1]->u.ival = get_real_size_by_name("obj_combined");

  rpc->result = NACL_SRPC_RESULT_OK;
  done->Run(done);
}

const struct NaClSrpcHandlerDesc srpc_methods[] = {
  { "Translate:h:hi", translate },
  { NULL, NULL },
};

int
main() {
  if (!NaClSrpcModuleInit()) {
    return 1;
  }
  if (!NaClSrpcAcceptClientConnection(srpc_methods)) {
    return 1;
  }
  NaClSrpcModuleFini();
  return 0;
}

#endif
