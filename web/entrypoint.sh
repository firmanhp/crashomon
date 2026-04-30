#!/bin/sh
set -e
chown -R crashomon:crashomon /data
# Ensure all gunicorn workers share one SECRET_KEY. Without this, each worker
# generates its own random key and flash messages (stored in the session cookie)
# are silently lost when the POST and redirect-GET are handled by different workers.
if [ -z "$SECRET_KEY" ]; then
    export SECRET_KEY=$(python3 -c "import secrets; print(secrets.token_hex(32))")
fi
exec gosu crashomon "$@"
