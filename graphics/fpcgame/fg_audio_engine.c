/* TRI's sound engine (TRI firmware/vdp2/audio.c, as of TRI de2286d), for the FPC game SDK:
 * only its includes changed; fg_audio_engine.h stands in for the Pico SDK and VDP2 */
#include <string.h>
#include "fg_audio_engine.h"   /* (FPC: was pico.h, pico/stdlib.h, audio.h, vdp2.h) */
#include "tri_isa.h"

// ---- DSP helpers ------------------------------------------------------------------
// The mixer leans on the Armv8-M DSP extension: SMLAWB/SMLAWT (32x16 -> top word of
// the 48-bit product, accumulated) apply a 32-bit gain to a 16-bit sample in one
// cycle, SMULWB is the same without the add, SMUAD is a dual 16x16 multiply-add,
// SSAT clips. Plain C fallbacks keep the file buildable on a host for A/B tests
// against synthetic voices.
#if defined(__ARM_FEATURE_DSP)
#include <arm_acle.h>
#define SMLAWB(g, s, a) __smlawb((g), (s), (a))         // a + (g * (int16)s) >> 16
#define SMLAWT(g, s, a) __smlawt((g), (s), (a))         // a + (g * (int16)(s >> 16)) >> 16
#define SMUAD(a, b) __smuad((int16x2_t)(a), (int16x2_t)(b))   // a.lo * b.lo + a.hi * b.hi
#define SSAT16(x) __ssat((x), 16)
static inline int32_t SMULWB(int32_t g, int32_t s) {    // (g * (int16)s) >> 16; no ACLE intrinsic for it
	int32_t r;
	__asm__("smulwb %0, %1, %2" : "=r"(r) : "r"(g), "r"(s));
	return r;
}
#else
static inline int32_t SMLAWB(int32_t g, int32_t s, int32_t a) { return a + (int32_t)(((int64_t)g * (int16_t)s) >> 16); }
static inline int32_t SMLAWT(int32_t g, int32_t s, int32_t a) { return a + (int32_t)(((int64_t)g * (int16_t)(s >> 16)) >> 16); }
static inline int32_t SMUAD(int32_t a, int32_t b) { return (int16_t)a * (int16_t)b + (int16_t)(a >> 16) * (int16_t)(b >> 16); }
static inline int32_t SMULWB(int32_t g, int32_t s) { return (int32_t)(((int64_t)g * (int16_t)s) >> 16); }
static inline int32_t SSAT16(int32_t x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }
#endif
// lo | hi << 16 as one PKHBT (GCC spells the C form as uxth + orr)
#if defined(__ARM_FEATURE_DSP)
static inline int32_t PACK16(int32_t lo, int32_t hi) {
	int32_t r;
	__asm__("pkhbt %0, %1, %2, lsl #16" : "=r"(r) : "r"(lo), "r"(hi));
	return r;
}
#else
#define PACK16(lo, hi) ((int32_t)(((uint32_t)(lo) & 0xFFFF) | ((uint32_t)(hi) << 16)))
#endif
#define INLINE static inline __attribute__((always_inline))

// ---- fixed-point conventions --------------------------------------------------------
// Block: BLOCK frames. Envelopes, gains and volumes are sampled once per block.
// FM phase: 32-bit accumulator, top 10 bits index a 1024-entry table. Feedback and
// modulation are added to the phase, so the wrap is the 32-bit wrap and the index
// never needs masking.
// Operator output: t * g, t = table sample (+-32767), g = env^2 * level (0..65535),
// kept as the 32-bit product; the mix takes the top word of a 48-bit product, so the
// carrier never loses bits to a 16-bit intermediate.
// Envelope level: 0..65535.
// PCM position: 24.8 (sample index . fraction).
// Mix: one int32 accumulator per channel per frame, in units of output LSB / 2^11:
// acc = sample * vol * master * 8 for PCM, and the same scale for FM through the
// per-block gain (voice volume and master folded in). 16 voices at full scale stay
// under 2^30; the output stage shifts by ACC_SHIFT and saturates. This keeps the
// per-frame cost at two SMLAWB per voice and one SSAT per channel.
#define BLOCK 8
#define WAVE_BITS 10
#define WAVE_N (1 << WAVE_BITS)
#define PH_SHIFT (32 - WAVE_BITS)
#define ACC_SHIFT 11

