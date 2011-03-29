#include "llvm/Wrap/bitcode_wrapperer.h"

#include <stdio.h>
#include <sys/stat.h>

// The number of bytes in a 32 bit integer.
static const uint32_t kWordSize = 4;

// The number of header words.
static const uint32_t kHeaderWords = 7;

// The number of bytes in a header block.
static const uint32_t kHeaderSize = kHeaderWords * kWordSize;

// The magic number that must exist for bitcode wrappers.
static const uint32_t kWrapperMagicNumber = 0x0B17C0DE;

// The version number associated with a wrapper file.
// Note: llvm currently only allows the value 0. When this changes,
// we should consider making this a command line option.
static const uint32_t kWrapperVersionNumber = 0;

BitcodeWrapperer::BitcodeWrapperer(WrapperInput* infile, WrapperOutput* outfile)
    : _infile(infile),
      _outfile(outfile),
      _buffer_size(0),
      _cursor(0),
      _in_at_eof(false),
      _wrapper_bc_offset(0),
      _wrapper_bc_size(0),
      _wrapper_bc_file_size(0),
      _wrapper_file_offset(0),
      _wrapper_file_size(0)
{}

void BitcodeWrapperer::ReplaceInFile(WrapperInput* new_infile) {
  _infile = new_infile;
  _buffer_size = 0;
  _cursor = 0;
  _in_at_eof = false;
  _wrapper_bc_offset = 0;
  _wrapper_bc_size = 0;
  _wrapper_bc_file_size = 0;
  _wrapper_file_offset = 0;
  _wrapper_file_size = 0;
}

void BitcodeWrapperer::ReplaceOutFile(WrapperOutput* new_outfile) {
  _outfile = new_outfile;
}

void BitcodeWrapperer::ClearBuffer() {
  _buffer_size = 0;
  _cursor = 0;
  _in_at_eof = false;
}

bool BitcodeWrapperer::Seek(uint32_t pos) {
  if (_infile->Seek(pos)) {
    ClearBuffer();
    return true;
  }
  return false;
}

bool BitcodeWrapperer::CanReadWord() {
  if (BufferSize() < kWordSize) {
    FillBuffer();
    return BufferSize() >= kWordSize;
  } else {
    return true;
  }
}

void BitcodeWrapperer::FillBuffer() {
  if (_cursor > 0) {
    // Before filling, move any remaining bytes to the
    // front of the buffer. This allows us to assume
    // that after the call to FillBuffer, readable
    // text is contiguous.
    if (_cursor < _buffer_size) {
      size_t i = 0;
      while (_cursor < _buffer_size) {
        _buffer[i++] = _buffer[_cursor++];
      }
      _cursor = 0;
      _buffer_size = i;
    }
  } else {
    // Assume the buffer contents have been used,
    // and we want to completely refill it.
    _buffer_size = 0;
  }

  // Now fill in remaining space.
  size_t needed = kBitcodeWrappererBufferSize - _buffer_size;

  while (kBitcodeWrappererBufferSize > _buffer_size) {
    int actually_read = _infile->Read(_buffer, needed);
    if (_infile->AtEof()) {
      _in_at_eof = true;
    }
    if (actually_read) {
      _buffer_size += actually_read;
      needed -= actually_read;
    } else if (_in_at_eof) {
      break;
    }
  }
}

bool BitcodeWrapperer::ReadWord(uint32_t& word) {
  if (!CanReadWord()) return false;
  word = (((uint32_t) BufferLookahead(0)) << 0)
      | (((uint32_t) BufferLookahead(1)) << 8)
      | (((uint32_t) BufferLookahead(2)) << 16)
      | (((uint32_t) BufferLookahead(3)) << 24);
  _cursor += kWordSize;
  return true;
}

bool BitcodeWrapperer::WriteWord(uint32_t value) {
  uint8_t buffer[kWordSize];
  buffer[3] = (value >> 24) & 0xFF;
  buffer[2] = (value >> 16) & 0xFF;
  buffer[1] = (value >> 8)  & 0xFF;
  buffer[0] = (value >> 0)  & 0xFF;
  return _outfile->Write(buffer, kWordSize);
}

bool BitcodeWrapperer::IsInputBitcodeWrapper() {
  ResetCursor();
  // First make sure that there are enough words (wrapper header)
  // to peek at.
  if (BufferSize() < kHeaderSize) {
    FillBuffer();
    if (BufferSize() < kHeaderSize) return false;
  }

  // Now make sure the magic number is right.
  uint32_t first_word;
  if ((!ReadWord(first_word)) ||
      (kWrapperMagicNumber != first_word)) return false;

  // Make sure the version is right.
  uint32_t second_word;
  if ((!ReadWord(second_word)) ||
      (kWrapperVersionNumber != second_word)) return false;

  // Make sure that the offset and size (for llvm) is defined.
  // along with our own added words defining the offset and size
  // of the added wrapped file.
  uint32_t bc_offset;
  uint32_t bc_size;
  uint32_t bc_file_size;
  uint32_t file_offset;
  uint32_t file_size;
  if (ReadWord(bc_offset) &&
      ReadWord(bc_size) &&
      ReadWord(bc_file_size) &&
      ReadWord(file_offset) &&
      ReadWord(file_size)) {
    // Before returning, save the extracted values.
    _wrapper_bc_offset = bc_offset;
    _wrapper_bc_size = bc_size;
    _wrapper_bc_file_size = bc_file_size;
    _wrapper_file_offset = file_offset;
    _wrapper_file_size = file_size;
    return true;
  }
  // If reached, unable to read wrapped header.
  return false;
}

