#include "private/compressor.h"
#include "private/errors.h"

#include <unistd.h> /* size_t */
#include <snappy-c.h>


size_t bp__max_compressed_size(size_t size) {
  return snappy_max_compressed_length(size);
}


int bp__compress(const char* input,
                 size_t input_length,
                 char* compressed,
                 size_t* compressed_length) {
  int ret = snappy_compress(input, input_length, compressed, compressed_length);
  return ret == SNAPPY_OK ? BP_OK : BP_ECOMP;
}

int bp__uncompressed_length(const char* compressed,
                            size_t compressed_length,
                            size_t* result) {
  int ret = snappy_uncompressed_length(compressed, compressed_length, result);
  return ret == SNAPPY_OK ? BP_OK : BP_EDECOMP;
}


int bp__uncompress(const char* compressed,
                   size_t compressed_length,
                   char* uncompressed,
                   size_t* uncompressed_length) {
  int ret = snappy_uncompress(compressed,
                              compressed_length,
                              uncompressed,
                              uncompressed_length);

  return ret == SNAPPY_OK ? BP_OK : BP_EDECOMP;
}
