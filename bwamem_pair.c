#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "kstring.h"
#include "bwamem.h"
#include "kvec.h"
#include "utils.h"
#include "ksw.h"

#define MIN_RATIO     0.8
#define MIN_DIR_CNT   10
#define MIN_DIR_RATIO 0.05
#define OUTLIER_BOUND 2.0
#define MAPPING_BOUND 3.0
#define MAX_STDDEV    4.0
#define EXT_STDDEV    4.0

void bwa_hit2sam(kstring_t *str, const int8_t mat[25], int q, int r, int w, const bntseq_t *bns, const uint8_t *pac, bseq1_t *s, bwahit_t *p, int is_hard);

static int cal_sub(const mem_opt_t *opt, mem_alnreg_v *r)
{
	int j;
	for (j = 1; j < r->n; ++j) { // choose unique alignment
		int b_max = r->a[j].qb > r->a[0].qb? r->a[j].qb : r->a[0].qb;
		int e_min = r->a[j].qe < r->a[0].qe? r->a[j].qe : r->a[0].qe;
		if (e_min > b_max) { // have overlap
			int min_l = r->a[j].qe - r->a[j].qb < r->a[0].qe - r->a[0].qb? r->a[j].qe - r->a[j].qb : r->a[0].qe - r->a[0].qb;
			if (e_min - b_max >= min_l * opt->mask_level) break; // significant overlap
		}
	}
	return j < r->n? r->a[j].score : opt->min_seed_len * opt->a;
}

void mem_pestat(const mem_opt_t *opt, int64_t l_pac, int n, const mem_alnreg_v *regs, mem_pestat_t pes[4])
{
	int i, d, max;
	uint64_v isize[4];
	memset(pes, 0, 4 * sizeof(mem_pestat_t));
	memset(isize, 0, sizeof(kvec_t(int)) * 4);
	for (i = 0; i < n>>1; ++i) {
		int dir;
		int64_t is;
		mem_alnreg_v *r[2];
		r[0] = (mem_alnreg_v*)&regs[i<<1|0];
		r[1] = (mem_alnreg_v*)&regs[i<<1|1];
		if (r[0]->n == 0 || r[1]->n == 0) continue;
		if (cal_sub(opt, r[0]) > MIN_RATIO * r[0]->a[0].score) continue;
		if (cal_sub(opt, r[1]) > MIN_RATIO * r[1]->a[0].score) continue;
		dir = mem_infer_dir(l_pac, r[0]->a[0].rb, r[1]->a[0].rb, &is);
		if (is && is <= opt->max_ins) kv_push(uint64_t, isize[dir], is);
	}
	if (mem_verbose >= 3) fprintf(stderr, "[M::%s] # candidate unique pairs for (FF, FR, RF, RR): (%ld, %ld, %ld, %ld)\n", __func__, isize[0].n, isize[1].n, isize[2].n, isize[3].n);
	for (d = 0; d < 4; ++d) { // TODO: this block is nearly identical to the one in bwtsw2_pair.c. It would be better to merge these two.
		mem_pestat_t *r = &pes[d];
		uint64_v *q = &isize[d];
		int p25, p50, p75, x;
		if (q->n < MIN_DIR_CNT) {
			fprintf(stderr, "[M::%s] skip orientation %c%c as there are not enough pairs\n", __func__, "FR"[d>>1&1], "FR"[d&1]);
			r->failed = 1;
			continue;
		} else fprintf(stderr, "[M::%s] analyzing insert size distribution for orientation %c%c...\n", __func__, "FR"[d>>1&1], "FR"[d&1]);
		ks_introsort_64(q->n, q->a);
		p25 = q->a[(int)(.25 * q->n + .499)];
		p50 = q->a[(int)(.50 * q->n + .499)];
		p75 = q->a[(int)(.75 * q->n + .499)];
		r->low  = (int)(p25 - OUTLIER_BOUND * (p75 - p25) + .499);
		if (r->low < 1) r->low = 1;
		r->high = (int)(p75 + OUTLIER_BOUND * (p75 - p25) + .499);
		fprintf(stderr, "[M::%s] (25, 50, 75) percentile: (%d, %d, %d)\n", __func__, p25, p50, p75);
		fprintf(stderr, "[M::%s] low and high boundaries for computing mean and std.dev: (%d, %d)\n", __func__, r->low, r->high);
		for (i = x = 0, r->avg = 0; i < q->n; ++i)
			if (q->a[i] >= r->low && q->a[i] <= r->high)
				r->avg += q->a[i], ++x;
		r->avg /= x;
		for (i = 0, r->std = 0; i < q->n; ++i)
			if (q->a[i] >= r->low && q->a[i] <= r->high)
				r->std += (q->a[i] - r->avg) * (q->a[i] - r->avg);
		r->std = sqrt(r->std / x);
		fprintf(stderr, "[M::%s] mean and std.dev: (%.2f, %.2f)\n", __func__, r->avg, r->std);
		r->low  = (int)(p25 - MAPPING_BOUND * (p75 - p25) + .499);
		r->high = (int)(p75 + MAPPING_BOUND * (p75 - p25) + .499);
		if (r->low  > r->avg - MAX_STDDEV * r->std) r->low  = (int)(r->avg - MAX_STDDEV * r->std + .499);
		if (r->high < r->avg - MAX_STDDEV * r->std) r->high = (int)(r->avg + MAX_STDDEV * r->std + .499);
		if (r->low < 1) r->low = 1;
		fprintf(stderr, "[M::%s] low and high boundaries for proper pairs: (%d, %d)\n", __func__, r->low, r->high);
		free(q->a);
	}
	for (d = 0, max = 0; d < 4; ++d)
		max = max > isize[d].n? max : isize[d].n;
	for (d = 0; d < 4; ++d)
		if (pes[d].failed == 0 && isize[d].n < max * MIN_DIR_RATIO) {
			pes[d].failed = 1;
			fprintf(stderr, "[M::%s] skip orientation %c%c\n", __func__, "FR"[d>>1&1], "FR"[d&1]);
		}
}

