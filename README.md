# rtcmbridge

Proyecto C++17 con herramientas RTCM/NTRIP para:
- `rtcm_bridge`: puente NTRIP -> NATS.
- `rtcm_decoder_ntrip`: decoder RTCM desde caster NTRIP.
- `rtcm_decoder_nats`: decoder RTCM desde NATS.
- `rtcm_rinex_recorder`: recorder en tiempo real RTCM -> RINEX.
- `rtcm_station_position`: estimador de posicion de estacion (`rtcm` o `ppp`).

## Estructura
- `src/core`: libreria comun (NTRIP, parser RTCM, mensajes 1005/1006, mountpoints).
- `src/tools`: binarios.
- `include/rtcmbridge/core`: headers publicos.
- `scripts`: utilidades de bootstrap/PPP.
- `tests`: tests unitarios de core.

## Requisitos
- CMake >= 3.16
- Compilador C++17
- `make`
- `git`

Dependencias opcionales:
- `libnats` para `rtcm_bridge` y `rtcm_decoder_nats`
- `convbin`/`rnx2rtkp` (RTKLIB) para RINEX/PPP

## Bootstrap de dependencias locales (opcional)
Instala dependencias en `third_party/` para reproducibilidad:

```bash
scripts/bootstrap_deps.sh
```

Solo RTKLIB:

```bash
scripts/bootstrap_deps.sh --rtklib-only
```

## Compilar
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Sin herramientas NATS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_NATS_TOOLS=OFF
cmake --build build -j
```

## Configuracion de mountpoints
Formato de `mountpoints.conf`:

```text
user:pass host:port/mountpoint
```

Si no pasas `--mountpoint`, los tools que lo soportan procesan todos los mountpoints del fichero.

## Ejecucion

### 1) Bridge NTRIP -> NATS
```bash
build/bin/rtcm_bridge mountpoints.conf
```

### 2) Decoder directo NTRIP
```bash
build/bin/rtcm_decoder_ntrip <host> <port> <mountpoint> <user> <pass>
```

### 3) Decoder desde NATS
```bash
build/bin/rtcm_decoder_nats <mountpoint> [nats_servers_csv]
```

### 4) Recorder RTCM -> RINEX en tiempo real
```bash
build/bin/rtcm_rinex_recorder \
  --mountpoints-file=mountpoints.conf \
  --out-dir=./data \
  --rinex-version=3.04 \
  --rinex-update-sec=10
```

Notas:
- Si `convbin` esta disponible, genera `*.obs` y `*.nav`.
- Si `convbin` no esta disponible, cae a `*.rtcm3`.
- El proceso es continuo y termina con `Ctrl+C`.

### 5) Posicion de estacion

Modo RTCM (1005/1006):
```bash
build/bin/rtcm_station_position \
  --mountpoints-file=mountpoints.conf \
  --mode=rtcm \
  --out-dir=./data
```

Modo PPP (solver externo):
```bash
build/bin/rtcm_station_position \
  --mountpoints-file=mountpoints.conf \
  --mode=ppp \
  --out-dir=./data \
  --solver-cmd='scripts/rtklib_ppp_solver.sh {rtcm} {solution} {workdir}'
```

## Tests
```bash
ctest --test-dir build --output-on-failure
```
