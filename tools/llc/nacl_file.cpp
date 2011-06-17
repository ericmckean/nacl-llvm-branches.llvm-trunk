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

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/nacl_syscalls.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <nacl/nacl_srpc.h>


#include <string>
#include <map>

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/ADT/StringRef.h"

using llvm::MemoryBuffer;
using llvm::StringRef;
using std::string;
using std::map;

#define MMAP_PAGE_SIZE 64 * 1024
#define MMAP_ROUND_MASK (MMAP_PAGE_SIZE - 1)
#define printerr(...)  fprintf(stderr, __VA_ARGS__)
#define printdbg(...)


static size_t roundToNextPageSize(size_t size) {
  size_t count_up = size + (MMAP_ROUND_MASK);
  return (count_up & ~(MMAP_ROUND_MASK));
}

class FileInfo {
 private:
  static map<string, FileInfo*> descriptor_map_;

  string filename_;
  int fd_;
  int size_;

 public:
  FileInfo(string fn, int fd) :
    filename_(fn), fd_(fd), size_(-1) {
    printdbg("DBG: registering file %d (%s)\n", fd, fn.c_str());
    descriptor_map_[fn] = this;
    if (fd >= 0) {
      struct stat stb;
      int result = fstat(fd_, &stb);
      if (result != 0) {
        printerr("ERROR: cannot stat %d (%s)\n", fd, fn.c_str());
      }
      size_ = stb.st_size;;
    }
  }

  int GetSize() {
    if (fd_ < 0) {
      printerr("ERROR: file has not been initialized!\n");
    }
    return size_;
  }

  int GetFd() {
    return fd_;
  }

  MemoryBuffer* ReadAllDataAsMemoryBuffer() {
    printdbg("DBG: reading file %d (%s): %d bytes\n",
             fd_, filename_.c_str(), size_);

    const int count_up = roundToNextPageSize(size_);
    char *buf = (char *) mmap(NULL,
                              count_up,
                              PROT_READ,
                              MAP_SHARED,
                              fd_, 0);
    if (NULL == buf) {
      printerr("ERROR: mmap call failed!\n");
      return 0;
    }

    printdbg("after mapping %p %d\n", buf, size_);
    // This copies the data into a new buffer
    MemoryBuffer* mb = MemoryBuffer::getMemBufferCopy(StringRef(buf, size_));
    munmap(buf, count_up);
    printdbg("after unmapping %p %d\n",
             mb->getBufferStart(), mb->getBufferSize());
    return mb;
  }

  void WriteAllData(string data) {
    printdbg("DBG: writing file %d (%s): %d bytes\n",
             fd_, filename_.c_str(), data.size());

    if (fd_ >= 0) {
      printerr("ERROR: cannot write file twice\n");
      return;
    }

    const int count_up =  roundToNextPageSize(data.size());
    const int fd = imc_mem_obj_create(count_up);
    if (fd < 0) {
      printerr("ERROR: imc_mem_obj_create failed\n");
    }

    char* buf = (char *) mmap(NULL,
                              count_up,
                              PROT_WRITE,
                              MAP_SHARED,
                              fd,
                              0);
    if (NULL == buf) {
      printerr("ERROR: cannot map shm for write\n");
      return;
    }

    memcpy(buf, data.c_str(), data.size());
    munmap(buf, count_up);
    fd_ = fd;
    size_ = data.size();
  }

  static FileInfo* FindFileInfo(const string& fn) {
    map<string, FileInfo*>::iterator it = descriptor_map_.find(fn);
    if (it == descriptor_map_.end()) {
      printerr("ERROR: unknown file %s\n", fn.c_str());
      return NULL;
    }

    return it->second;
  }
};

map<string, FileInfo*> FileInfo::descriptor_map_;

extern int llc_main(int argc, char **argv);


MemoryBuffer* NaClGetMemoryBufferForFile(const char* filename) {
  FileInfo* fi = FileInfo::FindFileInfo(string(filename));
  return fi->ReadAllDataAsMemoryBuffer();
}

void NaClOutputStringToFile(const char* filename, const string& data) {
  FileInfo* fi = FileInfo::FindFileInfo(filename);
  fi->WriteAllData(data);
}

#define MAX_LLC_ARGS 256
// Must keep in sync with initializer.
#define BAKED_IN_LLC_ARGS 4
static char *llc_argv[MAX_LLC_ARGS] = { "llc",
                                        "bitcode_combined",
                                        "-o",
                                        "obj_combined", };
static int llc_argc = BAKED_IN_LLC_ARGS;

static void reset_arg_array() {
  // Free old args
  for (int i = BAKED_IN_LLC_ARGS; i < llc_argc; ++i) {
    free(llc_argv[i]);
  }
  llc_argc = BAKED_IN_LLC_ARGS;
}

static void add_arg_string(NaClSrpcRpc *rpc,
          NaClSrpcArg **in_args,
          NaClSrpcArg **out_args,
          NaClSrpcClosure *done) {
  if (llc_argc >= MAX_LLC_ARGS) {
    printerr("Can't AddArg #(%d) beyond MAX_LLC_ARGS(%d)\n",
             llc_argc, MAX_LLC_ARGS);
    exit(1);
  }

  llc_argv[llc_argc] = strdup(in_args[0]->arrays.str);
  if (NULL == llc_argv[llc_argc]) {
    printerr("Out of memory for copying arg string\n");
    exit(1);
  }
  llc_argc++;

  rpc->result = NACL_SRPC_RESULT_OK;
  done->Run(done);
}

void
translate(NaClSrpcRpc *rpc,
          NaClSrpcArg **in_args,
          NaClSrpcArg **out_args,
          NaClSrpcClosure *done) {
  /* Input bitcode file. This is supplied by urlAsNaClDesc,
   * which should get the right size from fstat(). */
  int bitcode_fd = in_args[0]->u.hval;

  // input file (side effect is to register the file)
  new FileInfo("bitcode_combined", bitcode_fd);

  // NOTE: we bypass the output file name mangling in llc.cpp
  //       so the output name does not get the ".o" suffix
  FileInfo* output_file = new FileInfo("obj_combined", -1);


  /* Call main. */
  llc_main(llc_argc, llc_argv);
  reset_arg_array();

  /* Save obj fd for return. */
  out_args[0]->u.hval = output_file->GetFd();
  out_args[1]->u.ival = output_file->GetSize();

  rpc->result = NACL_SRPC_RESULT_OK;
  done->Run(done);
}

const struct NaClSrpcHandlerDesc srpc_methods[] = {
  { "AddArg:s:", add_arg_string },
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
