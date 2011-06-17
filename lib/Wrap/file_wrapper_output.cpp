#include <stdlib.h>

#include "llvm/Wrap/file_wrapper_output.h"

FileWrapperOutput::FileWrapperOutput(const char* name)
    : _name(name) {
  _file = fopen(name, "wb");
  if (NULL == _file) {
    fprintf(stderr, "Unable to open: %s\n", name);
    exit(1);
  }
}

FileWrapperOutput::~FileWrapperOutput() {
  fclose(_file);
}

bool FileWrapperOutput::Write(uint8_t byte) {
  return EOF != fputc(byte, _file);
}

bool FileWrapperOutput::Write(const uint8_t* buffer, size_t buffer_size) {
  if (buffer_size > 0) {
    return buffer_size == fwrite(buffer, 1, buffer_size, _file);
  } else {
    return true;
  }
}
