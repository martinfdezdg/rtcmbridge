# Architecture

## Layout
- `include/rtcmbridge/core`: reusable GNSS/NTRIP/RTCM headers.
- `src/core`: reusable implementation shared by tools.
- `src/tools`: independent binaries.
- `scripts`: helper scripts (PPP solver bridge to RTKLIB).

## Binaries
- `rtcm_bridge`: NTRIP->NATS bridge (independent service).
- `rtcm_decoder_ntrip`: direct decoder from caster.
- `rtcm_decoder_nats`: decoder from NATS topic.
- `rtcm_rinex_recorder`: captures RTCM and optionally converts to RINEX via `convbin`.
- `rtcm_station_position`: real-time station position monitor.
  - `mode=rtcm`: reads 1005/1006 and estimates stable ECEF.
  - `mode=ppp`: runs an external PPP solver periodically from recorded RTCM.

## Core reuse
- `RtcmFrameParser`: stream-safe RTCM framing + CRC24Q.
- `decode_station_position_1005_1006`: antenna reference point extraction from RTCM.
- `NtripStreamClient`: reconnecting authenticated NTRIP client.

