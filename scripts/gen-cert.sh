#!/usr/bin/env sh
#
# Generates a self-signed TLS certificate + RSA private key for NestDrive.
#
# Scope: LAN access and local development ONLY. Browsers will show a warning
# for a self-signed certificate, which is expected and unavoidable. For remote
# access from outside the LAN, use a real certificate (Let's Encrypt via
# DNS-01, or `tailscale cert`) rather than this one — NestDrive now obtains and
# renews one itself when Tailscale is present. See docs/REMOTE-ACCESS.md.
#
# This writes RSA because that is the lower-risk default for the LAN case and
# because every existing installation already has one. The server accepts EC
# keys too, and full certificate chains, so nothing here constrains what a real
# certificate authority may issue — see core::loadTls().
#
# Usage:
#   ./scripts/gen-cert.sh                          # localhost + detected LAN IP
#   ./scripts/gen-cert.sh nestdrive.ddns.net       # plus extra DNS names / IPs
#   ./scripts/gen-cert.sh nas.example.com 10.0.0.5
#
# Output: cert.pem and key.pem in the repository root. Both are gitignored.
# CMake copies them next to the executable at build time, which is where
# AppConfig looks for them by default.

set -eu

# Git Bash / MSYS rewrites arguments that look like Unix paths into Windows
# paths, which mangles openssl's "/CN=.../O=..." subject into "C:/Program
# Files/Git/CN=...". Disabling the conversion for this script fixes it, and the
# variable is simply ignored on Linux and macOS.
REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CERT_OUT="$REPO_ROOT/cert.pem"
KEY_OUT="$REPO_ROOT/key.pem"
DAYS=825          # ~27 months; rotate by re-running this script
KEY_BITS=2048     # FileServer::loadTlsFiles() expects QSsl::Rsa

command -v openssl >/dev/null 2>&1 || {
    echo "error: openssl not found on PATH." >&2
    echo "  Windows: it ships with Git for Windows, or install from https://slproweb.com/products/Win32OpenSSL.html" >&2
    echo "  Linux:   apt install openssl    macOS: brew install openssl" >&2
    exit 1
}

# ── Detect this machine's LAN IPv4 address ───────────────────────────────────
# `ipconfig` means two completely different tools on macOS and Windows, so
# branch on the platform rather than probing. Empty result is not fatal.
detect_lan_ip() {
    addr=""
    case "$(uname -s)" in
        Linux*)
            addr=$(ip -4 addr show scope global 2>/dev/null \
                   | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1)
            ;;
        Darwin*)
            for iface in en0 en1 en2; do
                addr=$(ipconfig getifaddr "$iface" 2>/dev/null) || addr=""
                [ -n "$addr" ] && break
            done
            ;;
        MINGW* | MSYS* | CYGWIN*)
            addr=$(ipconfig 2>/dev/null \
                   | sed -n 's/.*IPv4 Address[^:]*: *\([0-9.]*\).*/\1/p' \
                   | grep -v '^127\.' | head -1)
            ;;
    esac
    printf '%s' "$addr"
}

# ── Build the Subject Alternative Name list ──────────────────────────────────
# Modern browsers ignore the Common Name entirely and validate against SAN only,
# so every name and address used to reach the server must appear here.
SAN="DNS.1:localhost,IP.1:127.0.0.1"
DNS_N=2
IP_N=2

LAN_IP=$(detect_lan_ip)
if [ -n "$LAN_IP" ]; then
    SAN="$SAN,IP.$IP_N:$LAN_IP"
    IP_N=$((IP_N + 1))
    echo "detected LAN address: $LAN_IP"
else
    echo "warning: could not detect a LAN address; pass it as an argument if HTTPS fails from other devices" >&2
fi

for name in "$@"; do
    # Treat a bare dotted-quad as an IP, anything else as a DNS name
    if echo "$name" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$'; then
        SAN="$SAN,IP.$IP_N:$name"
        IP_N=$((IP_N + 1))
    else
        SAN="$SAN,DNS.$DNS_N:$name"
        DNS_N=$((DNS_N + 1))
    fi
done

echo "subjectAltName = $SAN"

# ── Generate ─────────────────────────────────────────────────────────────────
# A temporary config file is used instead of -subj and -addext on purpose.
# Git Bash rewrites any argument that starts with "/" into a Windows path, so
# -subj "/CN=..." arrives at openssl as "C:/Program Files/Git/CN=...". Disabling
# that conversion then breaks the /c/... output paths instead. Putting the
# subject and extensions in a config file sidesteps the problem on every
# platform.
CONF=$(mktemp)
trap 'rm -f "$CONF"' EXIT INT TERM

cat > "$CONF" <<EOF
[req]
prompt             = no
distinguished_name = dn
x509_extensions    = v3_req

[dn]
CN = nestdrive.local
O  = NestDrive
C  = IN

[v3_req]
subjectAltName   = $SAN
basicConstraints = critical,CA:FALSE
keyUsage         = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
EOF

# -noenc: no passphrase. Qt cannot prompt for one at startup, and an encrypted
# key would stop the server from starting unattended.
openssl req -x509 -newkey "rsa:$KEY_BITS" -noenc \
    -config "$CONF" \
    -extensions v3_req \
    -keyout "$KEY_OUT" \
    -out "$CERT_OUT" \
    -days "$DAYS"

# Restrict the private key where the filesystem supports it. NTFS ignores this;
# on Linux and macOS it stops other local accounts reading the key.
chmod 600 "$KEY_OUT" 2>/dev/null || true

echo
echo "wrote $CERT_OUT"
echo "wrote $KEY_OUT  (gitignored - never commit this)"
echo
openssl x509 -in "$CERT_OUT" -noout -subject -dates -ext subjectAltName
