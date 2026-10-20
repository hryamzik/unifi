#!/bin/sh
# Call the stock UI's ubus object on the private bus:  ui-call.sh state '{"value":"ready"}'
# Only the methods the feeder needs are allowed; restart/reset/shutdown are never exposed.
method=$1; shift
case "$method" in
state|info|userActivity|locate) ;;
*) echo "refusing ui method '$method'" >&2; exit 2 ;;
esac
exec ubus -t 3 -s /var/run/ubus.sock call ui "$method" "$@"
