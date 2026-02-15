# rtcmbridge

Proyecto C++ para ingesta, transporte, decodificación y postproceso RTCM.

## Requisitos
- C++17
- CMake >= 3.16
- pthreads
- `libnats` (para `rtcm_bridge` y `rtcm_decoder_nats`)
- Opcional: RTKLIB (`convbin`, `rnx2rtkp`) para RINEX y PPP

## Compilar
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Tests
```bash
ctest --test-dir build --output-on-failure
```

## 1) Bridge independiente (`rtcm_bridge`)
Lee `mountpoints.conf` y publica en NATS como `NTRIP.<mountpoint>`.
```bash
build/bin/rtcm_bridge mountpoints.conf
```

Monitoreo en vivo:
```bash
build/bin/rtcm_decoder_nats ABAN3M
```

## 2) Decoder directo NTRIP (`rtcm_decoder_ntrip`)
```bash
build/bin/rtcm_decoder_ntrip <host> <port> <mountpoint> <user> <pass>
```

Ejemplo:
```bash
build/bin/rtcm_decoder_ntrip 192.168.1.10 2101 BASE01 user pass
```

## 3) Decoder desde NATS (`rtcm_decoder_nats`)
```bash
build/bin/rtcm_decoder_nats <mountpoint> [nats_servers_csv]
```

Ejemplo:
```bash
build/bin/rtcm_decoder_nats BASE01 nats://127.0.0.1:4222
```

## 4) Recorder RTCM->RINEX (`rtcm_rinex_recorder`)
Lee conexión desde `mountpoints.conf`, graba RTCM válido y opcionalmente genera RINEX al finalizar.
Si no pasas `--mountpoint`, procesa todos los mountpoints del conf en paralelo.
```bash
build/bin/rtcm_rinex_recorder \
  --mountpoints-file=mountpoints.conf \
  --out-dir=./data --station=ABAN3M \
  --convbin=/usr/local/bin/convbin
```

Para un único mountpoint:
```bash
build/bin/rtcm_rinex_recorder \
  --mountpoint=ABAN3M --mountpoints-file=mountpoints.conf \
  --out-dir=./data --station=ABAN3M \
  --convbin=/usr/local/bin/convbin
```

Monitoreo en vivo del fichero RTCM:
```bash
watch -n 2 'ls -lh data/*.rtcm3 | tail -n 3'
```

## 5) Posición de estación en tiempo real (`rtcm_station_position`)

### Modo RTCM (1005/1006)
```bash
build/bin/rtcm_station_position \
  --mountpoints-file=mountpoints.conf \
  --mode=rtcm
```
Si no pasas `--mountpoint`, calcula para todos los mountpoints del conf en paralelo.

Salida en vivo: media ECEF, sigma3d y mejor precisión acumulada.

### Modo PPP (RTKLIB real)
```bash
export CONVBIN_BIN=/usr/local/bin/convbin
export RNX2RTKP_BIN=/usr/local/bin/rnx2rtkp
export PPP_CONF=scripts/ppp-static.conf

# Opcional: productos precisos
export PPP_PRODUCTS_DIR=/data/igs_products
# o explícitos:
# export PPP_SP3_FILE=/data/igs/igs.sp3
# export PPP_CLK_FILE=/data/igs/igs.clk
# export PPP_BIA_FILE=/data/igs/igs.bia
# export PPP_DCB_FILE=/data/igs/igs.dcb

build/bin/rtcm_station_position \
  --mountpoints-file=mountpoints.conf \
  --mode=ppp \
  --min-data-sec=1800 --solve-interval-sec=300 --progress-interval-sec=60 \
  --solver-cmd='scripts/rtklib_ppp_solver.sh {rtcm} {solution} {workdir}'
```

Monitoreo PPP en vivo (otra terminal):
```bash
tail -f data/*.ppp.sol
```

## Precision objetivo < 5 cm (PPP)
Regla práctica para PPP estático dual-frecuencia + productos precisos:
- Horizontal < 5 cm: ~20-40 min.
- Vertical < 5 cm: ~45-90 min.

Depende de multipath, entorno RF, máscara de elevación, antena y calidad de productos.

## Script PPP
`scripts/rtklib_ppp_solver.sh`:
- Convierte RTCM3 a RINEX (`convbin`).
- Ejecuta PPP (`rnx2rtkp`) con config `scripts/ppp-static.conf`.
- Entrega solución ECEF en formato `X Y Z SIGMA` para `rtcm_station_position`.
