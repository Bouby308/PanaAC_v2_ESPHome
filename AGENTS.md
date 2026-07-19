# PanaAC v2 ESPHome Component

## Scope

This repository implements the PanaAC v2 ESPHome external component. Primary
code is in `esphome/components/panaac_v2/`; example configurations are in
`esphome/`.

## Implementation Guidelines

- Keep the canonical AC state, IR encode/decode, native climate API, and v2
  MQTT state consistent. Avoid changing one representation without updating
  the others.
- v1 mode omits `topic_prefix`; v2 mode uses retained
  `<prefix>/availability`, `<prefix>/traits`, and `<prefix>/state`, and accepts
  partial JSON on `<prefix>/set`.
- Preserve public labels and semantics: ESPHome native presets are `None`,
  `BOOST`, and `ECO`; MQTT/HA exposes `None`, `Powerful`, and `Eco`. Powerful
  fan and preset are coupled. Powerful/Eco are allowed only in Auto, Cool, and
  Dry, and must reject in Heat.
- When changing schema or code generation, keep `device_id` grouping intact:
  the climate and Swing V/H entities share the optional ESPHome sub-device.
- Follow the existing C++ and ESPHome Python style. Keep generated build output
  and local secrets out of commits.

## Testing

Canonical tests are in `../PanaAC_v2_Testing`:

```bash
cd ../PanaAC_v2_Testing
python3 run_full_test.py run --suite esphome.g1
```

For firmware/MQTT behavior, run `esphome.g2` and `esphome.g3` only with the
configured external broker and DUT. Reflashing a DUT is an explicit operation;
use `--no-flash-dut` for diagnostics against an already-prepared device. Add or
update matching variants, runner assertions, and test documentation for every
behavior change.

## Repository Hygiene

- Never commit `esphome/secrets.yaml`, Wi-Fi credentials, MQTT credentials,
  `.esphome/` build output, or firmware artifacts.
- Keep README, DESIGN, and INSTALL documentation consistent with schema,
  MQTT-contract, or wiring changes.
- Use focused, imperative commits and preserve unrelated local changes.
