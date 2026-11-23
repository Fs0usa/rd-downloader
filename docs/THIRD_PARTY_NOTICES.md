# Third-party notices

## Mozilla CA certificate data

`src/core/ca_bundle_pem.inc` embeds the CA certificates from the Debian 13 `ca-certificates` package bundle at build time. The bundle is generated from `/etc/ssl/certs/ca-certificates.crt` with `scripts/embed-ca-bundle.py`; it also includes the separately documented Real-Debrid CDN intermediate CA under `certs/`. The Mozilla root certificate data is distributed under the Mozilla Public License 2.0. The Debian package version used for this build is `20250419+deb13u1`.

The CA set is used only for TLS peer verification. It does not disable certificate or hostname checks. To refresh it, install/update Debian's `ca-certificates`, run:

```sh
python3 scripts/embed-ca-bundle.py /etc/ssl/certs/ca-certificates.crt src/core/ca_bundle_pem.inc --extra certs/real-debrid-geotrust-intermediate.pem
```

Keep this notice and the Mozilla Public License 2.0 when redistributing the embedded bundle.
