"""Loopback HTTPS stub for the game's API host (68bd384lrg.execute-api...), port 443.
Self-signed certificate created locally (never installed in the system store).
Logs each request line and answers 200 with an empty JSON body. Research tool only."""
import asyncio, datetime, ssl, sys, time, os

HERE = os.path.dirname(os.path.abspath(__file__))
CERT = os.path.join(HERE, "api_stub_cert.pem")
KEY = os.path.join(HERE, "api_stub_key.pem")
LOG = sys.argv[1] if len(sys.argv) > 1 else "https_stub.log"
HOST = "68bd384lrg.execute-api.ap-northeast-1.amazonaws.com"


def log(msg):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(f"{time.strftime('%H:%M:%S')} {msg}\n")


def ensure_cert():
    if os.path.exists(CERT) and os.path.exists(KEY):
        return
    # only needed to create the certificate once; the shipped cert avoids the dependency
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, HOST)])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=3650))
            .add_extension(x509.SubjectAlternativeName([x509.DNSName(HOST), x509.DNSName("*.execute-api.ap-northeast-1.amazonaws.com")]), critical=False)
            .sign(key, hashes.SHA256()))
    with open(KEY, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
    with open(CERT, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))


async def handle(reader, writer):
    peer = writer.get_extra_info("peername")
    try:
        data = await asyncio.wait_for(reader.read(65536), timeout=8)
        first = data.split(b"\r\n", 1)[0] if data else b""
        log(f"{peer} {first!r} ({len(data)} bytes)")
        body = b"{}"
        resp = (b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                b"Content-Length: " + str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)
        writer.write(resp)
        await writer.drain()
    except Exception as e:  # noqa: BLE001
        log(f"{peer} error {e!r}")
    finally:
        writer.close()


async def main():
    ensure_cert()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)
    srv = await asyncio.start_server(handle, "127.0.0.1", 443, ssl=ctx)
    log("https stub listening on 127.0.0.1:443")
    async with srv:
        await srv.serve_forever()


asyncio.run(main())
