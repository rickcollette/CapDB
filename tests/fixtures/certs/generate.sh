#!/bin/sh
# Generate self-signed test certificates for capdb loopback tests.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout server.key -out server.pem -days 3650 \
  -subj "/CN=capdb-test" \
  -addext "subjectAltName=IP:127.0.0.1,DNS:localhost,DNS:capdb-test"
cp server.pem ca.pem
echo "Generated $DIR/server.pem, server.key, ca.pem"