typedef struct { uint32_t off, len, loop, rate; bool valid; } sample_t;

typedef struct {
	uint8_t mult, wave, level;
	int8_t detune;
	uint32_t att, dec, sus, rel;   // envelope steps per block (sus: level), from the ADSR bytes
} op_t;
typedef struct { uint8_t alg, fb; op_t op[2]; bool valid; } patch_t;

enum { V_FREE = 0, V_PCM, V_FM };
enum { E_OFF = 0, E_ATTACK, E_DECAY, E_SUSTAIN, E_RELEASE };

// 64 bytes each so &voices[i] is one shift-add in the hot loop.
typedef struct {
	uint8_t mode, vol_l, vol_r, patch;
	union {
		struct {                       // V_PCM
			const int8_t *pcm;
			uint32_t pos, step;            // 24.8
			uint32_t len8, loop8;          // 24.8 (loop8 = 0xFFFFFFFF: one-shot)
		};
		struct {                       // V_FM
			uint32_t ph[2], inc[2];
			const int16_t *tab[2];
			uint32_t env[2];               // 0..65535
			uint8_t estate[2];
			uint8_t fb_shift;              // 0 = none, else index += (fb1 + fb2) >> fb_shift
			int32_t fb1, fb2;              // last two op0 outputs, >> 16
		};
	};
} __attribute__((aligned(64))) voice_t;

static vdp2_t *VDP;                       // the VRAM map (SRAM + PSRAM) the samples live in
static sample_t samples[TRI_AUDIO_SAMPLES];
static patch_t patches[TRI_AUDIO_FM_PATCHES];
static voice_t voices[TRI_AUDIO_VOICES];
static uint32_t live;                                     // bit per voice with mode != V_FREE
static uint32_t master_l = 255, master_r = 255;
static vdp2_audio_stats_t ST;

// waveform tables (sine, half-sine, abs-sine, pulse-sine), int16, built at init: 8 KB in SRAM
static int16_t wave[4][WAVE_N] __attribute__((aligned(4)));

static void build_tables(void) {
	// sine from a 5-term Taylor series on [0, pi/2] in Q16 using 1/4-wave symmetry
	// (init only, accuracy is plenty)
	for (int i = 0; i < WAVE_N; i++) {
		int q = i & 255, quadrant = i >> 8;
		int32_t x = (int32_t)(((int64_t)q * 102943) >> 8);      // q/256 * pi/2 in Q16 (pi/2 = 102943.7 Q16)
		if (quadrant & 1) x = 102944 - x;
		int64_t x2 = ((int64_t)x * x) >> 16;
		int64_t term = x, sum = x;
		term = -(term * x2 >> 16) / 6;   sum += term;             // x^3/3!
		term = -(term * x2 >> 16) / 20;  sum += term;             // x^5/5!
		term = -(term * x2 >> 16) / 42;  sum += term;             // x^7/7!
		term = -(term * x2 >> 16) / 72;  sum += term;             // x^9/9!
		int32_t s = (int32_t)((sum * 32767) >> 16);
		if (s > 32767) s = 32767;
		if (quadrant >= 2) s = -s;
		wave[0][i] = (int16_t)s;
		wave[1][i] = (int16_t)(s < 0 ? 0 : s);                    // half-sine
		wave[2][i] = (int16_t)(s < 0 ? -s : s);                   // abs-sine
		wave[3][i] = (int16_t)((i & 256) ? 0 : (s < 0 ? -s : s));  // pulse-sine
	}
}

