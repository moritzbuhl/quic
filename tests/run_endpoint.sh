#!/bin/bash
set -e

# Set up the routing needed for the simulation.
/setup.sh

if [ "$ROLE" == "client" ]; then
    # Wait for the simulator to start up.
    /wait-for-it.sh sim:57832 -s -t 10
    echo "Starting QUIC client..."
    echo "Client params: $CLIENT_PARAMS"
    echo "Test case: $TESTCASE"
    ./interop_test -c $CLIENT_PARAMS $REQUESTS
else
    echo "Running QUIC server."
    ./interop_test -s -C /certs/cert.pem -P /certs/priv.key "$@"
fi
