# Beszel-HID-UPS

Shows a USB UPS battery in [Beszel](https://github.com/henrygd/beszel) when the agent runs in a Docker container.

Works with USB UPS models that follow the HID Power Device class (APC, Eaton, CyberPower, ...).

## Install

Download all files from the latest release. The binary runs on any x86_64 Linux.

```sh
sha256sum -c SHA256SUMS
install -m 755 beszel-hid-ups /usr/local/sbin/beszel-hid-ups
```

Start it with OpenRC (Alpine Linux):

```sh
install -m 755 beszel-hid-ups.initd /etc/init.d/beszel-hid-ups
rc-update add beszel-hid-ups default
rc-service beszel-hid-ups start
```

Or with systemd (Debian, Ubuntu, Fedora, ...):

```sh
install -m 644 beszel-hid-ups.service /etc/systemd/system/beszel-hid-ups.service
systemctl enable --now beszel-hid-ups
```

Mount the output into the agent container:

```yaml
services:
  beszel-agent:
    volumes:
      - /run/beszel-hid-ups:/sys/class/power_supply:ro
```

## How it works

The Beszel agent reads batteries from `/sys/class/power_supply`, where Linux does not list USB UPS. Whenever the UPS signals a change, and at least every 30 seconds, `beszel-hid-ups` reads the UPS through `/dev/hidraw*` and writes the same files a battery driver would:

```
/run/beszel-hid-ups/ups/
├── capacity      87
├── status        Charging
├── model_name    Back-UPS BX2200MI FW:378700G -302202G
└── type          Battery
```
