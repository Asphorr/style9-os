/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * Random numbers (random.h).  BLAKE2s (RFC 7693) hashes what is gathered;
 * ChaCha20 (RFC 8439) expands a key from it.  Both are checked against
 * their published vectors at boot, before the generator is trusted.
 *
 * A reseed finishes the pool's hash into a seed, and from the seed derives
 * two keys: one for the generator, one to key the next pool, so every
 * later seed depends on every earlier one.  A request takes one ChaCha20
 * block under the lock -- half the new generator key, half a key for this
 * request alone -- and expands its own key with the lock dropped.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clock.h"
#include "cpu.h"
#include "cpuid.h"
#include "kprintf.h"
#include "panic.h"
#include "random.h"
#include "spinlock.h"
#include "tsc.h"

#define	RANDOM_RESEED_MS	1000	/* the longest the pool waits      */
#define	RANDOM_JITTER		4096	/* boot timing samples             */
#define	RANDOM_RDRAND_WORDS	32	/* from RDRAND, where there is one */

/* ---- BLAKE2s ------------------------------------------------------------- */

struct blake2s {
	uint32_t	b_h[8];
	uint32_t	b_t[2];		/* bytes hashed, low word first */
	uint8_t		b_buf[64];
	uint32_t	b_len;		/* bytes waiting in b_buf       */
	uint32_t	b_outlen;
};

static const uint32_t	blake2s_iv[8] = {
	0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
	0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
};

