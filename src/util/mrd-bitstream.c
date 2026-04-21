#include "mrd-bitstream.h"
#include <string.h>
MrdBitstream *
mrd_bitstream_new (size_t initial_capacity)
{
  MrdBitstream *bitstream = g_new0 (MrdBitstream, 1);
  if (initial_capacity > 0) {
    bitstream->data = g_malloc (initial_capacity);
    bitstream->capacity = initial_capacity;
  }
  bitstream->owns_data = TRUE;
  return bitstream;
}
MrdBitstream *
mrd_bitstream_new_from_data (uint8_t *data,
                              size_t   length,
                              gboolean take_ownership)
{
  MrdBitstream *bitstream = g_new0 (MrdBitstream, 1);
  bitstream->data = data;
  bitstream->length = length;
  bitstream->capacity = length;
  bitstream->owns_data = take_ownership;
  return bitstream;
}
MrdBitstream *
mrd_bitstream_new_with_data (const uint8_t *data,
                              size_t         length)
{
  MrdBitstream *bitstream = g_new0 (MrdBitstream, 1);
  bitstream->data = g_memdup2 (data, length);
  bitstream->length = length;
  bitstream->capacity = length;
  bitstream->owns_data = TRUE;
  return bitstream;
}
void
mrd_bitstream_free (MrdBitstream *bitstream)
{
  if (!bitstream)
    return;
  if (bitstream->owns_data)
    g_free (bitstream->data);
  g_free (bitstream);
}
void
mrd_bitstream_append (MrdBitstream  *bitstream,
                      const uint8_t *data,
                      size_t         length)
{
  g_return_if_fail (bitstream != NULL);
  g_return_if_fail (bitstream->owns_data);
  size_t new_length = bitstream->length + length;
  if (new_length > bitstream->capacity) {
    size_t new_capacity = MAX (new_length, bitstream->capacity * 2);
    bitstream->data = g_realloc (bitstream->data, new_capacity);
    bitstream->capacity = new_capacity;
  }
  memcpy (bitstream->data + bitstream->length, data, length);
  bitstream->length = new_length;
}
void
mrd_bitstream_clear (MrdBitstream *bitstream)
{
  g_return_if_fail (bitstream != NULL);
  bitstream->length = 0;
}
uint8_t *
mrd_bitstream_get_data (MrdBitstream *bitstream)
{
  g_return_val_if_fail (bitstream != NULL, NULL);
  return bitstream->data;
}
size_t
mrd_bitstream_get_length (MrdBitstream *bitstream)
{
  g_return_val_if_fail (bitstream != NULL, 0);
  return bitstream->length;
}