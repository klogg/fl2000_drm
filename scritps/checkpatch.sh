#!/bin/bash

mod_pattern="*.mod.c"

if [ ! -f checkpatch.pl ]; then
	wget https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/scripts/checkpatch.pl
fi

if [ ! -f spelling.txt ]; then
	wget https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/scripts/spelling.txt
fi

if [ ! -f const_structs.checkpatch ]; then
	wget https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/scripts/const_structs.checkpatch
fi

for file in {,bridge/}*.[ch]
do
	if [[ $file == $mod_pattern ]]; then
		continue
	fi
	./checkpatch.pl --strict --no-summary --no-tree $@ -f "$file"
done

