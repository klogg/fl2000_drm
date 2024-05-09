#!/bin/bash

mod_pattern="*.mod.c"

if [ ! -f .clang-format ]; then
	wget https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/.clang-format
	sed -i '/AlignConsecutiveAssignments/i AlignConsecutiveMacros: true\nAlignConsecutiveBitFields: true' .clang-format
	sed -i 's/\(ColumnLimit: \)[0-9]*/\1100/g' .clang-format
fi

for file in {,bridge/}*.[ch]
do
	if [[ $file == $mod_pattern ]]; then
		continue
	fi
	clang-format $@ -i "$file"
done
