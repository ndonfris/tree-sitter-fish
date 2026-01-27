#!/bin/bash

shopt -s globstar

parse_errors=''
dir="${1:-./examples/fish/share/functions}"

for file in "$dir"/**/*.fish
do
    ./node_modules/.bin/tree-sitter parse $file > /dev/null

    if test "$?" != "0"; then
        parse_errors+=$file
        parse_errors+='\n'
    fi
done

if test -n "$parse_errors"; then
    echo "Parsing failed for following files:"
    printf "$parse_errors"
    exit 1
else
    echo "All files parsed successfully"
fi
