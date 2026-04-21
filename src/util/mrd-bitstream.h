#pragma once

#include <glib.h>
#include "../mrd-types.h"

G_BEGIN_DECLS

struct _MrdBitstream
{
  uint8_t *data;
  size_t length;
  size_t capacity;
  gboolean owns_data;
};

MrdBitstream *mrd_bitstream_new (size_t initial_capacity);

MrdBitstream *mrd_bitstream_new_from_data (uint8_t *data,
                                            size_t   length,
                                            gboolean take_ownership);

/* Copies the data. */
MrdBitstream *mrd_bitstream_new_with_data (const uint8_t *data,
                                            size_t         length);

void mrd_bitstream_free (MrdBitstream *bitstream);

void mrd_bitstream_append (MrdBitstream  *bitstream,
                           const uint8_t *data,
                           size_t         length);

void mrd_bitstream_clear (MrdBitstream *bitstream);

uint8_t *mrd_bitstream_get_data (MrdBitstream *bitstream);
size_t mrd_bitstream_get_length (MrdBitstream *bitstream);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MrdBitstream, mrd_bitstream_free)

G_END_DECLS
