#include "algo-gate-api.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "algo/groestl/sph_groestl.h"
#include "algo/blake/sph_blake.h"
#include "algo/bmw/sph_bmw.h"
#include "algo/jh/sph_jh.h"
#include "algo/keccak/sph_keccak.h"
#include "algo/skein/sph_skein.h"
#include "algo/shavite/sph_shavite.h"
#include "algo/luffa/sph_luffa.h"
#include "algo/cubehash/sph_cubehash.h"
#include "algo/simd/sph_simd.h"
#include "algo/echo/sph_echo.h"
#include "algo/hamsi/sph_hamsi.h"
#include "algo/fugue/sph_fugue.h"

#include "algo/luffa/sse2/luffa_for_sse2.h" 
#include "algo/cubehash/sse2/cubehash_sse2.h"
#include "algo/simd/sse2/nist.h"
#include "algo/blake/sse2/blake.c"   
#include "algo/bmw/sse2/bmw.c"
#include "algo/keccak/sse2/keccak.c"
#include "algo/skein/sse2/skein.c"
#include "algo/jh/sse2/jh_sse2_opt64.h"

#ifndef NO_AES_NI
  #include "algo/groestl/aes_ni/hash-groestl.h"
  #include "algo/echo/aes_ni/hash_api.h"
#endif

typedef struct {
#ifdef NO_AES_NI
        sph_groestl512_context   groestl;
        sph_echo512_context      echo;
#else
        hashState_groestl       groestl;
        hashState_echo          echo;
#endif
        hashState_luffa         luffa;
        cubehashParam           cubehash;
        sph_shavite512_context  shavite;
        hashState_sd            simd;
        sph_hamsi512_context    hamsi;
        sph_fugue512_context    fugue;
} x13_ctx_holder;

x13_ctx_holder x13_ctx;

void init_x13_ctx()
{
#ifdef NO_AES_NI
        sph_groestl512_init(&x13_ctx.groestl);
        sph_echo512_init(&x13_ctx.echo);
#else
        init_echo( &x13_ctx.echo, 512 );
        init_groestl (&x13_ctx.groestl, 64 );
#endif
        init_luffa( &x13_ctx.luffa, 512 );
        cubehashInit( &x13_ctx.cubehash, 512, 16, 32 );
        sph_shavite512_init( &x13_ctx.shavite );
        init_sd( &x13_ctx.simd, 512 );
        sph_hamsi512_init( &x13_ctx.hamsi );
        sph_fugue512_init( &x13_ctx.fugue );
};

static void x13hash(void *output, const void *input)
{
	unsigned char hash[128] __attribute__ ((aligned (32)));
	#define hashB hash+64
      
        x13_ctx_holder ctx;
        memcpy( &ctx, &x13_ctx, sizeof(x13_ctx) );

        // X11 algos

        unsigned char hashbuf[128];
        size_t hashptr;
        sph_u64 hashctA;
        sph_u64 hashctB;

        //---blake1---

        DECL_BLK;
        BLK_I;
        BLK_W;
        BLK_C;

        //---bmw2---

        DECL_BMW;
        BMW_I;
        BMW_U;

        #define M(x)    sph_dec64le_aligned(data + 8 * (x))
        #define H(x)    (h[x])
        #define dH(x)   (dh[x])

        BMW_C;

        #undef M
        #undef H
        #undef dH
        
        //---groetl----

#ifdef NO_AES_NI
        sph_groestl512 (&ctx.groestl, hash, 64);
        sph_groestl512_close(&ctx.groestl, hash);
#else
        update_and_final_groestl( &ctx.groestl, (char*)hash,
                                  (const char*)hash, 512 );
#endif

        //---skein4---

        DECL_SKN;
        SKN_I;
        SKN_U;
        SKN_C;

        //---jh5------

        DECL_JH;
        JH_H;

        //---keccak6---

        DECL_KEC;
        KEC_I;
        KEC_U;
        KEC_C;

        //--- luffa7
        update_and_final_luffa( &ctx.luffa, (BitSequence*)hashB,
                                (const BitSequence*)hash, 64 );

        // 8 Cube
        cubehashUpdateDigest( &ctx.cubehash, (byte*) hash,
                              (const byte*)hashB, 64 );

        // 9 Shavite
        sph_shavite512( &ctx.shavite, hash, 64);
        sph_shavite512_close( &ctx.shavite, hashB);

        // 10 Simd
        update_final_sd( &ctx.simd, (BitSequence *)hash,
                         (const BitSequence *)hashB, 512 );

        //11---echo---

#ifdef NO_AES_NI
        sph_echo512(&ctx.echo, hash, 64);
        sph_echo512_close(&ctx.echo, hashB);
#else
        update_final_echo ( &ctx.echo, (BitSequence *)hashB,
                            (const BitSequence *)hash, 512 );
#endif

        // X13 algos
        // 12 Hamsi
	sph_hamsi512(&ctx.hamsi, hashB, 64);
	sph_hamsi512_close(&ctx.hamsi, hash);

        // 13 Fugue
	sph_fugue512(&ctx.fugue, hash, 64);
	sph_fugue512_close(&ctx.fugue, hashB);

        asm volatile ("emms");
	memcpy(output, hashB, 32);
}