void vdp2_audio_init(vdp2_t *vdp) {
	VDP = vdp;
	memset(samples, 0, sizeof samples);
	memset(patches, 0, sizeof patches);
	memset(voices, 0, sizeof voices);
	memset(&ST, 0, sizeof ST);
	live = 0;
	master_l = master_r = 255;
	build_tables();
}

void vdp2_audio_reset(void) {
	memset(voices, 0, sizeof voices);
	live = 0;
	master_l = master_r = 255;
}

const vdp2_audio_stats_t *vdp2_audio_stats(void) { return &ST; }
void vdp2_audio_stats_reset(void) { ST.render_us_max = 0; ST.render_us_sum = 0; ST.render_calls = 0; }

// ---- envelopes --------------------------------------------------------------------
// Rate r (0..255) -> level change per block: r*r/4 + 1. 255: 4 blocks (0.7 ms),
// 128: 2.7 ms, 64: 11 ms, 32: 42 ms, 16: 170 ms, 8: 0.6 s, 4: 2.2 s, 0: 11 s.
// Attack runs 4x faster than the table.
static inline uint32_t rate_inc(uint32_t r) { return (r * r >> 2) + 1; }

static inline void env_key_on(voice_t *v, int i) {
	v->env[i] = 0;
	v->estate[i] = E_ATTACK;
}

// One block of envelope; returns the operator gain env^2 * level, 0..65535.
INLINE uint32_t env_step(voice_t *v, const op_t *o, int i) {
	uint32_t e = v->env[i];
	switch (v->estate[i]) {
	case E_ATTACK:
		e += o->att;
		if (e >= 65535) { e = 65535; v->estate[i] = E_DECAY; }
		break;
	case E_DECAY:
		if (e <= o->sus + o->dec) { e = o->sus; v->estate[i] = E_SUSTAIN; }
		else e -= o->dec;
		break;
	case E_SUSTAIN:
		break;
	case E_RELEASE:
		if (e <= o->rel) { e = 0; v->estate[i] = E_OFF; }
		else e -= o->rel;
		break;
	default:
		e = 0;
		break;
	}
	v->env[i] = e;
	return ((e * e) >> 16) * o->level >> 8;
}

// ---- frequency -> phase increments (note-on only; 64-bit math is fine here) ------
static uint32_t phase_inc(uint32_t freq16, const op_t *o) {
	// inc = f / 48000 * 2^32 = freq16 * 2^16 / 48000
	uint64_t inc = ((uint64_t)freq16 << 16) / TRI_AUDIO_RATE;
	inc = o->mult ? inc * o->mult : inc >> 1;
	int64_t det = (int64_t)inc * o->detune >> 10;       // +-0.8% per detune step
	return (uint32_t)(inc + det);
}

static void fm_set_freq(voice_t *v, uint32_t freq16) {
	const patch_t *p = &patches[v->patch];
	v->inc[0] = phase_inc(freq16, &p->op[0]);
	v->inc[1] = phase_inc(freq16, &p->op[1]);
}

