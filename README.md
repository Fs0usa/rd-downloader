# RD Downloader for PS5

## What it is

RD Downloader is a native PS5 payload with a mobile-friendly web dashboard. It downloads direct HTTP/HTTPS links to the PS5 and can extract archives through the WFM 7-Zip Helper.

## Why

Downloads and queue state run on the PS5, so they continue if you close the browser. A phone or computer on the same network is only needed to control the dashboard.

## Install

1. Start your PS5 jailbreak and ELF loader.
2. In Payload Manager, add this custom source:

   `https://raw.githubusercontent.com/Fs0usa/rd-downloader/main/payloads.json`

3. Refresh the source and install RD Downloader.
4. Open `http://PS5_IP:8090` in a browser on your local network.

Archive extraction also requires the WFM 7-Zip Helper ELF at `/data/wfm/wfm-7zip-helper.elf`.

## Build

On Debian 13, install the PS5 SDK and build tools:

```sh
./scripts/install-ps5-sdk.sh
```

Build the payload:

```sh
make
```

To send it to a PS5 with `elfldr` listening on port 9021:

```sh
make deploy PS5_HOST=192.168.1.123
```

Replace the example address with your PS5's IP.
