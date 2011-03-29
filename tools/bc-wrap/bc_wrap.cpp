/************************
 * Utility to wrap a .bc file.
 */

#include <string.h>

#include "llvm/Wrap/bitcode_wrapperer.h"
#include "llvm/Wrap/file_wrapper_input.h"
#include "llvm/Wrap/file_wrapper_output.h"

static bool k_unwrap_bc = false;

static void usage(int argc, const char* argv[]) {
  fprintf(stderr, "usage: %s [-u] in.bc file out.bc\n\n", argv[0]);
  fprintf(stderr, "\tWraps (unwraps) file into (out of) bitcode file\n");
}

static int GrokFlags(int argc, const char* argv[]) {
  if (argc == 0) return 0;
  int new_argc = 1;
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (0 == strcmp("-u", arg)) {
      k_unwrap_bc = true;
    } else {
      argv[new_argc++] = argv[i];
    }
  }
  return new_argc;
}

int main(const int argc, const char* argv[]) {
  bool success = false;
  int new_argc = GrokFlags(argc, argv);
  if (new_argc != 4) {
    usage(argc, argv);
  } else if (k_unwrap_bc) {
    FileWrapperInput inbc(argv[1]);
    FileWrapperOutput file(argv[2]);
    FileWrapperOutput outbc(argv[3]);
    BitcodeWrapperer wrapperer(&inbc, &outbc);
    if (wrapperer.IsInputBitcodeWrapper()) {
      success = wrapperer.GenerateBitcodeFile(&file);
    }
  } else {
    FileWrapperInput inbc(argv[1]);
    FileWrapperInput file(argv[2]);
    FileWrapperOutput outbc(argv[3]);
    BitcodeWrapperer wrapperer(&inbc, &outbc);
    if (wrapperer.IsInputBitcodeWrapper()) {
      success = wrapperer.GenerateBitcodeWrapperWrapper(&file);
    } else if (wrapperer.IsInputBitcodeFile()) {
      success = wrapperer.GenerateBitcodeFileWrapper(&file);
    }
  }
  if (success) return 0;
  fprintf(stderr, "error: Unable to generate a proper %s bitcode file!\n",
          (k_unwrap_bc ? "unwrapped" : "wrapped"));
  return 1;
}
