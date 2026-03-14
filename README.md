# rtcmbridge

Proyecto C++17 reducido con 3 utilidades RTCM/NTRIP:
- `ntrip2nats`: puente NTRIP -> NATS.
- `rtcm_decoder_ntrip`: decoder RTCM desde caster NTRIP.
- `rtcm_decoder_nats`: decoder RTCM desde NATS.

## Estructura
- `src/tools/ntrip2nats.cpp`
- `src/tools/rtcm_decoder_ntrip.cpp`
- `src/tools/rtcm_decoder_nats.cpp`
- `scripts/bootstrap_deps.sh` (opcional, instala `nats.c` local en `third_party/`)
- `docker-compose.yml` (cluster NATS local de 2 nodos)
- `ntrip2nats.conf` (configuración única para `ntrip2nats`)

## Requisitos
- CMake >= 3.16
- Compilador C++17
- `make`
- `git`
- `libnats` (sistema o `third_party/nats/install`)
- OpenSSL (`libcrypto`)

## Dependencia local opcional (nats.c)
```bash
scripts/bootstrap_deps.sh
```

## Compilar
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Configuración de ntrip2nats
`ntrip2nats` usa un único fichero (`ntrip2nats.conf`) con:
- `[options]` para opciones globales.
- `[nats_destinations]` para destinos NATS (una URL por línea, sin CSV).
- `[ntrip_sources]` para fuentes NTRIP.

```text
[options]
connection_header=keep-alive
read_buffer_bytes=4096
throughput_log_kb=100
reconnect_min_sec=1
reconnect_max_sec=30

[nats_destinations]
nats://127.0.0.1:4222
nats://127.0.0.1:4223

[ntrip_sources]
# Formato: user:pass host:port/mountpoint
user:pass host:port/mountpoint
```

Valores fijos en código:
- `subject_prefix = NTRIP.`
- `user_agent = NTRIP-CppBridge`

## Ejecución

### 1) Bridge NTRIP -> NATS
```bash
build/bin/ntrip2nats ntrip2nats.conf
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
