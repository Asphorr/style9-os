/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * libz_stub.c -- clean-room /usr/lib/libz.1.dylib, without a codec.
 *
 * sqlite3's shell links zlib for its archive commands (.archive, the
 * sqlar and zipfile tables) and for nothing else, so a zlib that cannot
 * compress costs only those.  Every stream call fails -- Z_STREAM_ERROR,
 * with the reason in strm->msg -- and compress/uncompress answer
 * Z_MEM_ERROR.  crc32 and the bounds are the real arithmetic.
 *
 * Zero imports, so the dyld closure ends here.  A z_stream is opaque but
 * for msg, at offset 48 on LP64.
 */

#define	Z_STREAM_ERROR	(-2)
#define	Z_MEM_ERROR	(-4)
#define	ZS_MSG_OFF	48		/* z_stream.msg */

typedef unsigned long	uLong;
typedef unsigned int	uInt;

static const char	no_codec[] = "this zlib has no codec (style9 stub)";

static int
refuse(void *strm)
{

	if (strm != 0)
		*(const char **)(void *)((char *)strm + ZS_MSG_OFF) = no_codec;
	return (Z_STREAM_ERROR);
}

const char *
zlibVersion(void)
{

	return ("1.3.1");
}

int
deflateInit2_(void *strm, int level, int method, int window, int memlevel,
    int strategy, const char *version, int size)
{

	(void)level;
	(void)method;
	(void)window;
	(void)memlevel;
	(void)strategy;
	(void)version;
	(void)size;
	return (refuse(strm));
}

int
inflateInit2_(void *strm, int window, const char *version, int size)
{

	(void)window;
	(void)version;
	(void)size;
	return (refuse(strm));
}

int
deflate(void *strm, int flush)
{

	(void)flush;
	return (refuse(strm));
}

int
inflate(void *strm, int flush)
{

	(void)flush;
	return (refuse(strm));
}

int
deflateEnd(void *strm)
{

	return (refuse(strm));
}

int
inflateEnd(void *strm)
{

	return (refuse(strm));
}

int
compress(unsigned char *dst, uLong *dstlen, const unsigned char *src,
    uLong srclen)
{

	(void)dst;
	(void)dstlen;
	(void)src;
	(void)srclen;
	return (Z_MEM_ERROR);
}

int
uncompress(unsigned char *dst, uLong *dstlen, const unsigned char *src,
    uLong srclen)
{

	(void)dst;
	(void)dstlen;
	(void)src;
	(void)srclen;
	return (Z_MEM_ERROR);
}

/* zlib's own bounds, so a caller sizes its buffers as it would anywhere. */
uLong
compressBound(uLong n)
{

	return (n + (n >> 12) + (n >> 14) + (n >> 25) + 13);
}

uLong
deflateBound(void *strm, uLong n)
{

	(void)strm;
	return (n + ((n + 7) >> 3) + ((n + 63) >> 6) + 5 + 6);
}

/* CRC-32 (IEEE 802.3, reflected), bit by bit. */
uLong
crc32(uLong crc, const unsigned char *buf, uInt len)
{
	uInt	k;

	if (buf == 0)
		return (0);
	crc = ~crc & 0xFFFFFFFFul;
	while (len-- != 0) {
		crc ^= *buf++;
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320ul & (0ul - (crc & 1)));
	}
	return (~crc & 0xFFFFFFFFul);
}
