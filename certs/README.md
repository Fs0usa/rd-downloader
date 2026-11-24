# Additional TLS trust anchor

`real-debrid-geotrust-intermediate.pem` is the `GeoTrust TLS RSA CA G1` intermediate certificate observed in the live TLS chain served by `sao1-4.download.real-debrid.com` on 2026-11-23. It chains to `DigiCert Global Root G2`, which is already present in Debian's Mozilla-derived CA bundle embedded by the app.

Certificate SHA-256 fingerprint: `C0:6E:30:7F:7C:FC:1D:32:FA:72:A4:C0:33:C8:7B:90:01:9A:F2:16:F0:77:5D:64:97:8A:2E:CA:6C:8A:23:0E`.

The user confirmed that adding this trust anchor fixed the PS5 `UNKNOWN_CA` failure for the Real-Debrid CDN URL tested. Normal certificate-chain, hostname, and SNI verification remain enabled. The PEM is embedded at build time; it is not fetched or regenerated per download. Recheck and replace it if the CDN changes its issuing chain, and before the certificate expires on 2027-11-02. `make` regenerates the embedded bundle from the installed Debian CA bundle and this file.