int scanhash_x13(int thr_id, struct work *work, uint32_t max_nonce,
                               uint64_t *hashes_done)
{
        uint32_t endiandata[20] __attribute__((aligned(64)));
        uint32_t hash64[8] __attribute__((aligned(64)));
        uint32_t *pdata = work->data;
        uint32_t *ptarget = work->target;
	uint32_t n = pdata[19] - 1;
	const uint32_t first_nonce = pdata[19];
	const uint32_t Htarg = ptarget[7];

	uint64_t htmax[] = {
		0,
		0xF,
		0xFF,
		0xFFF,
		0xFFFF,
		0x10000000
	};
	uint32_t masks[] = {
		0xFFFFFFFF,
		0xFFFFFFF0,
		0xFFFFFF00,
		0xFFFFF000,
		0xFFFF0000,
		0
	};

	// we need bigendian data...
        swab32_array( endiandata, pdata, 20 );

#ifdef DEBUG_ALGO
	printf("[%d] Htarg=%X\n", thr_id, Htarg);
#endif
   for (int m=0; m < 6; m++) {
      if (Htarg <= htmax[m]) {
        uint32_t mask = masks[m];
	do {
	   pdata[19] = ++n;
	   be32enc(&endiandata[19], n);
	   x13hash(hash64, endiandata);
#ifndef DEBUG_ALGO
	   if (!(hash64[7] & mask))
           { 
              if ( fulltest(hash64, ptarget) )
              {
	     	*hashes_done = n - first_nonce + 1;
	 	return true;
              }
//                                   else
//                                  {
//                                      applog(LOG_INFO, "Result does not validate on CPU!");
//                                  }
            }
                                   
#else
	    if (!(n % 0x1000) && !thr_id) printf(".");
		if (!(hash64[7] & mask)) {
			printf("[%d]",thr_id);
			if (fulltest(hash64, ptarget)) {
				*hashes_done = n - first_nonce + 1;
				return true;
			}
		}
#endif
	} while (n < max_nonce && !work_restart[thr_id].restart);
			// see blake.c if else to understand the loop on htmax => mask
	break;
     }
  }

  *hashes_done = n - first_nonce + 1;
  pdata[19] = n;
  return 0;
}


bool register_x13_algo( algo_gate_t* gate )
{
  gate->optimizations = SSE2_OPT | AES_OPT | AVX_OPT | AVX2_OPT;
  init_x13_ctx();
  gate->scanhash = (void*)&scanhash_x13;
  gate->hash     = (void*)&x13hash;
  gate->get_max64 = (void*)&get_max64_0x3ffff;
  return true;
};