static const uint8_t	blake2s_sigma[10][16] = {
	{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
	{ 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
	{  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
	{  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
	{  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
	{ 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
	{ 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
	{  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
	{ 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
};

static inline uint32_t
ror32(uint32_t v, unsigned int n)
{

	return ((v >> n) | (v << (32 - n)));
}

static inline uint32_t
rol32(uint32_t v, unsigned int n)
{

	return ((v << n) | (v >> (32 - n)));
}

static inline uint32_t
le32(const uint8_t *p)
{

	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static inline void
put_le32(uint8_t *p, uint32_t v)
{

	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Forget a secret: stores the compiler may not drop as dead. */
static void
wipe(void *p, size_t n)
{
	volatile uint8_t	*v;

	for (v = p; n > 0; n--)
		*v++ = 0;
}

#define	B2S_G(a, b, c, d, x, y) do {					\
	v[a] = v[a] + v[b] + (x);	v[d] = ror32(v[d] ^ v[a], 16);	\
	v[c] = v[c] + v[d];		v[b] = ror32(v[b] ^ v[c], 12);	\
	v[a] = v[a] + v[b] + (y);	v[d] = ror32(v[d] ^ v[a], 8);	\
	v[c] = v[c] + v[d];		v[b] = ror32(v[b] ^ v[c], 7);	\
} while (0)

static void
blake2s_compress(struct blake2s *s, const uint8_t *block, bool last)
{
	const uint8_t	*g;
	uint32_t	 m[16];
	uint32_t	 v[16];
	unsigned int	 i;

	for (i = 0; i < 16; i++)
		m[i] = le32(block + 4 * i);
	for (i = 0; i < 8; i++) {
		v[i]     = s->b_h[i];
		v[i + 8] = blake2s_iv[i];
	}
	v[12] ^= s->b_t[0];
	v[13] ^= s->b_t[1];
	if (last)
		v[14] = ~v[14];
	for (i = 0; i < 10; i++) {
		g = blake2s_sigma[i];
		B2S_G(0, 4,  8, 12, m[g[0]],  m[g[1]]);
		B2S_G(1, 5,  9, 13, m[g[2]],  m[g[3]]);
		B2S_G(2, 6, 10, 14, m[g[4]],  m[g[5]]);
		B2S_G(3, 7, 11, 15, m[g[6]],  m[g[7]]);
		B2S_G(0, 5, 10, 15, m[g[8]],  m[g[9]]);
		B2S_G(1, 6, 11, 12, m[g[10]], m[g[11]]);
		B2S_G(2, 7,  8, 13, m[g[12]], m[g[13]]);
		B2S_G(3, 4,  9, 14, m[g[14]], m[g[15]]);
	}
	for (i = 0; i < 8; i++)
		s->b_h[i] ^= v[i] ^ v[i + 8];
	wipe(m, sizeof(m));
	wipe(v, sizeof(v));
}

static void
blake2s_count(struct blake2s *s, uint32_t n)
{

	s->b_t[0] += n;
	if (s->b_t[0] < n)
		s->b_t[1]++;
}

/* An `outlen`-byte hash, keyed if `keylen` is not 0 (at most 32 each). */
static void
blake2s_init(struct blake2s *s, uint32_t outlen, const void *key,
    uint32_t keylen)
{
	unsigned int	i;

	for (i = 0; i < 8; i++)
		s->b_h[i] = blake2s_iv[i];
	s->b_h[0] ^= 0x01010000u ^ keylen << 8 ^ outlen;
	s->b_t[0]   = 0;
	s->b_t[1]   = 0;
	s->b_len    = 0;
	s->b_outlen = outlen;
	for (i = 0; i < sizeof(s->b_buf); i++)
		s->b_buf[i] = 0;
	if (keylen != 0) {
		for (i = 0; i < keylen; i++)
			s->b_buf[i] = ((const uint8_t *)key)[i];
		s->b_len = sizeof(s->b_buf);	/* the key is a whole block */
	}
}

/* The last full block is kept back: only final knows it is the last. */
static void
blake2s_update(struct blake2s *s, const void *in, size_t n)
{
	const uint8_t	*p;
	uint32_t	 take;
	uint32_t	 i;

	for (p = in; n > 0; p += take, n -= take) {
		if (s->b_len == sizeof(s->b_buf)) {
			blake2s_count(s, sizeof(s->b_buf));
			blake2s_compress(s, s->b_buf, false);
			s->b_len = 0;
		}
		take = sizeof(s->b_buf) - s->b_len;
		if (take > n)
			take = (uint32_t)n;
		for (i = 0; i < take; i++)
			s->b_buf[s->b_len + i] = p[i];
		s->b_len += take;
	}
}

static void
blake2s_final(struct blake2s *s, uint8_t *out)
{
	uint8_t		h[32];
	unsigned int	i;

	blake2s_count(s, s->b_len);
	for (i = s->b_len; i < sizeof(s->b_buf); i++)
		s->b_buf[i] = 0;
	blake2s_compress(s, s->b_buf, true);
	for (i = 0; i < 8; i++)
		put_le32(h + 4 * i, s->b_h[i]);
	for (i = 0; i < s->b_outlen; i++)
		out[i] = h[i];
	wipe(h, sizeof(h));
	wipe(s, sizeof(*s));
}

/* ---- ChaCha20 ------------------------------------------------------------ */

#define	CHACHA_QR(a, b, c, d) do {					\
	x[a] += x[b]; x[d] = rol32(x[d] ^ x[a], 16);			\
	x[c] += x[d]; x[b] = rol32(x[b] ^ x[c], 12);			\
	x[a] += x[b]; x[d] = rol32(x[d] ^ x[a], 8);			\
	x[c] += x[d]; x[b] = rol32(x[b] ^ x[c], 7);			\
} while (0)

/* One block: `in` is state words 12..15, the counter and the nonce. */
static void
chacha20_block(const uint32_t key[8], const uint32_t in[4], uint32_t out[16])
{
	uint32_t	s[16];
	uint32_t	x[16];
	unsigned int	i;

	s[0] = 0x61707865;			/* "expand 32-byte k" */
	s[1] = 0x3320646e;
	s[2] = 0x79622d32;
	s[3] = 0x6b206574;
	for (i = 0; i < 8; i++)
		s[4 + i] = key[i];
	for (i = 0; i < 4; i++)
		s[12 + i] = in[i];
	for (i = 0; i < 16; i++)
		x[i] = s[i];
	for (i = 0; i < 10; i++) {
		CHACHA_QR(0, 4,  8, 12);
		CHACHA_QR(1, 5,  9, 13);
		CHACHA_QR(2, 6, 10, 14);
		CHACHA_QR(3, 7, 11, 15);
		CHACHA_QR(0, 5, 10, 15);
		CHACHA_QR(1, 6, 11, 12);
		CHACHA_QR(2, 7,  8, 13);
		CHACHA_QR(3, 4,  9, 14);
	}
	for (i = 0; i < 16; i++)
		out[i] = x[i] + s[i];
	wipe(x, sizeof(x));
	wipe(s, sizeof(s));
}

/* ---- the generator ------------------------------------------------------- */

static struct spinlock	random_lock = SPINLOCK_INIT("random");
static struct blake2s	random_pool;		/* (r) gathered since reseed */
static uint32_t		random_key[8];		/* (r) the generator's key   */
static uint64_t		random_last_ms;		/* (r) when it was reseeded  */
static bool		random_ready;		/* (r) seeded once           */

/* Written by each CPU's interrupts, read by whoever reseeds. */
static uint64_t		random_irq[MAXCPU];	/* (a) */
static uint64_t		random_irq_n[MAXCPU];	/* (a) */

void
random_intr(uint64_t vector, uint64_t rip)
{
	uint64_t	v;
	uint32_t	id;

	id = curcpu()->cp_id;
	if (id >= MAXCPU)
		return;
	v = __atomic_load_n(&random_irq[id], __ATOMIC_RELAXED);
	v = (v << 19 | v >> 45) ^ tsc_read() ^ vector << 56 ^ rip;
	__atomic_store_n(&random_irq[id], v * 0x9E3779B97F4A7C15ULL,
	    __ATOMIC_RELAXED);
	__atomic_store_n(&random_irq_n[id],
	    __atomic_load_n(&random_irq_n[id], __ATOMIC_RELAXED) + 1,
	    __ATOMIC_RELAXED);
}

/* BLAKE2s keyed with `seed`, over one little-endian word: a derived key. */
static void
derive(uint8_t out[32], const uint8_t seed[32], uint64_t label)
{
	struct blake2s	s;

	blake2s_init(&s, 32, seed, 32);
	blake2s_update(&s, &label, sizeof(label));
	blake2s_final(&s, out);
}

static void
reseed_locked(void)
{
	uint8_t		seed[32];
	uint8_t		next[32];
	uint8_t		key[32];
	uint64_t	w;
	unsigned int	i;

	for (i = 0; i < MAXCPU; i++) {
		w = __atomic_load_n(&random_irq[i], __ATOMIC_RELAXED);
		blake2s_update(&random_pool, &w, sizeof(w));
	}
	w = tsc_read();
	blake2s_update(&random_pool, &w, sizeof(w));

	blake2s_final(&random_pool, seed);
	derive(next, seed, 0);
	derive(key, seed, 1);
	blake2s_init(&random_pool, 32, next, 32);
	for (i = 0; i < 8; i++)
		random_key[i] = le32(key + 4 * i);
	random_last_ms = clock_uptime_ms();
	wipe(seed, sizeof(seed));
	wipe(next, sizeof(next));
	wipe(key, sizeof(key));
}

void
random_bytes(void *buf, size_t n)
{
	uint32_t	blk[16];
	uint32_t	k[8];
	uint32_t	in[4];
	uint8_t		*p;
	uint64_t	 ctr;
	size_t		 take;
	unsigned int	 i;

	in[0] = in[1] = in[2] = in[3] = 0;
	spin_lock(&random_lock);
	KASSERT(random_ready, "random_bytes before random_init");
	if (clock_uptime_ms() - random_last_ms >= RANDOM_RESEED_MS)
		reseed_locked();
	chacha20_block(random_key, in, blk);
	for (i = 0; i < 8; i++) {
		random_key[i] = blk[i];
		k[i] = blk[8 + i];
	}
	spin_unlock(&random_lock);

	p = buf;
	for (ctr = 0; n > 0; ctr++, p += take, n -= take) {
		in[0] = (uint32_t)ctr;
		in[1] = (uint32_t)(ctr >> 32);
		chacha20_block(k, in, blk);
		take = n < 64 ? n : 64;
		for (i = 0; i < take; i++)
			p[i] = (uint8_t)(blk[i / 4] >> 8 * (i % 4));
	}
	wipe(blk, sizeof(blk));
	wipe(k, sizeof(k));
}

void
random_add(const void *buf, size_t n)
{

	spin_lock(&random_lock);
	blake2s_update(&random_pool, buf, n);
	spin_unlock(&random_lock);
}

/* ---- boot ---------------------------------------------------------------- */

static bool
rdrand64(uint64_t *v)
{
	unsigned char	ok;

	__asm__ __volatile__ ("rdrand %0; setc %1" : "=r" (*v), "=qm" (ok));
	return (ok != 0);
}

/*
 * The published vectors: BLAKE2s-256 of "abc" (RFC 7693 appendix B), keyed
 * BLAKE2s of nothing under the key 00..1f (the BLAKE2 reference's first
 * keyed vector), and the ChaCha20 block of RFC 8439 section 2.3.2.
 */
static bool
random_kat(void)
{
	static const uint8_t	abc[32] = {
		0x50, 0x8c, 0x5e, 0x8c, 0x32, 0x7c, 0x14, 0xe2,
		0xe1, 0xa7, 0x2b, 0xa3, 0x4e, 0xeb, 0x45, 0x2f,
		0x37, 0x45, 0x8b, 0x20, 0x9e, 0xd6, 0x3a, 0x29,
		0x4d, 0x99, 0x9b, 0x4c, 0x86, 0x67, 0x59, 0x82,
	};
	static const uint8_t	keyed[32] = {
		0x48, 0xa8, 0x99, 0x7d, 0xa4, 0x07, 0x87, 0x6b,
		0x3d, 0x79, 0xc0, 0xd9, 0x23, 0x25, 0xad, 0x3b,
		0x89, 0xcb, 0xb7, 0x54, 0xd8, 0x6a, 0xb7, 0x1a,
		0xee, 0x04, 0x7a, 0xd3, 0x45, 0xfd, 0x2c, 0x49,
	};
	static const uint32_t	block[16] = {
		0xe4e7f110, 0x15593bd1, 0x1fdd0f50, 0xc47120a3,
		0xc7f4d1c7, 0x0368c033, 0x9aaa2204, 0x4e6cd4c3,
		0x466482d2, 0x09aa9f07, 0x05d7c214, 0xa2028bd9,
		0xd19c12b5, 0xb94e16de, 0xe883d0cb, 0x4e3c50a2,
	};
	static const uint32_t	in[4] = { 1, 0x09000000, 0x4a000000, 0 };
	struct blake2s		s;
	uint8_t			key[32];
	uint8_t			h[32];
	uint32_t		k[8];
	uint32_t		out[16];
	unsigned int		i;
	bool			ok;

	ok = true;
	blake2s_init(&s, 32, NULL, 0);
	blake2s_update(&s, "abc", 3);
	blake2s_final(&s, h);
	for (i = 0; i < 32; i++)
		ok = ok && h[i] == abc[i];

	for (i = 0; i < 32; i++)
		key[i] = (uint8_t)i;
	blake2s_init(&s, 32, key, 32);
	blake2s_final(&s, h);
	for (i = 0; i < 32; i++)
		ok = ok && h[i] == keyed[i];

	for (i = 0; i < 8; i++)
		k[i] = le32(key + 4 * i);
	chacha20_block(k, in, out);
	for (i = 0; i < 16; i++)
		ok = ok && out[i] == block[i];
	return (ok);
}

void
random_init(void)
{
	struct blake2s	boot;
	uint8_t		scratch[256];
	uint8_t		a[32];
	uint8_t		b[32];
	uint64_t	acc;
	uint64_t	t0;
	uint64_t	w;
	uint64_t	irqs;
	uint32_t	eax, ebx, ecx, edx;
	unsigned int	i;
	unsigned int	j;
	unsigned int	hw;
	bool		kat;
	bool		differ;

	kat = random_kat();

	/*
	 * Gathered into a hash of its own, with interrupts on: they move
	 * the timings, and the jitter loop is too long to hold a spinlock.
	 */
	blake2s_init(&boot, 32, NULL, 0);

	/* The machine's own source, where it has one (not a Nehalem). */
	hw = 0;
	cpuid_count(1, 0, &eax, &ebx, &ecx, &edx);
	if ((ecx & (1u << 30)) != 0) {
		for (i = 0; i < RANDOM_RDRAND_WORDS; i++) {
			if (rdrand64(&w)) {
				blake2s_update(&boot, &w, sizeof(w));
				hw++;
			}
		}
	}
	w = tsc_read();
	blake2s_update(&boot, &w, sizeof(w));
	w = (uint64_t)clock_walltime_us();
	blake2s_update(&boot, &w, sizeof(w));

	/*
	 * Jitter: how long a walk takes whose length hangs on the walks
	 * before it.  Caches, interrupts and a host's scheduler all move it.
	 */
	for (i = 0; i < sizeof(scratch); i++)
		scratch[i] = (uint8_t)i;
	acc = 0;
	for (i = 0; i < RANDOM_JITTER; i++) {
		t0 = tsc_read();
		for (j = 0; j < 16 + (acc & 15); j++)
			scratch[(acc + j * 61) & 255] += (uint8_t)t0;
		acc = (acc << 7 | acc >> 57) ^ (tsc_read() - t0);
		if ((i & 63) == 63)
			blake2s_update(&boot, &acc, sizeof(acc));
	}
	blake2s_update(&boot, scratch, sizeof(scratch));
	blake2s_final(&boot, a);
	wipe(scratch, sizeof(scratch));

	irqs = 0;
	for (i = 0; i < MAXCPU; i++)
		irqs += __atomic_load_n(&random_irq_n[i], __ATOMIC_RELAXED);

	spin_lock(&random_lock);
	blake2s_init(&random_pool, 32, NULL, 0);
	blake2s_update(&random_pool, a, sizeof(a));
	reseed_locked();
	random_ready = true;
	spin_unlock(&random_lock);

	random_bytes(a, sizeof(a));
	random_bytes(b, sizeof(b));
	differ = false;
	for (i = 0; i < sizeof(a); i++)
		differ = differ || a[i] != b[i];

	if (kat && differ)
		kprintf("random: PASS -- BLAKE2s and ChaCha20 answer their "
		    "published vectors, two draws differ; seeded from %u "
		    "timing samples, %llu interrupts and %u RDRAND words\n",
		    (unsigned)RANDOM_JITTER, (unsigned long long)irqs, hw);
	else
		kprintf("random: FAIL -- %s\n", !kat ?
		    "a primitive disagrees with its published vector" :
		    "two draws came out the same");
	wipe(a, sizeof(a));
	wipe(b, sizeof(b));
}
