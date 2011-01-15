#include <zlib.h>
#include "bwtaln.h"
#include "utils.h"
#include "bamlite.h"

#include "kseq.h"
KSEQ_INIT(gzFile, gzread)

extern unsigned char nst_nt4_table[256];
static char bam_nt16_nt4_table[] = { 4, 0, 1, 4, 2, 4, 4, 4, 3, 4, 4, 4, 4, 4, 4, 4 };

struct __bwa_seqio_t {
	// for BAM input
	int is_bam, which; // 1st bit: read1, 2nd bit: read2, 3rd: SE
	bamFile fp;
	// for fastq input
	kseq_t *ks;
};

bwa_seqio_t *bwa_bam_open(const char *fn, int which)
{
	bwa_seqio_t *bs;
	bam_header_t *h;
	bs = (bwa_seqio_t*)calloc(1, sizeof(bwa_seqio_t));
	bs->is_bam = 1;
	bs->which = which;
	bs->fp = bam_open(fn, "r");
	h = bam_header_read(bs->fp);
	bam_header_destroy(h);
	return bs;
}

bwa_seqio_t *bwa_seq_open(const char *fn)
{
	gzFile fp;
	bwa_seqio_t *bs;
	bs = (bwa_seqio_t*)calloc(1, sizeof(bwa_seqio_t));
	fp = xzopen(fn, "r");
	bs->ks = kseq_init(fp);
	return bs;
}

void bwa_seq_close(bwa_seqio_t *bs)
{
	if (bs == 0) return;
	if (bs->is_bam) bam_close(bs->fp);
	else {
		gzclose(bs->ks->f->f);
		kseq_destroy(bs->ks);
	}
	free(bs);
}

void seq_reverse(int len, ubyte_t *seq, int is_comp)
{
	int i;
	if (is_comp) {
		for (i = 0; i < len>>1; ++i) {
			char tmp = seq[len-1-i];
			if (tmp < 4) tmp = 3 - tmp;
			seq[len-1-i] = (seq[i] >= 4)? seq[i] : 3 - seq[i];
			seq[i] = tmp;
		}
		if (len&1) seq[i] = (seq[i] >= 4)? seq[i] : 3 - seq[i];
	} else {
		for (i = 0; i < len>>1; ++i) {
			char tmp = seq[len-1-i];
			seq[len-1-i] = seq[i]; seq[i] = tmp;
		}
	}
}

int bwa_trim_read(int trim_qual, bwa_seq_t *p)
{
	int s = 0, l, max = 0, max_l = p->len - 1;
	if (trim_qual < 1 || p->qual == 0) return 0;
	for (l = p->len - 1; l >= BWA_MIN_RDLEN - 1; --l) {
		s += trim_qual - (p->qual[l] - 33);
		if (s < 0) break;
		if (s > max) {
			max = s; max_l = l;
		}
	}
	p->clip_len = p->len = max_l + 1;
	return p->full_len - p->len;
}

