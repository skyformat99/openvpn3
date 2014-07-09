//
//  lzoasym_impl.hpp
//  OpenVPN
//
//  Copyright (c) 2012 OpenVPN Technologies, Inc. All rights reserved.
//

// This is a special OpenVPN-specific implementation of LZO decompression.
// It is generally only used when OpenVPN is built without linkage to the
// actual LZO library, but where we want to maintain compatibility with
// peers that might send us LZO-compressed packets.
//
// It is significantly faster than LZO 2 on ARM because it makes heavy use
// of branch prediction hints.

#ifndef OPENVPN_COMPRESS_LZOASYM_IMPL_H
#define OPENVPN_COMPRESS_LZOASYM_IMPL_H

#include <boost/cstdint.hpp> // for boost::uint32_t, etc.

#include <openvpn/common/types.hpp>  // for ssize_t
#include <openvpn/common/likely.hpp> // for likely/unlikely

// Implementation of asymmetrical LZO compression (only uncompress, don't compress)

// Branch prediction hints (these make a difference on ARM)
# define LZOASYM_LIKELY(x)   likely(x)
# define LZOASYM_UNLIKELY(x) unlikely(x)

// Failure modes
#define LZOASYM_CHECK_INPUT_OVERFLOW(x) if (LZOASYM_UNLIKELY(int(input_ptr_end - input_ptr) < int(x))) goto input_overflow
#define LZOASYM_CHECK_OUTPUT_OVERFLOW(x) if (LZOASYM_UNLIKELY(int(output_ptr_end - output_ptr) < int(x))) goto output_overflow
#define LZOASYM_CHECK_MATCH_OVERFLOW(match_ptr) if (LZOASYM_UNLIKELY(match_ptr < output) || LZOASYM_UNLIKELY(match_ptr >= output_ptr)) goto match_overflow
#define LZOASYM_ASSERT(cond) if (LZOASYM_UNLIKELY(!(cond))) goto assert_fail

namespace openvpn {
  namespace lzo_asym_impl {
    // Return status values
    enum {
      LZOASYM_E_OK=0,
      LZOASYM_E_EOF_NOT_FOUND=-1,
      LZOASYM_E_INPUT_NOT_CONSUMED=-2,
      LZOASYM_E_INPUT_OVERFLOW=-3,
      LZOASYM_E_OUTPUT_OVERFLOW=-4,
      LZOASYM_E_MATCH_OVERFLOW=-5,
      LZOASYM_E_ASSERT_FAILED=-6,
      LZOASYM_E_INPUT_TOO_LARGE=-7,
    };

    // Internal constants
    enum {
      LZOASYM_EOF_CODE=1,
      LZOASYM_M2_MAX_OFFSET=0x0800,
    };

    // Polymorphic get/set/copy

    template <typename T>
    inline T get_mem(const void *p)
    {
      typedef volatile const T* cptr;
      return *cptr(p);
    }

    template <typename T>
    inline T set_mem(void *p, const T value)
    {
      typedef volatile T* ptr;
      *ptr(p) = value;
    }

    template <typename T>
    inline void copy_mem(void *dest, const void *src)
    {
      typedef volatile T* ptr;
      typedef volatile const T* cptr;
      *ptr(dest) = *cptr(src);
    }

    template <typename T>
    inline bool ptr_aligned_4(const T* a, const T* b)
    {
      return ((size_t(a) | size_t(b)) & 3) == 0;
    }


    // take the number of objects difference between two pointers
    template <typename T>
    inline size_t ptr_diff(const T* a, const T* b)
    {
      return a - b;
    }

    // read uint16_t from memory
    inline size_t get_u16(const unsigned char *p)
    {
      // NOTE: assumes little-endian and unaligned 16-bit access is okay.
      // For a slower alternative without these assumptions, try: p[0] | (p[1] << 8)
      return get_mem<boost::uint16_t>(p);
    }

    // copy 64 bits
    inline void copy_64(unsigned char *dest, const unsigned char *src)
    {
      // NOTE: assumes that 64-bit machines can do 64-bit unaligned access, and
      // 32-bit machines can do 32-bit unaligned access.
      if (sizeof(void *) == 8)
	{
	  copy_mem<boost::uint64_t>(dest, src);
	}
      else
	{
	  copy_mem<boost::uint32_t>(dest, src);
	  copy_mem<boost::uint32_t>(dest+4, src+4);
	}
    }

    // Fast version of incremental copy.
    // NOTE: we might write up to ten extra bytes after the end of the copy.
    inline void incremental_copy_fast(unsigned char *dest, const unsigned char *src, ssize_t len)
    {
      while (LZOASYM_UNLIKELY(dest - src < 8))
	{
	  copy_64(dest, src);
	  len -= dest - src;
	  dest += dest - src;
	}
      while (len > 0)
	{
	  copy_64(dest, src);
	  src += 8;
	  dest += 8;
	  len -= 8;
	}
    }