// ---- commands -----------------------------------------------------------------------
// Commands and rendering both run on core 0 between scanlines, never interleaved,
// so a voice can be rewritten in place.
bool vdp2_audio_exec(uint32_t op, uint32_t imm, const uint32_t *p, uint32_t nw) {
	switch (op) {
	case TRI_OP_AUDIO_SAMPLE_DEF: {
		if (nw < 4 || imm >= TRI_AUDIO_SAMPLES) break;
		uint32_t off = p[0], len = p[1], loop = p[2], rate = p[3];
		if (len == 0 || !vdp2_vram_range(VDP, off, len)) break;
		if (loop != 0xFFFFFFFFu && loop >= len) break;
		if (rate < 1000 || rate > 192000) break;
		samples[imm] = (sample_t){ off, len, loop, rate, true };
		return true;
	}
	case TRI_OP_AUDIO_PLAY: {
		uint32_t vi = imm & 15, si = (imm >> 4) & 63;
		if (nw < 2 || si >= TRI_AUDIO_SAMPLES || !samples[si].valid) break;
		const sample_t *s = &samples[si];
		voice_t *v = &voices[vi];
		uint32_t pitch = p[0];
		if (pitch == 0 || pitch > (16u << 16)) break;
		v->pcm = (const int8_t *)vdp2_vram_base(VDP, s->off);
		v->pos = 0;
		v->step = (uint32_t)(((uint64_t)s->rate * pitch) / (TRI_AUDIO_RATE * 256u));   // 24.8
		if (v->step == 0) v->step = 1;
		v->len8 = s->len << 8;
		v->loop8 = s->loop == 0xFFFFFFFFu ? 0xFFFFFFFFu : s->loop << 8;
		v->vol_l = (uint8_t)p[1];
		v->vol_r = (uint8_t)(p[1] >> 8);
		v->mode = V_PCM;
		live |= 1u << vi;
		return true;
	}
	case TRI_OP_AUDIO_STOP: {
		if (imm >= TRI_AUDIO_VOICES) break;
		voices[imm].mode = V_FREE;
		live &= ~(1u << imm);
		return true;
	}
	case TRI_OP_AUDIO_VOL: {
		if (nw < 1) break;
		if (imm == 0xFFFF) { master_l = (uint8_t)p[0]; master_r = (uint8_t)(p[0] >> 8); return true; }
		if (imm >= TRI_AUDIO_VOICES) break;
		voices[imm].vol_l = (uint8_t)p[0];
		voices[imm].vol_r = (uint8_t)(p[0] >> 8);
		return true;
	}
	case TRI_OP_AUDIO_FM_PATCH: {
		if (nw < 5 || imm >= TRI_AUDIO_FM_PATCHES) break;
		patch_t *pt = &patches[imm];
		pt->alg = p[0] & 1;
		pt->fb = (p[0] >> 4) & 7;
		for (int i = 0; i < 2; i++) {
			uint32_t w0 = p[1 + 2 * i], w1 = p[2 + 2 * i];
			op_t *o = &pt->op[i];
			o->mult = w0 & 15;
			o->detune = (int8_t)((int)((w0 >> 4) & 15) - (((w0 >> 4) & 8) ? 16 : 0));   // signed nibble
			o->wave = (w0 >> 8) & 3;
			o->level = (uint8_t)(w0 >> 16);
			o->att = rate_inc(w1 & 255) * 4;
			o->dec = rate_inc((w1 >> 8) & 255);
			o->sus = ((w1 >> 16) & 255) << 8;
			o->rel = rate_inc(w1 >> 24);
		}
		pt->valid = true;
		return true;
	}
	case TRI_OP_AUDIO_FM_ON: {
		uint32_t vi = imm & 15, pi = (imm >> 4) & 31;
		if (nw < 2 || !patches[pi].valid) break;
		uint32_t freq16 = p[0];
		if (freq16 < (1u << 16) || freq16 > (20000u << 16)) break;
		voice_t *v = &voices[vi];
		const patch_t *pt = &patches[pi];
		v->patch = (uint8_t)pi;
		v->tab[0] = wave[pt->op[0].wave];
		v->tab[1] = wave[pt->op[1].wave];
		v->fb_shift = pt->fb ? 11 - pt->fb : 0;
		v->fb1 = v->fb2 = 0;
		v->ph[0] = v->ph[1] = 0;
		fm_set_freq(v, freq16);
		env_key_on(v, 0);
		env_key_on(v, 1);
		v->vol_l = (uint8_t)p[1];
		v->vol_r = (uint8_t)(p[1] >> 8);
		v->mode = V_FM;
		live |= 1u << vi;
		return true;
	}
	case TRI_OP_AUDIO_FM_OFF: {
		if (imm >= TRI_AUDIO_VOICES) break;
		voice_t *v = &voices[imm];
		if (v->mode == V_FM) { v->estate[0] = E_RELEASE; v->estate[1] = E_RELEASE; }
		return true;
	}
	case TRI_OP_AUDIO_FM_FREQ: {
		if (nw < 1 || imm >= TRI_AUDIO_VOICES) break;
		voice_t *v = &voices[imm];
		uint32_t freq16 = p[0];
		if (freq16 < (1u << 16) || freq16 > (20000u << 16)) break;
		if (v->mode == V_FM) fm_set_freq(v, freq16);
		return true;
	}
	default:
		break;
	}
	ST.bad_cmds++;
	return false;
}

