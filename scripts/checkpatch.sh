#!/bin/bash

if [ ! -f checkpatch.pl ]; then
	wget https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/scripts/checkpatch.pl
fi

if [ ! -f spelling.txt ]; then
	wget https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/scripts/spelling.txt
fi

if [ ! -f const_structs.checkpatch ]; then
	wget https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/scripts/const_structs.checkpatch
fi

declare -i status=0
for file in {,bridge/}*.[ch]
do
	if [[ $file == *.mod.c ]]; then
		continue
	fi
	./checkpatch.pl --strict --no-summary --no-tree "$@" -f "$file"
	status=$status+$?
done

exit $status
