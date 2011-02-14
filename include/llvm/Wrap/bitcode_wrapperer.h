// Define utility class to wrap/unwrap bitcode files. Does wrapping/unwrapping
// in such a way that the wrappered bitcode file is still a bitcode file.

#ifndef LLVM_WRAP_BITCODE_WRAPPERER_H__
#define LLVM_WRAP_BITCODE_WRAPPERER_H__

#include <stdint.h>
#include <stddef.h>

#include "llvm/Support/support_macros.h"
#include "llvm/Wrap/wrapper_input.h"
#include "llvm/Wrap/wrapper_output.h"

// The following Must be at least 28 (7 word), so that it can hold the bitcode
// wrapper header. The bitcode wrapper header is the following 7 words:
//      1) 0B17C0DE - The magic number expected by llvm for wrapped bitcodes
//      2) Version # 0 - The current version of wrapped bitcode files.
//      3) (raw) bitcode offset.
//      4) (raw) bitcode size.
//      5) Size of wrapped bitcode.
//      6) Offset of wrapped file.
//      7) Size of wrapped file.
static const size_t kBitcodeWrappererBufferSize = 1024;

// Support class for wrapping the input bitcode file, and
// a second input file, into an output wrappered bitcode
// file, or to take a wrappered input bitcode file and split
// it back up into a bitcode file and a second file.
class BitcodeWrapperer {
 public:
  // Create a bitcode wrapperer using the following
  // (initial) input and output files.
  BitcodeWrapperer(WrapperInput* infile, WrapperOutput* outfile);

  // Returns true if the input file begins with a bitcode
  // wrapper magic number. As a side effect, _wrapper_ fields are set.
  bool IsInputBitcodeWrapper();

  // Returns true if the input file begins with a bitcode
  // file magic number.
  bool IsInputBitcodeFile();

  // Generate a wrapped bitcode file from the input bitcode
  // file, and wrap file into the generated wrapped bitcode
  // file. Returns true if able to correctly generate the
  // wrapped bitcode file.
  bool GenerateBitcodeFileWrapper(WrapperInput* file);

  // Generate a wrapped bitcode file form an existing wrapped
  // bitcode file, and wrap file into the generated wrapped
  // bitcode file. Returns true if able to correctly generate
  // the wrapped bitcode file.
  bool GenerateBitcodeWrapperWrapper(WrapperInput* file);

  // Unwrap the wrapped bitcode file, to the corresponding
  // outfile, and put the embedded file into the given file.
  // Returns true if able to correctly generate both files.
  bool GenerateBitcodeFile(WrapperOutput* file);

  ~BitcodeWrapperer() {}

 private:
  DISALLOW_CLASS_COPY_AND_ASSIGN(BitcodeWrapperer);

  // Refills the buffer with more bytes. Does this in a way
  // such that it is maximally filled. At eof if BufferSize() == 0
  // after call.
  void FillBuffer();

  // Returns the number of bytes in infile.
  off_t GetInFileSize() { return _infile->Size(); }

  // Returns true if we can read a word. If necessary, fills the buffer
  // with enough characters so that there are at least a 32-bit value
  // in the buffer. Returns false if there isn't a 32-bit value
  // to read from the input file.
  bool CanReadWord();

  // Read a (32-bit) word from the input. Returns true
  // if able to read the word.
  bool ReadWord(uint32_t& word);

  // Write a (32-bit) word to the output. Return true if successful
  bool WriteWord(uint32_t word);

  // Returns the i-th character in front of the cursor in the buffer.
  uint8_t BufferLookahead(int i) { return _buffer[_cursor + i]; }

  // Returns how many unread bytes are in the buffer.
  size_t BufferSize() { return _buffer_size - _cursor; }


  // Backs up the read cursor to the beginning of the input buffer.
  void ResetCursor() {
    _cursor = 0;
  }

  // Generates the header sequence for the wrapped bitcode being
  // generated.
  // Parameters:
  //   offset - The offset of the wrapped raw bitcode file to be wrapped.
  //   size - The number of bytes in the raw bitcode file to be wrapped.
  //   file - The file to be wrapped into the bitcode file.
  //Returns: true if successfully generated header.
  bool WriteBitcodeWrapperHeader(uint32_t offset, uint32_t size,
                                 WrapperInput* file);

  // Build a wrapped bitcode file, embedding the given file. Use
  // The given offset and file size as the sizes to put into
  // the bitcoder wrapper header.
  bool GenerateBitcodeWrapper(WrapperInput* file,
                              uint32_t offset,
                              uint32_t size);

  // Copies size bytes of infile to outfile, using the buffer.
  bool BufferCopyInToOut(uint32_t size);

  // Discards the old infile and replaces it with the given file.
  void ReplaceInFile(WrapperInput* new_infile);

  // Discards the old outfile and replaces it with the given file.
  void ReplaceOutFile(WrapperOutput* new_outfile);

  // Moves to the given position in the input file. Returns false
  // if unsuccessful.
  bool Seek(uint32_t pos);

  // Clear the buffer of all contents.
  void ClearBuffer();

  // The (current) input file being processed. Can be either
  // a bitcode file, a wrappered bitcode file, or a secondary
  // file to be wrapped.
  WrapperInput* _infile;

  // The (current) output file being generated. Can be either
  // a bitcode file, a wrappered bitcode file, or a secondary
  // unwrapped file.
  WrapperOutput* _outfile;

  // A buffer of bytes read from the input file.
  uint8_t _buffer[kBitcodeWrappererBufferSize];

  // The number of bytes that were read from the input file
  // into the buffer.
  size_t _buffer_size;

  // The index to the current read point within the buffer.
  size_t _cursor;

  // True when eof of input is reached.
  bool _in_at_eof;

  // The 32-bit value defining the offset of the raw wrapped bitcode.
  uint32_t _wrapper_bc_offset;

  // The 32-bit value defining the size of the raw wrapped bitcode.
  uint32_t _wrapper_bc_size;

  // The 32-bit value defining the size of the wrapped bitcode file.
  uint32_t _wrapper_bc_file_size;

  // The 32-bit value defining the offset of the wrapped file.
  uint32_t _wrapper_file_offset;

  // The 32-bit value defining the size of the wrapped file.
  uint32_t _wrapper_file_size;
};

#endif  // LLVM_WRAP_BITCODE_WRAPPERER_H__
