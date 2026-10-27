#!/bin/sh
# Example: compute md5sums of different data in parallel

# @label hash_alpha
echo "alpha bravo charlie" | md5sum > out/hash_alpha.txt

# @label hash_delta
echo "delta echo foxtrot" | md5sum > out/hash_delta.txt

# @label hash_golf
echo "golf hotel india" | md5sum > out/hash_golf.txt

# @label hash_juliet
echo "juliet kilo lima" | md5sum > out/hash_juliet.txt

# @label count_words
wc -w payload.txt > out/wordcount.txt

# @label list_files
ls -la > out/filelist.txt