// ---- rendering ------------------------------------------------------------------------
// All of it lives in RAM. render_block owns the stereo accumulator acc[2 * BLOCK]
// (L, R interleaved); the PCM loop is inlined into it, the FM loops are leaf
// functions (see RAMFN). Loops unroll by 2 only: a full unroll made GCC hoist all
// eight phases and spill everything.

// PCM: acc += s * gain, gain = vol * master << 3. Returns false when a one-shot ends.
INLINE bool render_pcm(voice_t *v, int32_t *acc, int32_t gl, int32_t gr) {
	uint32_t pos = v->pos, step = v->step, len8 = v->len8;
	const int8_t *pcm = v->pcm;
	if (pos + step * (BLOCK - 1) < len8) {              // whole block inside the sample: no end checks
		int n = BLOCK;
#pragma GCC unroll 2
		do {
			int32_t s = pcm[pos >> 8];
			acc[0] += s * gl;
			acc[1] += s * gr;
			acc += 2;
			pos += step;
		} while (--n);
		v->pos = pos;
		return true;
	}
	uint32_t loop8 = v->loop8;
#pragma GCC unroll 1
	for (int n = BLOCK; n; n--) {                        // the end falls in this block
		if (pos >= len8) {
			if (loop8 == 0xFFFFFFFFu) { v->pos = pos; return false; }
			do pos -= len8 - loop8; while (pos >= len8);   // loops shorter than a step wrap more than once
		}
		int32_t s = pcm[pos >> 8];
		acc[0] += s * gl;
		acc[1] += s * gr;
		acc += 2;
		pos += step;
	}
	v->pos = pos;
	return true;
}

// FM, one block. Four loops, each a leaf function: alg 0 with and without feedback,
// alg 1 without (dual MAC) and with (two single-operator passes). fb is a
// compile-time constant.
// g0 is op0's raw gain (feedback and modulation use the operator output before
// any volume); carrier gains include voice volume and master (conventions above):
// acc += (g * t) >> 16.
// Feedback matches the old 16-bit form bit for bit: index += (fb1 + fb2) >> fb_shift
// with fb = out >> 16. Modulation is added to the carrier phase as out << 2, which
// is index += out >> 20 without truncating phase and output separately first.
typedef struct { int32_t g0, gl0, gr0, gl1, gr1; } fm_gains_t;

// alg 0: op0 modulates op1, op1 into the mix.
INLINE void fm_mod_block(voice_t *v, int32_t *acc, int fb, const fm_gains_t *g) {
	uint32_t ph0 = v->ph[0], ph1 = v->ph[1], inc0 = v->inc[0], inc1 = v->inc[1];
	const int16_t *t0 = v->tab[0], *t1 = v->tab[1];
	int32_t g0 = g->g0, gl = g->gl1, gr = g->gr1;
	int32_t fb1 = v->fb1, fb2 = v->fb2;
	uint32_t fbs = v->fb_shift;
	int n = BLOCK;
#pragma GCC unroll 2
	do {
		uint32_t p0 = ph0;
		if (fb) p0 += (uint32_t)((fb1 + fb2) >> fbs) << PH_SHIFT;
		int32_t s0 = t0[p0 >> PH_SHIFT];
		if (fb) { fb2 = fb1; fb1 = SMULWB(g0, s0); }
		int32_t t = t1[(ph1 + ((uint32_t)(s0 * g0) << 2)) >> PH_SHIFT];
		acc[0] = SMLAWB(gl, t, acc[0]);
		acc[1] = SMLAWB(gr, t, acc[1]);
		acc += 2;
		ph0 += inc0; ph1 += inc1;
	} while (--n);
	v->ph[0] = ph0; v->ph[1] = ph1;
	if (fb) { v->fb1 = fb1; v->fb2 = fb2; }
}

