# ZMK module: Ploopy Adept

```text
.-----.                       .-----. .-----.
|  0  | .----. .----.  .----. |  1  | .----.
|     | |  4 | |  5 |  |  6  | |  7  | |    |
|     | '----' '----'  '----' |  8  | |    |
|     | .----. .----.  .----. |  9  | |    |
|     | |  A | |  B |  |  C  | |  D  | |    |
'-----' '----' '----'  '----' '----' '----'
   Q       R     S        T       U     V
   Tab    MB4   MB5  &lt SCROLL Tab  MB1   MB2
```

A Zephyr module that adds the Ploopy Adept (RP2040 trackball) board, an
optical scroll-wheel sensor driver, and a keymap tuned for drag-scroll.

## Boards

Only the Ploopy Adept is shipped and tested. Earlier revisions of this
module also included Classic 2, Mouse, and Thumb; those were removed so
the module focuses on the one layout its maintainer owns hardware for.

## Keymap at a glance

The Adept exposes six inputs. The kscan matrix in
`boards/ploopyco/ploopy_adept/ploopy_adept.dts` maps them to key positions
0-5. The keymap reuses the kaihchang `simply_adept` pattern:

| Pos | Tap       | Hold   |
|-----|-----------|--------|
| 0   | Tab       | —      |
| 1   | MB4       | —      |
| 2   | MB5       | —      |
| 3   | Tab       | SCROLL |
| 4   | MB1       | —      |
| 5   | MB2       | —      |

The SCROLL layer is held for drag-scroll. Other positions stay
transparent so MB4/MB5/MB1/MB2 still fire while you scroll. Releasing
the scroll key returns to the base layer.

Middle click (MB3) is not bound to a physical position; it is generated
by the `combo_mb3` chord (MB1 + MB2 pressed together).

ZMK Studio is supported via the `studio-rpc-usb-uart` snippet and
unlocked with the `combo_studio_unlock` chord (MB4 + MB5 + MB1 + MB2).

## Combos

| Combo                     | Output         | Notes |
|---------------------------|----------------|-------|
| Pos 0 + Pos 1             | `&bootloader`  | 500 ms timeout; bring the mouse back to QMK easily. |
| Pos 4 + Pos 5             | MB3            | Pinch-chord the BIG buttons to send middle click. |
| Pos 1 + Pos 2 + Pos 4 + Pos 5 | `&studio_unlock` | 150 ms timeout; unlock for ZMK Studio remapping. |

## Input processing pipeline

The trackball motion goes through four stages:

1. `pointer_accel` - velocity-based acceleration (kaihchang reference curve).
2. `zip_report_rate_limit 8` - 8 ms minimum between HID reports.
3. `zip_xy_transform INPUT_TRANSFORM_X_INVERT` - PMW3360 PCB X-axis flip.
4. `zip_xy_scaler 1 1` - 1:1 cursor amplifier (replaces a previous 3x/2x
   scaler that distorted drag-scroll because it lived before the scroll
   mapper).

Inside the SCROLL layer the chain is:

1. `zip_xy_transform INPUT_TRANSFORM_X_INVERT` - orientation fix.
2. `zip_xy_to_scroll_mapper` - Y → WHEEL, X → HWHEEL.
3. `scroll_inertia` - mjmjm0101 kinetic coast, scale 4:675.

`layer = <SCROLL>` on the scroll-inertia node resets coast state the
moment the layer deactivates, avoiding the stuck-scroll re-entry bug.

## Build

This module ships as a ZMK user config. Local verification:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
.venv/bin/west init -l config
.venv/bin/west update
.venv/bin/west packages pip --install
.venv/bin/west build -s zmk/app -b ploopy_adept \
    -- -DZMK_CONFIG="$PWD/config" -DSNIPPET=studio-rpc-usb-uart
```

The GitHub Actions workflow at `.github/workflows/build.yml` uses the
`studio-rpc-usb-uart` snippet via the `include` matrix in `build.yaml`.

## West modules pulled in

`config/west.yml` pulls:

- `zmk` upstream.
- `zmk-driver-pmw3360` and `zmk-behavior-sensor-attr-cycle`
  (george-norton `v0.3`).
- `zmk-driver-rp2040-sleep` for the deep-sleep hook in `ploopy_adept.dts`.
- `zmk-input-processor-report-rate-limit` and `zmk-input-processor-xyz`
  from `badjeff`.
- `zmk-pointing-acceleration-alpha` from `nuovotaka` (replaces the
  oleksandrmaslov module because both share the `pointer_accel` node).
- `zmk-input-processor-scroll-inertia` from `mjmjm0101`.
- `zmk-feature-custom-settings` and `zmk-module-runtime-input-processor`
  from `cormoran` (persistence backend; the runtime module additionally
  requires cormoran's patched ZMK fork for the web-UI RPC).

## Sensor driver

`drivers/sensor/ploopy_optical_encoder/` polls two ADC channels attached
to the Ploopy trackball's optical scroll wheel. It emits
`SENSOR_CHAN_ROTATION` events through the standard sensor trigger API
and integrates with `&mouse_listener` for drag-scroll.

## Repository layout

```text
boards/ploopyco/ploopy_adept/        Adept shield definition and keymap
drivers/sensor/ploopy_optical_encoder/  Scroll-wheel sensor driver
dts/bindings/sensor/ploopy,optical-encoder.yaml  Devicetree binding
config/west.yml                      West manifest for downstream users
build.yaml                           GitHub Actions matrix
.github/workflows/build.yml          Reusable ZMK build workflow call
requirements-dev.txt                 Python tooling pin (keymap-drawer etc.)
```

## License

MIT. Original drivers and bindings retain their individual copyright
headers.