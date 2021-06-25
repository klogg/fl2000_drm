#!/bin/bash

mod_pattern="*.mod.c"

if [ ! -f .clang-format ]; then
	wget https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/.clang-format
fi

# TODO: Add following:
#   AlignConsecutiveMacros: true
#   AlignConsecutiveBitFields: true

for file in {,bridge/}*.[ch]
do
	if [[ $file == $mod_pattern ]]; then
		continue
	fi
	clang-format -i "$file"
done
