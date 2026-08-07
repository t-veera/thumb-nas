"""Generate a self-signed EC P-256 certificate for the thumb-nas WebDAV server.

Equivalent to:
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -nodes -keyout server.key -out server.crt -days 3650 \
    -subj "/CN=keychain-nas.local"

openssl is not installed on this machine, so this uses the `cryptography`
package already present in the ESP-IDF Python environment. Output is
byte-for-byte the same kind of PEM material.

One addition over the plain openssl command: a subjectAltName extension.
Modern TLS clients (including Windows' WebClient and every browser) ignore
CN entirely and validate against SAN, so a CN-only cert would be rejected
even after being explicitly trusted.
"""
import datetime
import ipaddress
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

out_dir = sys.argv[1]
board_ip = sys.argv[2] if len(sys.argv) > 2 else None

key = ec.generate_private_key(ec.SECP256R1())  # prime256v1

subject = issuer = x509.Name([
    x509.NameAttribute(NameOID.COMMON_NAME, "keychain-nas.local"),
])

san = [x509.DNSName("keychain-nas.local")]
if board_ip:
    san.append(x509.IPAddress(ipaddress.ip_address(board_ip)))

now = datetime.datetime.now(datetime.timezone.utc)

cert = (
    x509.CertificateBuilder()
    .subject_name(subject)
    .issuer_name(issuer)
    .public_key(key.public_key())
    .serial_number(x509.random_serial_number())
    .not_valid_before(now - datetime.timedelta(days=1))
    .not_valid_after(now + datetime.timedelta(days=3650))
    .add_extension(x509.SubjectAlternativeName(san), critical=False)
    .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
    .sign(key, hashes.SHA256())
)

with open(out_dir + "/server.key", "wb") as f:
    f.write(key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),  # -nodes
    ))

with open(out_dir + "/server.crt", "wb") as f:
    f.write(cert.public_bytes(serialization.Encoding.PEM))

print("wrote server.key and server.crt to", out_dir)
print("subject:", cert.subject.rfc4514_string())
print("SAN:", [str(g.value) for g in cert.extensions.get_extension_for_class(
    x509.SubjectAlternativeName).value])
print("not_valid_after:", cert.not_valid_after_utc)
print("key: EC prime256v1 (SECP256R1)")
