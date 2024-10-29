All credit to the original bwa this is forked to.

This is a hacky version who's sole purpose is to dump reads and queries to a
file in the following format
```
score
reference (located around the seed hit)
query (the subsequence of the read around the seed hit)
```
IT IS NOT MULTITHREADED, though there does not seem to be anything that prevents
that if you modify the printf so it makes one printf per iteration instead of
many. It's possible a different function is called though. Haven't looked
## Getting started

	git clone https://github.com/lh3/bwa.git
	cd bwa; make
	./bwa index ref.fa
	./bwa mem ref.fa read-se.fq.gz | gzip -3 > aln-se.sam.gz
	./bwa mem ref.fa read1.fq read2.fq | gzip -3 > aln-pe.sam.gz