static bwa_seq_t *bwa_read_bam(bwa_seqio_t *bs, int n_needed, int *n, int is_comp, int trim_qual)
{
	bwa_seq_t *seqs, *p;
	int n_seqs, l, i;
	long n_trimmed = 0, n_tot = 0;
	bam1_t *b;

	b = bam_init1();
	n_seqs = 0;
	seqs = (bwa_seq_t*)calloc(n_needed, sizeof(bwa_seq_t));
	while (bam_read1(bs->fp, b) >= 0) {
		uint8_t *s, *q;
		int go = 0;
		if ((bs->which & 1) && (b->core.flag & BAM_FREAD1)) go = 1;
		if ((bs->which & 2) && (b->core.flag & BAM_FREAD2)) go = 1;
		if ((bs->which & 4) && !(b->core.flag& BAM_FREAD1) && !(b->core.flag& BAM_FREAD2))go = 1;
		if (go == 0) continue;
		l = b->core.l_qseq;
		p = &seqs[n_seqs++];
		p->tid = -1; // no assigned to a thread
		p->qual = 0;
		p->full_len = p->clip_len = p->len = l;
		n_tot += p->full_len;
		s = bam1_seq(b); q = bam1_qual(b);
		p->seq = (ubyte_t*)calloc(p->len + 1, 1);
		p->qual = (ubyte_t*)calloc(p->len + 1, 1);
		for (i = 0; i != p->full_len; ++i) {
			p->seq[i] = bam_nt16_nt4_table[(int)bam1_seqi(s, i)];
			p->qual[i] = q[i] + 33 < 126? q[i] + 33 : 126;
		}
		if (bam1_strand(b)) { // then reverse 
			seq_reverse(p->len, p->seq, 1);
			seq_reverse(p->len, p->qual, 0);
		}
		if (trim_qual >= 1) n_trimmed += bwa_trim_read(trim_qual, p);
		p->rseq = (ubyte_t*)calloc(p->full_len, 1);
		memcpy(p->rseq, p->seq, p->len);
		seq_reverse(p->len, p->seq, 0); // *IMPORTANT*: will be reversed back in bwa_refine_gapped()
		seq_reverse(p->len, p->rseq, is_comp);
		p->name = strdup((const char*)bam1_qname(b));
		if (n_seqs == n_needed) break;
	}
	*n = n_seqs;
	if (n_seqs && trim_qual >= 1)
		fprintf(stderr, "[bwa_read_seq] %.1f%% bases are trimmed.\n", 100.0f * n_trimmed/n_tot);
	if (n_seqs == 0) {
		free(seqs);
		bam_destroy1(b);
		return 0;
	}
	bam_destroy1(b);
	return seqs;
}

bwa_seq_t *bwa_read_seq(bwa_seqio_t *bs, int n_needed, int *n, int flag, int trim_qual)
{
	bwa_seq_t *seqs, *p;
	kseq_t *seq = bs->ks;
	int n_seqs, l, i, is_comp = flag&1, is_64 = flag&2;
	long n_trimmed = 0, n_tot = 0;

	if (bs->is_bam) return bwa_read_bam(bs, n_needed, n, is_comp, trim_qual);
	n_seqs = 0;
	seqs = (bwa_seq_t*)calloc(n_needed, sizeof(bwa_seq_t));
	while ((l = kseq_read(seq)) >= 0) {
		if (is_64 && seq->qual.l)
			for (i = 0; i < seq->qual.l; ++i) seq->qual.s[i] -= 31;
		p = &seqs[n_seqs++];
		p->tid = -1; // no assigned to a thread
		p->qual = 0;
		p->full_len = p->clip_len = p->len = l;
		n_tot += p->full_len;
		p->seq = (ubyte_t*)calloc(p->len, 1);
		for (i = 0; i != p->full_len; ++i)
			p->seq[i] = nst_nt4_table[(int)seq->seq.s[i]];
		if (seq->qual.l) { // copy quality
			p->qual = (ubyte_t*)strdup((char*)seq->qual.s);
			if (trim_qual >= 1) n_trimmed += bwa_trim_read(trim_qual, p);
		}
		p->rseq = (ubyte_t*)calloc(p->full_len, 1);
		memcpy(p->rseq, p->seq, p->len);
		seq_reverse(p->len, p->seq, 0); // *IMPORTANT*: will be reversed back in bwa_refine_gapped()
		seq_reverse(p->len, p->rseq, is_comp);
		p->name = strdup((const char*)seq->name.s);
		{ // trim /[12]$
			int t = strlen(p->name);
			if (t > 2 && p->name[t-2] == '/' && (p->name[t-1] == '1' || p->name[t-1] == '2')) p->name[t-2] = '\0';
		}
		if (n_seqs == n_needed) break;
	}
	*n = n_seqs;
	if (n_seqs && trim_qual >= 1)
		fprintf(stderr, "[bwa_read_seq] %.1f%% bases are trimmed.\n", 100.0f * n_trimmed/n_tot);
	if (n_seqs == 0) {
		free(seqs);
		return 0;
	}
	return seqs;
}

void bwa_free_read_seq(int n_seqs, bwa_seq_t *seqs)
{
	int i, j;
	for (i = 0; i != n_seqs; ++i) {
		bwa_seq_t *p = seqs + i;
		for (j = 0; j < p->n_multi; ++j)
			if (p->multi[j].cigar) free(p->multi[j].cigar);
		free(p->name);
		free(p->seq); free(p->rseq); free(p->qual); free(p->aln); free(p->md); free(p->multi);
		free(p->cigar);
	}
	free(seqs);
}