// alg 1, no feedback: both operators into the mix. The two samples and the two
// gains travel as halfword pairs so SMUAD sums t0*g0 + t1*g1 in one instruction
// (gains at 15 bits: g >> 1 keeps the pair signed and the sum under 2^31); the
// stereo volume pair then goes through SMLAWB/SMLAWT. Six fewer live registers
// than four separate gains, which is what keeps this loop free of spills.
INLINE void fm_add_block(voice_t *v, int32_t *acc, int32_t gpack, int32_t vpack) {
	uint32_t ph0 = v->ph[0], ph1 = v->ph[1], inc0 = v->inc[0], inc1 = v->inc[1];
	const int16_t *t0 = v->tab[0], *t1 = v->tab[1];
	int n = BLOCK;
#pragma GCC unroll 2
	do {
		int32_t s = SMUAD(PACK16(t0[ph0 >> PH_SHIFT], t1[ph1 >> PH_SHIFT]), gpack) >> 4;
		acc[0] = SMLAWB(s, vpack, acc[0]);
		acc[1] = SMLAWT(s, vpack, acc[1]);
		acc += 2;
		ph0 += inc0; ph1 += inc1;
	} while (--n);
	v->ph[0] = ph0; v->ph[1] = ph1;
}

// One operator on its own into the mix (alg 1 with feedback: op0 with feedback and
// op1 plain need more registers together than there are, so two passes).
INLINE void fm_op_block(voice_t *v, int32_t *acc, int i, int fb, const fm_gains_t *g) {
	uint32_t ph = v->ph[i], inc = v->inc[i];
	const int16_t *t = v->tab[i];
	int32_t g0 = g->g0, gl = i ? g->gl1 : g->gl0, gr = i ? g->gr1 : g->gr0;
	int32_t fb1 = v->fb1, fb2 = v->fb2;
	uint32_t fbs = v->fb_shift;
	int n = BLOCK;
#pragma GCC unroll 2
	do {
		uint32_t p = ph;
		if (fb) p += (uint32_t)((fb1 + fb2) >> fbs) << PH_SHIFT;
		int32_t s = t[p >> PH_SHIFT];
		if (fb) { fb2 = fb1; fb1 = SMULWB(g0, s); }
		acc[0] = SMLAWB(gl, s, acc[0]);
		acc[1] = SMLAWB(gr, s, acc[1]);
		acc += 2;
		ph += inc;
	} while (--n);
	v->ph[i] = ph;
	if (fb) { v->fb1 = fb1; v->fb2 = fb2; }
}

// The FM loops are out of line: inlined into render_block they inherit its register
// pressure and spill inside the loop; as leaf functions they fit in registers, and
// the call costs less than the spills did.
#define RAMFN(name) static void __attribute__((noinline)) __not_in_flash_func(name)
RAMFN(fm_mod)(voice_t *v, int32_t *acc, const fm_gains_t *g) { fm_mod_block(v, acc, 0, g); }
RAMFN(fm_mod_fb)(voice_t *v, int32_t *acc, const fm_gains_t *g) { fm_mod_block(v, acc, 1, g); }
RAMFN(fm_add)(voice_t *v, int32_t *acc, int32_t gpack, int32_t vpack) { fm_add_block(v, acc, gpack, vpack); }
RAMFN(fm_add_fb)(voice_t *v, int32_t *acc, const fm_gains_t *g) {
	fm_op_block(v, acc, 0, 1, g);
	fm_op_block(v, acc, 1, 0, g);
}

