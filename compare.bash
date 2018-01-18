#!/bin/bash

sublime=/opt/sublime/sublime_text

for e in $sublime vim nano ./kilo
do
if type -t $e > /dev/null
then wc -c $(which $e)
else echo $e not found
fi
done

