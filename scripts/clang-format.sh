#!/bin/bash

if [ ! -f .clang-format ]; then
	wget https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/.clang-format
	sed -i '/AlignConsecutiveAssignments/i AlignConsecutiveMacros: true\nAlignConsecutiveBitFields: true' .clang-format
	sed -i 's/\(ColumnLimit: \)[0-9]*/\1100/g' .clang-format
fi

declare -i status=0
for file in {,bridge/}*.[ch]
do
	if [[ $file == *.mod.c ]]; then
		continue
	fi
	clang-format "$@" -i "$file"
	status=$status+$?
done

exit $status