    // Slow version of incremental copy
    inline void incremental_copy(unsigned char *dest, const unsigned char *src, ssize_t len)
    {
      do {
	*dest++ = *src++;
      } while (--len);
    }

    // Faster version of memcpy
    inline void copy_fast(unsigned char *dest, const unsigned char *src, ssize_t len)
    {
      while (len >= 8)
	{
	  copy_64(dest, src);
	  src += 8;
	  dest += 8;
	  len -= 8;
	}
      if (len >= 4)
	{
	  copy_mem<boost::uint32_t>(dest, src);
	  src += 4;
	  dest += 4;
	  len -= 4;
	}
      switch (len)
	{
	case 3:
	  *dest++ = *src++;
	case 2:
	  *dest++ = *src++;
	case 1:
	  *dest = *src;
	}
    }

    int lzo1x_decompress_safe(const unsigned char *input,
			      size_t input_length,
			      unsigned char *output,
			      size_t *output_length)
    {
      size_t z;
      const unsigned char *input_ptr;
      unsigned char *output_ptr;
      const unsigned char *match_ptr;
      const unsigned char *const input_ptr_end = input + input_length;
      unsigned char *const output_ptr_end = output + *output_length;

      *output_length = 0;

      input_ptr = input;
      output_ptr = output;

      if (LZOASYM_UNLIKELY(input_length > 65536)) // quick fix to prevent 16MB integer overflow vulnerability
	goto input_too_large;

      if (LZOASYM_LIKELY(*input_ptr <= 17))
	{
	  while (LZOASYM_LIKELY(input_ptr < input_ptr_end) && LZOASYM_LIKELY(output_ptr <= output_ptr_end))
	    {
	      z = *input_ptr++;
	      if (z < 16) // literal data?
		{
		  if (LZOASYM_UNLIKELY(z == 0))
		    {
		      LZOASYM_CHECK_INPUT_OVERFLOW(1);
		      while (LZOASYM_UNLIKELY(*input_ptr == 0))
			{
			  z += 255;
			  input_ptr++;
			  LZOASYM_CHECK_INPUT_OVERFLOW(1);
			}
		      z += 15 + *input_ptr++;
		    }

		  // copy literal data
		  {
		    LZOASYM_ASSERT(z > 0);
		    const size_t len = z + 3;
		    LZOASYM_CHECK_OUTPUT_OVERFLOW(len);
		    LZOASYM_CHECK_INPUT_OVERFLOW(len+1);
		    copy_fast(output_ptr, input_ptr, len);
		    input_ptr += len;
		    output_ptr += len;
		  }

		initial_literal:
		  z = *input_ptr++;
		  if (LZOASYM_UNLIKELY(z < 16))
		    {
		      match_ptr = output_ptr - (1 + LZOASYM_M2_MAX_OFFSET);
		      match_ptr -= z >> 2;
		      match_ptr -= *input_ptr++ << 2;

		      LZOASYM_CHECK_MATCH_OVERFLOW(match_ptr);
		      LZOASYM_CHECK_OUTPUT_OVERFLOW(3);
		      *output_ptr++ = *match_ptr++;
		      *output_ptr++ = *match_ptr++;
		      *output_ptr++ = *match_ptr;
		      goto match_complete;
		    }
		}

	      // found a match (M2, M3, M4, or M1)
	      do {
		if (LZOASYM_LIKELY(z >= 64))           // LZO "M2" match (most likely)
		  {
		    match_ptr = output_ptr - 1;
		    match_ptr -= (z >> 2) & 7;
		    match_ptr -= *input_ptr++ << 3;
		    z = (z >> 5) - 1;
		  }
		else if (LZOASYM_LIKELY(z >= 32))      // LZO "M3" match
		  {
		    z &= 31;
		    if (LZOASYM_UNLIKELY(z == 0))
		      {
			LZOASYM_CHECK_INPUT_OVERFLOW(1);
			while (LZOASYM_UNLIKELY(*input_ptr == 0))
			  {
			    z += 255;
			    input_ptr++;
			    LZOASYM_CHECK_INPUT_OVERFLOW(1);
			  }
			z += 31 + *input_ptr++;
		      }

		    match_ptr = output_ptr - 1;
		    match_ptr -= get_u16(input_ptr) >> 2;
		    input_ptr += 2;
		  }
		else if (LZOASYM_LIKELY(z >= 16))      // LZO "M4" match
		  {
		    match_ptr = output_ptr;
		    match_ptr -= (z & 8) << 11;
		    z &= 7;
		    if (LZOASYM_UNLIKELY(z == 0))
		      {
			LZOASYM_CHECK_INPUT_OVERFLOW(1);
			while (LZOASYM_UNLIKELY(*input_ptr == 0))
			  {
			    z += 255;
			    input_ptr++;
			    LZOASYM_CHECK_INPUT_OVERFLOW(1);
			  }
			z += 7 + *input_ptr++;
		      }

		    match_ptr -= get_u16(input_ptr) >> 2;
		    input_ptr += 2;
		    if (LZOASYM_UNLIKELY(match_ptr == output_ptr))
		      goto success;
		    match_ptr -= 0x4000;
		  }
		else                                   // LZO "M1" match (least likely)
		  {
		    match_ptr = output_ptr - 1;
		    match_ptr -= z >> 2;
		    match_ptr -= *input_ptr++ << 2;

		    LZOASYM_CHECK_MATCH_OVERFLOW(match_ptr);
		    LZOASYM_CHECK_OUTPUT_OVERFLOW(2);
		    *output_ptr++ = *match_ptr++;
		    *output_ptr++ = *match_ptr;
		    goto match_complete;
		  }

		// copy the match we found above
		{
		  LZOASYM_CHECK_MATCH_OVERFLOW(match_ptr);
		  LZOASYM_ASSERT(z > 0);
		  LZOASYM_CHECK_OUTPUT_OVERFLOW(z+3-1);

		  const size_t len = z + 2;
		  // Should we use optimized incremental copy?
		  // incremental_copy_fast might copy 10 more bytes than needed, so
		  // don't use it unless we have enough trailing space in buffer.
		  if (LZOASYM_LIKELY(size_t(output_ptr_end - output_ptr) >= len + 10))
		    incremental_copy_fast(output_ptr, match_ptr, len);
		  else
		    incremental_copy(output_ptr, match_ptr, len);
		  match_ptr += len;
		  output_ptr += len;
		}

	      match_complete:
		z = input_ptr[-2] & 3;
		if (LZOASYM_LIKELY(z == 0))
		  break;

	      match_continue:
		// copy literal data
		LZOASYM_ASSERT(z > 0);
		LZOASYM_ASSERT(z < 4);
		LZOASYM_CHECK_OUTPUT_OVERFLOW(z);
		LZOASYM_CHECK_INPUT_OVERFLOW(z+1);
		*output_ptr++ = *input_ptr++;
		if (LZOASYM_LIKELY(z > 1))
		  {
		    *output_ptr++ = *input_ptr++;
		    if (z > 2)
		      *output_ptr++ = *input_ptr++;
		  }
		z = *input_ptr++;
	      } while (LZOASYM_LIKELY(input_ptr < input_ptr_end) && LZOASYM_LIKELY(output_ptr <= output_ptr_end));
	    }
	}
      else
	{
	  // input began with a match or a literal (rare)
	  z = *input_ptr++ - 17;
	  if (z < 4)
	    goto match_continue;
	  LZOASYM_ASSERT(z > 0);
	  LZOASYM_CHECK_OUTPUT_OVERFLOW(z);
	  LZOASYM_CHECK_INPUT_OVERFLOW(z+1);
	  do {
	    *output_ptr++ = *input_ptr++;
	  } while (--z > 0);
	  goto initial_literal;
	}

      *output_length = ptr_diff(output_ptr, output);
      return LZOASYM_E_EOF_NOT_FOUND;

    success:
      LZOASYM_ASSERT(z == 1);
      *output_length = ptr_diff(output_ptr, output);
      return (input_ptr == input_ptr_end ? LZOASYM_E_OK :
	      (input_ptr < input_ptr_end  ? LZOASYM_E_INPUT_NOT_CONSUMED : LZOASYM_E_INPUT_OVERFLOW));

    input_overflow:
      *output_length = ptr_diff(output_ptr, output);
      return LZOASYM_E_INPUT_OVERFLOW;

    output_overflow:
      *output_length = ptr_diff(output_ptr, output);
      return LZOASYM_E_OUTPUT_OVERFLOW;

    match_overflow:
      *output_length = ptr_diff(output_ptr, output);
      return LZOASYM_E_MATCH_OVERFLOW;

    assert_fail:
      return LZOASYM_E_ASSERT_FAILED;

    input_too_large:
      return LZOASYM_E_INPUT_TOO_LARGE;
    }
  }
}

#undef LZOASYM_CHECK_INPUT_OVERFLOW
#undef LZOASYM_CHECK_OUTPUT_OVERFLOW
#undef LZOASYM_CHECK_MATCH_OVERFLOW
#undef LZOASYM_ASSERT
#undef LZOASYM_LIKELY
#undef LZOASYM_UNLIKELY

#endif