// Returns false once the voice is silent and released (carrier for alg 0, both for alg 1).
INLINE bool render_fm(voice_t *v, int32_t *acc, uint32_t vl, uint32_t vr) {
	const patch_t *pt = &patches[v->patch];
	uint32_t g0 = env_step(v, &pt->op[0], 0), g1 = env_step(v, &pt->op[1], 1);
	// carrier gain * vol * master is 16 x 16 bits; >> 5 keeps it under 2^31 and lands
	// the SMLAWB result in acc units (>> 6: two carriers at half level each)
	if (pt->alg == 0) {
		fm_gains_t g = { .g0 = (int32_t)g0, .gl1 = (int32_t)(g1 * vl >> 5), .gr1 = (int32_t)(g1 * vr >> 5) };
		if (v->fb_shift) fm_mod_fb(v, acc, &g);
		else fm_mod(v, acc, &g);
		return v->estate[1] != E_OFF;
	}
	if (v->fb_shift) {
		fm_gains_t g = { (int32_t)g0, (int32_t)(g0 * vl >> 6), (int32_t)(g0 * vr >> 6),
		                 (int32_t)(g1 * vl >> 6), (int32_t)(g1 * vr >> 6) };
		fm_add_fb(v, acc, &g);
	} else {
		// (t0 * g0/2 + t1 * g1/2) / 16 * (vol/2) >> 16 == (o0 + o1) * vol >> 22, the alg 1 scale
		fm_add(v, acc, PACK16(g0 >> 1, g1 >> 1), PACK16(vl >> 1, vr >> 1));
	}
	return v->estate[0] != E_OFF || v->estate[1] != E_OFF;
}

// One block: mix every live voice into acc[], then shift, saturate and write BLOCK frames.
static uint32_t __attribute__((noinline)) __not_in_flash_func(render_block)(int16_t *lr) {
	int32_t acc[2 * BLOCK];
	uint32_t active = 0;
	for (int i = 0; i < 2 * BLOCK; i += 2) { acc[i] = 0; acc[i + 1] = 0; }
	for (uint32_t m = live; m; m &= m - 1) {
		uint32_t vi = (uint32_t)__builtin_ctz(m);
		voice_t *v = &voices[vi];
		uint32_t vl = v->vol_l * master_l, vr = v->vol_r * master_r;   // 0..65025
		bool alive;
		if (v->mode == V_PCM) alive = render_pcm(v, acc, (int32_t)(vl << 3), (int32_t)(vr << 3));
		else alive = render_fm(v, acc, vl, vr);
		if (alive) active++;
		else { v->mode = V_FREE; live &= ~(1u << vi); }
	}
	for (int i = 0; i < 2 * BLOCK; i += 2) {
		lr[i] = (int16_t)SSAT16(acc[i] >> ACC_SHIFT);
		lr[i + 1] = (int16_t)SSAT16(acc[i + 1] >> ACC_SHIFT);
	}
	return active;
}

void __not_in_flash_func(vdp2_audio_render)(int16_t *lr, int frames) {
	uint32_t t0 = time_us_32();
	uint32_t active = 0;
	int done = 0;
	for (; done + BLOCK <= frames; done += BLOCK, lr += 2 * BLOCK) active = render_block(lr);
	if (done < frames) {                                   // trailing partial block: render whole, copy what fits
		int16_t tail[2 * BLOCK];
		active = render_block(tail);
		memcpy(lr, tail, (size_t)(frames - done) * 2 * sizeof(int16_t));
	}
	uint32_t dt = time_us_32() - t0;
	ST.active_voices = active;
	if (dt > ST.render_us_max) ST.render_us_max = dt;
	ST.render_us_sum += dt;
	ST.render_calls++;
	ST.frames_rendered += (uint32_t)frames;
}
