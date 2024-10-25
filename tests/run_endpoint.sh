#!/bin/bash
set -e

# Set up the routing needed for the simulation.
/setup.sh

case $TESTCASE in
versionnegotiation|handshake|transfer|retry|resumption|http3|multiconnect|zerortt|chacha20|keyupdate|ecn|v2)
	:
	;;
*)
	exit 127
	;;
esac

if [ "$ROLE" == "client" ]; then
	# Wait for the simulator to start up.
	/wait-for-it.sh sim:57832 -s -t 10
	echo "Starting QUIC client..."
	echo "Test case: $TESTCASE"
	echo "Requests: $REQUESTS"
	echo "Keylogfile: $SSLKEYLOGFILE"

	if [ "$TESTCASE" == "resumption" ] || [ "$TESTCASE" == "zerortt" ]; then
		REQS=($REQUESTS)

		echo "./interop_test -c -D /downloads -S ./session.bin -T ./tp.bin ${REQS[0]}"
		./interop_test -c -D /downloads -S ./session.bin -T ./tp.bin ${REQS[0]}

		echo "./interop_test -c -D /downloads -S ./session.bin -T ./tp.bin ${REQS[@]:1}"
		./interop_test -c -D /downloads -S ./session.bin -T ./tp.bin ${REQS[@]:1}
	elif [ "$TESTCASE" == "handshakeloss" ]; then
		for REQ in $REQUESTS; do
			echo "./interop_test -c -D /downloads \"$REQ\""
			./interop_test -c -D /downloads "$REQ"
		done
	else
		echo "./interop_test -c -D /downloads \"$REQUESTS\""
		./interop_test -c -D /downloads "$REQUESTS"
	fi
else
	echo "Running QUIC server."
	echo "Test case: $TESTCASE"
	echo "Keylogfile: $SSLKEYLOGFILE"

	echo "./interop_test -s -D /www -C /certs/cert.pem -P /certs/priv.key :::443"
	./interop_test -s -D /www -C /certs/cert.pem -P /certs/priv.key :::443
fi