int mem_matesw(const mem_opt_t *opt, int64_t l_pac, const uint8_t *pac, const mem_pestat_t pes[4], const mem_alnreg_t *a, int l_ms, const uint8_t *ms, mem_alnreg_v *ma)
{
	int i, r, skip[4], n = 0;
	for (r = 0; r < 4; ++r)
		skip[r] = pes[r].failed? 1 : 0;
	for (i = 0; i < ma->n; ++i) { // check which orinentation has been found
		int64_t dist;
		r = mem_infer_dir(l_pac, a->rb, ma->a[i].rb, &dist);
		if (dist >= pes[r].low && dist <= pes[r].high)
			skip[r] = 1;
	}
	if (skip[0] + skip[1] + skip[2] + skip[3] == 4) return 0; // consistent pair exist; no need to perform SW
	for (r = 0; r < 4; ++r) {
		int is_rev, is_larger;
		uint8_t *seq, *rev = 0, *ref;
		int64_t rb, re, len;
		if (skip[r]) continue;
		is_rev = (r>>1 != (r&1)); // whether to reverse complement the mate
		is_larger = !(r>>1); // whether the mate has larger coordinate
		if (is_rev) {
			rev = malloc(l_ms); // this is the reverse complement of $ms
			for (i = 0; i < l_ms; ++i) rev[l_ms - 1 - i] = ms[i] < 4? 3 - ms[i] : 4;
			seq = rev;
		} else seq = (uint8_t*)ms;
		if (!is_rev) {
			rb = is_larger? a->rb + pes[r].low : a->rb - pes[r].high;
			re = (is_larger? a->rb + pes[r].high: a->rb - pes[r].low) + l_ms; // if on the same strand, end position should be larger to make room for the seq length
		} else {
			rb = (is_larger? a->rb + pes[r].low : a->rb - pes[r].high) - l_ms; // similarly on opposite strands
			re = is_larger? a->rb + pes[r].high: a->rb - pes[r].low;
		}
		if (rb < 0) rb = 0;
		if (re > l_pac<<1) re = l_pac<<1;
		ref = bns_get_seq(l_pac, pac, rb, re, &len);
		if (len == re - rb) { // no funny things happening
			kswr_t aln;
			mem_alnreg_t b;
			int tmp, xtra = KSW_XSUBO | KSW_XSTART | (l_ms * opt->a < 250? KSW_XBYTE : 0) | opt->min_seed_len;
			aln = ksw_align(l_ms, seq, len, ref, 5, opt->mat, opt->q, opt->r, xtra, 0);
			memset(&b, 0, sizeof(mem_alnreg_t));
			b.qb = aln.qb; b.qe = aln.qe + 1;
			b.rb = is_rev? (l_pac<<1) - (rb + aln.te + 1) : rb + aln.tb;
			b.re = is_rev? (l_pac<<1) - (rb + aln.tb) : rb + aln.te + 1;
			b.score = aln.score;
			b.csub = aln.score2;
			b.secondary = -1;
//			printf("*** %d, [%lld,%lld], %d:%d, (%lld,%lld), (%lld,%lld) == (%lld,%lld)\n", aln.score, rb, re, is_rev, is_larger, a->rb, a->re, ma->a[0].rb, ma->a[0].re, b.rb, b.re);
			kv_push(mem_alnreg_t, *ma, b); // make room for a new element
			// move b s.t. ma is sorted
			for (i = 0; i < ma->n - 1; ++i) // find the insertion point
				if (ma->a[i].score < b.score) break;
			tmp = i;
			for (i = ma->n - 1; i > tmp; --i) ma->a[i] = ma->a[i-1];
			ma->a[i] = b;
			++n;
		}
		if (rev == 0) free(rev);
		free(ref);
	}
	return n;
}

