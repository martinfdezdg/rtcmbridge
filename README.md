# rtcmbridge

Proyecto C++17 reducido con 3 utilidades RTCM/NTRIP:
- `rtcm_bridge`: puente NTRIP -> NATS.
- `rtcm_decoder_ntrip`: decoder RTCM desde caster NTRIP.
- `rtcm_decoder_nats`: decoder RTCM desde NATS.

## Estructura
- `src/tools/rtcm_bridge.cpp`
- `src/tools/rtcm_decoder_ntrip.cpp`
- `src/tools/rtcm_decoder_nats.cpp`
- `scripts/bootstrap_deps.sh` (opcional, instala `nats.c` local en `third_party/`)
- `docker-compose.yml` (cluster NATS local de 2 nodos)

## Requisitos
- CMake >= 3.16
- Compilador C++17
- `make`
- `git`
- `libnats` (sistema o `third_party/nats/install`)

## Dependencia local opcional (nats.c)
```bash
scripts/bootstrap_deps.sh
```

## Compilar
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Configuración de mountpoints
Formato de `mountpoints.conf`:

```text
user:pass host:port/mountpoint
```

## Ejecución

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

## NATS local (opcional)
```bash
docker compose up -d
```

- `nats://127.0.0.1:4222`
- `nats://127.0.0.1:4223`
