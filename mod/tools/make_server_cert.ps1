# Makes the local server's TLS certificate for a manual install (the launcher does the same by itself on Play):
# mod\server\CSNZ_Server\bin\Data\Certs\server-cert.pem and server-key.pem, self-signed for "localhost", made on this PC
# and never shipped. Nothing happens if they're already there.
# Usage (no administrator rights needed):  powershell -ExecutionPolicy Bypass -File mod\tools\make_server_cert.ps1
$ErrorActionPreference = "Stop"
$dir = Join-Path $PSScriptRoot "..\server\CSNZ_Server\bin\Data\Certs"
$dir = [System.IO.Path]::GetFullPath($dir)
$cert = Join-Path $dir "server-cert.pem"
$key = Join-Path $dir "server-key.pem"
if ((Test-Path $cert) -and (Test-Path $key)) {
    Write-Host "The server's certificate is already there: $dir"
    exit 0
}
New-Item -ItemType Directory -Force -Path $dir | Out-Null

# the launcher's code (launcher\Core.cs, Certificates): .NET Framework 4.7.2+'s CertificateRequest, the key as PKCS#1
Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

public static class CsnzCert
{
    public static void Make(string certPath, string keyPath)
    {
        using (var rsa = new RSACryptoServiceProvider(2048))
        {
            var req = new CertificateRequest("CN=localhost", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            using (var c = req.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(20)))
                System.IO.File.WriteAllText(certPath, Pem("CERTIFICATE", c.Export(X509ContentType.Cert)));
            System.IO.File.WriteAllText(keyPath, Pem("RSA PRIVATE KEY", Pkcs1(rsa.ExportParameters(true))));
        }
    }

    static string Pem(string label, byte[] der)
    {
        var b64 = Convert.ToBase64String(der);
        var sb = new StringBuilder("-----BEGIN " + label + "-----\n");
        for (int i = 0; i < b64.Length; i += 64)
            sb.Append(b64.Substring(i, Math.Min(64, b64.Length - i))).Append('\n');
        return sb.Append("-----END " + label + "-----\n").ToString();
    }

    static byte[] Pkcs1(RSAParameters k)
    {
        var body = new List<byte>();
        foreach (var part in new[] { new byte[] { 0 }, k.Modulus, k.Exponent, k.D, k.P, k.Q, k.DP, k.DQ, k.InverseQ })
            body.AddRange(DerInteger(part));
        return DerTag(0x30, body.ToArray());
    }

    static byte[] DerInteger(byte[] v)
    {
        int i = 0;
        while (i < v.Length - 1 && v[i] == 0)
            i++;
        var trimmed = v.Skip(i).ToArray();
        if ((trimmed[0] & 0x80) != 0)
            trimmed = new byte[] { 0 }.Concat(trimmed).ToArray();
        return DerTag(0x02, trimmed);
    }

    static byte[] DerTag(byte tag, byte[] content)
    {
        var len = new List<byte>();
        if (content.Length < 0x80)
            len.Add((byte)content.Length);
        else
        {
            var bytes = BitConverter.GetBytes(content.Length).Reverse().SkipWhile(b => b == 0).ToArray();
            len.Add((byte)(0x80 | bytes.Length));
            len.AddRange(bytes);
        }
        return new[] { tag }.Concat(len).Concat(content).ToArray();
    }
}
"@ -ReferencedAssemblies System.Core, System.Security

[CsnzCert]::Make($cert, $key)
Write-Host "Made the local server's certificate (self-signed, this PC only): $dir"