int mem_pair(const mem_opt_t *opt, int64_t l_pac, const uint8_t *pac, const mem_pestat_t pes[4], bseq1_t s[2], mem_alnreg_v a[2], bwahit_t h[2])
{
	extern void mem_alnreg2hit(const mem_alnreg_t *a, bwahit_t *h);
	pair64_v v;
	pair64_t o, subo; // score<<32 | raw_score<<8 | hash
	int r, i, k, y[4]; // y[] keeps the last hit
	kv_init(v);
	for (r = 0; r < 2; ++r) { // loop through read number
		for (i = 0; i < a[r].n; ++i) {
			pair64_t key;
			mem_alnreg_t *e = &a[r].a[i];
			key.x = e->rb < l_pac? e->rb : (l_pac<<1) - 1 - e->rb; // forward position
			key.y = (uint64_t)e->score << 32 | i << 2 | (e->rb >= l_pac)<<1 | r;
			kv_push(pair64_t, v, key);
		}
	}
	ks_introsort_128(v.n, v.a);
	y[0] = y[1] = y[2] = y[3] = -1;
	o.x = o.y = subo.x = subo.y = 0;
	for (i = 0; i < v.n; ++i) {
		for (r = 0; r < 2; ++r) { // loop through direction
			int dir = r<<1 | (v.a[i].y>>1&1), which;
			if (pes[dir].failed) continue; // invalid orientation
			which = r<<1 | ((v.a[i].y&1)^1);
			if (y[which] < 0) continue; // no previous hits
			for (k = y[which]; k >= 0; --k) { // TODO: this is a O(n^2) solution in the worst case; remember to check if this loop takes a lot of time (I doubt)
				int64_t dist;
				int raw_score, score;
				double ns;
				uint64_t x, pair;
				if ((v.a[k].y&3) != which) continue;
				dist = (int64_t)v.a[i].x - v.a[k].x;
				if (dist > pes[dir].high) break;
				if (dist < pes[dir].low)  continue;
				raw_score = (v.a[i].y>>32) + (v.a[i].y>>32);
				if (raw_score + 20 * opt->a < (subo.x>>8&0xffffff)) continue; // skip the following if the score is too small
				ns = (dist - pes[dir].avg) / pes[dir].std;
				score = (int)(raw_score - 4.343 / 23. * (opt->a + opt->b) * log(erfc(fabs(ns) * M_SQRT1_2)) + .499);
				pair = (uint64_t)k<<32 | i;
				x = (uint64_t)score<<32 | (int64_t)raw_score<<8 | (hash_64(pair)&0xff);
				if (x > o.x) subo = o, o.x = x, o.y = pair;
				else if (x > subo.x) subo.x = x, subo.y = pair;
			}
		}
		y[v.a[i].y&3] = i;
	}
	if (o.x > 0) {
		i = o.y >> 32; k = o.y << 32 >> 32;
		mem_alnreg2hit(&a[v.a[i].y&1].a[v.a[i].y<<32>>34], &h[v.a[i].y&1]);
		mem_alnreg2hit(&a[v.a[k].y&1].a[v.a[k].y<<32>>34], &h[v.a[k].y&1]);
	}
	free(v.a);
	return o.x == 0? -1 : 0;
}

int mem_sam_pe(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, const mem_pestat_t pes[4], bseq1_t s[2], mem_alnreg_v a[2])
{
	int n = 0;
	kstring_t str;
	bwahit_t h[2];
	mem_alnreg_t a0[2];
	str.l = str.m = 0; str.s = 0;
	// perform SW for the best alignment
	a0[0].score = a0[1].score = -1;
	if (a[0].n) a0[0] = a[0].a[0];
	if (a[1].n) a0[1] = a[1].a[0];
	if (a0[0].score > 0) n += mem_matesw(opt, bns->l_pac, pac, pes, &a0[0], s[1].l_seq, (uint8_t*)s[1].seq, &a[1]);
	if (a0[1].score > 0) n += mem_matesw(opt, bns->l_pac, pac, pes, &a0[1], s[0].l_seq, (uint8_t*)s[0].seq, &a[0]);
	// pairing single-end hits
	if (mem_pair(opt, bns->l_pac, pac, pes, s, a, h) == 0) { // successful pairing
		bwa_hit2sam(&str, opt->mat, opt->q, opt->r, opt->w, bns, pac, &s[0], &h[0], opt->is_hard);
		s[0].sam = strdup(str.s); str.l = 0;
		bwa_hit2sam(&str, opt->mat, opt->q, opt->r, opt->w, bns, pac, &s[1], &h[1], opt->is_hard);
		s[1].sam = str.s;
	} else {
	}
	return n;
}