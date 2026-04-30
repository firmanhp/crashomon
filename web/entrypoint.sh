#!/bin/sh
set -e
chown -R crashomon:crashomon /data
exec gosu crashomon "$@"