bool BitcodeWrapperer::IsInputBitcodeFile() {
  ResetCursor();
  // First make sure that there are four bytes to peek at.
  if (BufferSize() < kWordSize) {
    FillBuffer();
    if (BufferSize() < kWordSize) return false;
  }
  // If reached, Check if first 4 bytes match bitcode
  // file magic number.
  return (BufferLookahead(0) == 'B') &&
      (BufferLookahead(1) == 'C') &&
      (BufferLookahead(2) == 0xc0) &&
      (BufferLookahead(3) == 0xde);
}

bool BitcodeWrapperer::BufferCopyInToOut(uint32_t size) {
  while (size > 0) {
    // Be sure buffer is non-empty before writing.
    if (0 == _buffer_size) {
      FillBuffer();
      if (0 == _buffer_size) {
        return false;
      }
    }
    // copy the buffer to the output file.
    size_t block = (_buffer_size < size) ? _buffer_size : size;
    if (!_outfile->Write(_buffer, block)) return false;
    size -= block;
    _buffer_size = 0;
  }
  // Be sure that there isn't more bytes on the input stream.
  FillBuffer();
  return _buffer_size == 0;
}

bool BitcodeWrapperer::WriteBitcodeWrapperHeader(uint32_t offset,
                                                 uint32_t size,
                                                 WrapperInput* file) {
  return
      // Note: This writes out the 4 word header required by llvm wrapped
      // bitcode.
      WriteWord(kWrapperMagicNumber) &&
      WriteWord(kWrapperVersionNumber) &&
      WriteWord(offset) &&
      WriteWord(size) &&
      // Save size of input bitcode file, since the size passed in may be
      // smaller than the bitcode file (this happens when we wrap multiple
      // files into a bitcode file). Note: We follow the assumption of llvm,
      // which wants bitcode (and hence wrapped bitcode) to be limitted to
      // values that fit in 32 bits.
      WriteWord(GetInFileSize()) &&
      // Place wrapped file right after bitcode file. Note: We follow the
      // assumption of llvm, which wants bitcode (and hence wrapped bitcode)
      // to be limitted to values that fit in 32 bits.
      WriteWord(kHeaderSize + GetInFileSize()) &&
      WriteWord(file->Size());
}

bool BitcodeWrapperer::GenerateBitcodeWrapper(WrapperInput* file,
                                              uint32_t offset,
                                              uint32_t size) {
  ResetCursor();
  if (WriteBitcodeWrapperHeader(offset, size, file) &&
      BufferCopyInToOut(GetInFileSize())) {
    ReplaceInFile(file);
    off_t file_size = GetInFileSize();
    if (BufferCopyInToOut(file_size) &&
        _buffer_size == 0) {
      // Deal with silly fact that llvm expects bitcode files to
      // be multipes of 4 bytes. Note: we assume that the header
      // and the original input are already a multiple of 4, so
      // we only need to fill based on the size of the wrapped file.
      off_t dangling = GetInFileSize() & 3;
      if (dangling > 0) {
        return _outfile->Write((const uint8_t*) "\0\0\0\0", 4 - dangling);
      }
    }
  }
  // If reached, something went wrong.
  return false;
}

bool BitcodeWrapperer::GenerateBitcodeFileWrapper(WrapperInput* file) {
  // Begin wrapped bitcode right after header!
  return GenerateBitcodeWrapper(file, kHeaderSize,
                                (uint32_t) _infile->Size());
}

bool BitcodeWrapperer::GenerateBitcodeWrapperWrapper(WrapperInput* file) {
  // Begin wrapped code at position of wrapped bitcode.
  return GenerateBitcodeWrapper(file, kHeaderSize + _wrapper_bc_offset,
                                _wrapper_bc_size);
}

bool BitcodeWrapperer::GenerateBitcodeFile(WrapperOutput* file) {
  if (_wrapper_bc_offset < kHeaderSize) return false;
  if (Seek(kHeaderSize) &&
      BufferCopyInToOut(_wrapper_bc_file_size)) {
    ReplaceOutFile(file);
    return Seek(_wrapper_file_offset) &&
        BufferCopyInToOut(_wrapper_file_size);
  }
  return false;
}
