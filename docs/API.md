# HTTP API

All endpoints are on the controller's own address. Requests that change anything are
`POST` with a JSON body and must send the header `X-Local-Control: 1`.

## `GET /api/status`

Everything the page needs, polled regularly.

```json
{
  "ready": true, "pairing": false, "pairing_mode": false, "rebooting": false,
  "confirmed": 157978, "failed": 9, "message": "Light updated",
  "free_heap": 98196, "minimum_heap": 29036, "largest_block": 29696, "in_flight": 0,
  "bulbs":   [{ "id": 1, "endpoint": 1, "name": "Lamp", "flicker": true,
                "flicker_speed": 7, "flicker_range": 5,
                "flicker_brightness": 27, "flicker_kelvin": 2200 }],
  "presets": [{ "id": 1, "name": "Night time", "lights": [ ... ] }]
}
```

`confirmed` and `failed` count Matter commands since boot.

## `POST /api/control`

One command to one bulb.

| `action` | `value` | Matter cluster |
|---|---|---|
| `power` | `0` or `1` | On/Off |
| `brightness` | `1` to `100` | Level Control |
| `temperature` | `2000` to `6500` (kelvin) | Color Control |
| `hue` | `0` to `360`, at full saturation | Color Control |
| `color` | none; send `hue` (`0` to `360`) and `saturation` (`0` to `100`) | Color Control |

```json
{ "id": 1, "action": "brightness", "value": 60 }
{ "id": 1, "action": "color", "hue": 28, "saturation": 80 }
```

## `POST /api/settings`

Rename a bulb, or set its flicker.

```json
{ "id": 1, "name": "Desk lamp" }
{ "id": 1, "flicker": true, "brightness": 27, "kelvin": 2200,
  "flicker_speed": 7, "flicker_range": 5 }
```

`flicker_speed` is 1 to 120 and `flicker_range` 1 to 20.

## `POST /api/presets`

```json
{ "action": "save", "name": "Night time", "lights": [ ... ] }
{ "action": "delete", "id": 1 }
```

Each entry in `lights` holds one bulb's brightness, colour or kelvin, and flicker settings.

## `POST /api/mode`

```json
{ "pairing": true }
```

Switches pairing mode on or off. The controller restarts to do it, because Bluetooth is
only brought up in pairing mode.

## `POST /api/pair`

Only in pairing mode.

```json
{ "name": "Reading corner", "code": "12345678901" }
```

`code` is the bulb's Matter setup code with spaces and dashes removed.
