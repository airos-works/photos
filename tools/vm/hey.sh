#!/usr/bin/env bash
# hey.sh COMMAND [name=value ...]: scripts Photos in the test VM (the
# commands are in src/ui/App.cpp), e.g.
#   hey.sh group mode=0     hey.sh openphoto id=25     hey.sh state
HERE=$(dirname "$(readlink -f "$0")")
code=$1; shift
args=""
for a in "$@"; do
	# hey sends a bare number as text: say what it is.
	name=${a%%=*}; value=${a#*=}
	if [[ $value =~ ^-?[0-9]+$ ]]; then a="'$name=int32($value)'"
	elif [[ $value == true || $value == false ]]; then a="'$name=bool($value)'"
	else a="'$name=$value'"; fi
	args="$args and $a"
done
[[ -n $args ]] && args="with ${args# and }"
"$HERE/ssh.sh" "hey Photos $code $args"
