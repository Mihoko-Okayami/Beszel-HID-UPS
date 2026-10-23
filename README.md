# Beszel-HID-UPS

Shows a USB UPS battery in [Beszel](https://github.com/henrygd/beszel) when the agent runs in a Docker container.

Works with USB UPS models that follow the HID Power Device class (APC, Eaton, CyberPower, ...).

## Install

Download all files from the latest release. `beszel-hid-ups` runs on any x86_64 Linux. On ARM, such as a Raspberry Pi, use `beszel-hid-ups-aarch64` instead if `getconf LONG_BIT` prints 64, otherwise `beszel-hid-ups-armv7`, or `beszel-hid-ups-armv6` on a Raspberry Pi Zero or 1.

```sh
sha256sum -c SHA256SUMS
install -D -m 755 beszel-hid-ups /usr/local/sbin/beszel-hid-ups
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

On hosts where SELinux labels containers (Docker with SELinux enabled, or Podman run as root), use `:ro,z` instead of `:ro`. Rootless Podman cannot relabel this directory, which belongs to root.

## CyberPower UT and similar models

Some UPS models, such as the CyberPower UT series, keep reporting mains power when it is lost and only flag the battery as discharging, so the status stays `Full` until the charge drops below 100 %. Like NUT's option of the same name, `-x onlinedischarge_onbattery` makes this state count as on battery:

- OpenRC: add `command_args="-x onlinedischarge_onbattery"` to `/etc/conf.d/beszel-hid-ups`, then run `rc-service beszel-hid-ups restart`
- systemd: run `systemctl edit beszel-hid-ups` and add the lines below, then run `systemctl restart beszel-hid-ups`

```ini
[Service]
ExecStart=
ExecStart=/usr/local/sbin/beszel-hid-ups -x onlinedischarge_onbattery
```

Do not set it on models that report the same state on mains when fully charged, such as some APC Back-UPS. Like NUT, the log suggests the option the first time the UPS reports mains power while discharging below 100 %.

## Logs

Only changes are logged: UPS found, unplugged or without usable data, read and write errors, on battery or on mains, and stops.

- OpenRC: `/var/log/beszel-hid-ups.log`
- systemd: `journalctl -u beszel-hid-ups`

## How it works

The Beszel agent reads batteries from `/sys/class/power_supply`, where Linux does not list USB UPS. Whenever the UPS signals a change, and at least every 30 seconds, `beszel-hid-ups` reads the UPS through `/dev/hidraw*` and writes the same files a battery driver would:

```
/run/beszel-hid-ups/ups/
├── capacity      87
├── status        Charging
├── model_name    Back-UPS BX2200MI FW:378700G -302202G
└── type          Battery
```

## License

Beszel-HID-UPS is released under the [MIT license](LICENSE).

## Third-party software

The binaries contain [musl](https://musl.libc.org) (MIT). Its license is in [`licenses/`](licenses), which the source code archive of each release includes.
