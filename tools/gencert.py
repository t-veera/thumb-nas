"""Generate the TLS material for the thumb-nas WebDAV server.

Produces a two-certificate chain rather than a single self-signed cert:

  ca.crt / ca.key       A certificate authority carrying a critical
                        nameConstraints extension. This is the one you install
                        into the Windows trust store.
  server.crt            The server leaf, signed by that CA, with the CA
                        appended so the board serves a complete chain.
  server.key            The leaf's private key. Embedded in the firmware.

Why two certs instead of one self-signed cert:

A single self-signed certificate must be marked CA=TRUE to be trusted, and
installing it into LocalMachine\\Root makes it a full certificate authority on
that machine. Its private key lives in the board's flash, recoverable by anyone
who can read the chip. Such a key could then issue a valid certificate for any
site and intercept that machine's traffic.

nameConstraints cannot fix that on a single cert: per RFC 5280 the extension
constrains *subordinate* certificates, not the certificate carrying it, so a
self-signed leaf is not bound by its own constraints.

Splitting them does fix it. The constrained CA key never leaves your machine,
and the key that does ship on the board is a leaf that cannot sign anything.
Even if the board's flash is dumped, the extracted key only impersonates
keychain-nas.local.

openssl is not required; this uses the `cryptography` package already present
in the ESP-IDF Python environment.

Usage:
    python tools/gencert.py <output-dir> [board-ip] [ip-network]

    python tools/gencert.py main/certs 192.168.1.42 192.168.1.0/24
"""
import datetime
import ipaddress
import os
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

HOSTNAME = "keychain-nas.local"

out_dir = sys.argv[1]
board_ip = sys.argv[2] if len(sys.argv) > 2 else None
ip_net = sys.argv[3] if len(sys.argv) > 3 else None

os.makedirs(out_dir, exist_ok=True)
now = datetime.datetime.now(datetime.timezone.utc)

PEM = serialization.Encoding.PEM


def write(path, data):
    with open(os.path.join(out_dir, path), "wb") as f:
        f.write(data)


def private_pem(key):
    return key.private_bytes(
        encoding=PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    )


# --------------------------------------------------------------------------
# 1. The constrained CA
# --------------------------------------------------------------------------
ca_key = ec.generate_private_key(ec.SECP256R1())  # prime256v1
ca_name = x509.Name([
    x509.NameAttribute(NameOID.COMMON_NAME, "thumb-nas local CA"),
])

# Permitted subtrees. A CA is only allowed to issue for names inside these, and
# Windows' schannel enforces it. Anything else the key tries to sign is
# rejected by the client before it is ever trusted.
permitted = [x509.DNSName(HOSTNAME)]
if ip_net:
    net = ipaddress.ip_network(ip_net, strict=False)
    permitted.append(x509.IPAddress(net))

ca_cert = (
    x509.CertificateBuilder()
    .subject_name(ca_name)
    .issuer_name(ca_name)
    .public_key(ca_key.public_key())
    .serial_number(x509.random_serial_number())
    .not_valid_before(now - datetime.timedelta(days=1))
    .not_valid_after(now + datetime.timedelta(days=3650))
    .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
    .add_extension(
        x509.KeyUsage(
            digital_signature=False, content_commitment=False,
            key_encipherment=False, data_encipherment=False,
            key_agreement=False, key_cert_sign=True, crl_sign=True,
            encipher_only=False, decipher_only=False,
        ),
        critical=True,
    )
    .add_extension(
        x509.NameConstraints(permitted_subtrees=permitted, excluded_subtrees=None),
        critical=True,
    )
    .add_extension(
        x509.SubjectKeyIdentifier.from_public_key(ca_key.public_key()),
        critical=False,
    )
    .sign(ca_key, hashes.SHA256())
)

# --------------------------------------------------------------------------
# 2. The server leaf, signed by the CA
# --------------------------------------------------------------------------
leaf_key = ec.generate_private_key(ec.SECP256R1())

san = [x509.DNSName(HOSTNAME)]
if board_ip:
    san.append(x509.IPAddress(ipaddress.ip_address(board_ip)))

leaf_cert = (
    x509.CertificateBuilder()
    .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, HOSTNAME)]))
    .issuer_name(ca_name)
    .public_key(leaf_key.public_key())
    .serial_number(x509.random_serial_number())
    .not_valid_before(now - datetime.timedelta(days=1))
    .not_valid_after(now + datetime.timedelta(days=3650))
    # CA=FALSE: this key ships on the board and must not be able to sign.
    .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
    .add_extension(x509.SubjectAlternativeName(san), critical=False)
    .add_extension(
        x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]),
        critical=False,
    )
    .add_extension(
        x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()),
        critical=False,
    )
    .sign(ca_key, hashes.SHA256())
)

write("ca.key", private_pem(ca_key))
write("ca.crt", ca_cert.public_bytes(PEM))
write("server.key", private_pem(leaf_key))
# Leaf first, then the issuing CA: mbedtls serves this as a complete chain.
write("server.crt", leaf_cert.public_bytes(PEM) + ca_cert.public_bytes(PEM))

print("wrote to", out_dir)
print()
print("  ca.crt      install THIS one into the Windows trust store")
print("  ca.key      never leaves this machine, never goes on the board")
print("  server.crt  leaf + CA chain, embedded in firmware")
print("  server.key  leaf key, embedded in firmware (cannot sign anything)")
print()
print("CA subject :", ca_cert.subject.rfc4514_string())
print("constraints:", [str(g.value) for g in
                       ca_cert.extensions.get_extension_for_class(
                           x509.NameConstraints).value.permitted_subtrees])
print("leaf SAN   :", [str(g.value) for g in
                       leaf_cert.extensions.get_extension_for_class(
                           x509.SubjectAlternativeName).value])
print("leaf is CA :", leaf_cert.extensions.get_extension_for_class(
    x509.BasicConstraints).value.ca)
print("expires    :", leaf_cert.not_valid_after_utc)
