# Cast remoting (native elementary-stream passthrough) — design

**Status:** design / investigation. The transport exists in openscreen; the *content
pipeline* does not. Live validation is gated on the target TV (see §6).

## 1. Problem

To cast a movie file at the device's **native fidelity** (4K, HDR10/Dolby Vision,
HEVC 10-bit, Dolby AC-3/E-AC-3/Atmos) we must **not transcode**. Two facts force a
protocol-level solution:

- The **Default Media Receiver** on the target (Philips 43PUS9235, Android TV) plays
  HEVC/4K/HDR10 natively but **does not pass through Dolby audio** (empirically: AC-3 →
  silent; per Cast docs, AC-3/E-AC-3/Atmos passthrough is a *Web Receiver SDK* feature,
  not the bare DMR). Reaching it needs a registered/custom receiver (Google-gated, not
  libre) — rejected.
- The **mirroring** path (our `cast_sender` file path, VAAPI H.264) **re-encodes** to
  1080p SDR H.264 — a hard fidelity ceiling. Good as a safety net, wrong as default.

The libre, no-third-party-app, protocol-correct answer is **Cast Streaming remoting**:
the sender ships the file's **original compressed elementary streams** to the receiver's
**native decoder** over the existing RTP transport. This is how Chrome remotes Netflix/
YouTube. No transcode, no registration, no Kodi/VLC.

## 2. How remoting works (CASTv2 + Cast Streaming)

Remoting runs *inside* a normal mirroring-app session (same LAUNCH + OFFER/ANSWER), but
instead of capture→encode→RTP, the sender exposes a **DemuxerStream** the receiver's
Renderer pulls from. Control rides an RPC channel (`cast/streaming/remoting.proto`,
base64 binary messages of `SenderMessage::Type::kRpc`); compressed frame **payloads ride
the RTP audio/video Senders** (`Sender::EnqueueFrame`), exactly like mirroring but with
original bytes.

RPC flow (receiver ⇄ sender), per `remoting.proto`:

1. `RPC_ACQUIRE_RENDERER` / `RPC_ACQUIRE_DEMUXER` — receiver acquires our demuxer handles.
2. `DemuxerStreamInitializeCallback` — sender declares `AudioDecoderConfig` /
   `VideoDecoderConfig` (codec, profile, extradata, sample-rate/channels, coded size).
3. `RPC_R_INITIALIZE` → `RPC_R_INITIALIZE_CALLBACK` — renderer init.
4. `RPC_R_STARTPLAYINGFROM(time)` — begin playback at a timestamp.
5. `DemuxerStreamReadUntil(count)` — receiver requests N frames; sender enqueues `count`
   `DecoderBuffer`s (metadata: `timestamp_usec`, `duration_usec`, `is_key_frame`,
   `is_eos`) and pushes the **compressed payloads onto the RTP Sender**, then replies
   `DemuxerStreamReadUntilCallback(count, status)`.
6. Control: `RPC_R_SETPLAYBACKRATE`, `RPC_R_SETVOLUME`, `RPC_R_FLUSHUNTIL` (seek).
7. Client callbacks sender-ward: `RPC_RC_ONTIMEUPDATE`, `ONBUFFERINGSTATECHANGE`,
   `ONENDED`, `ONERROR` (drives our position/end/seek state).

Capability negotiation: `SenderSession::RequestCapabilities()` sends `GET_CAPABILITIES`;
the receiver returns `RemotingCapabilities { audio[], video[] }` (`remoting_capabilities.h`).

## 3. What openscreen has vs. the gap

**Has (transport + handshake):**
- `SenderSession::NegotiateRemoting(audio, video)`, `RequestCapabilities()`,
  `OnCapabilitiesDetermined()`, `RpcMessenger` (`cast/streaming/public/sender_session.*`).
- `RemotingSender` (`cast/standalone_sender/remoting_sender.*`) — but it is **handshake
  only**: it answers the receiver's initialize RPC with the pre-known codecs and fires
  `OnReady()`. Its own header says: *"we don't have a fully functional implementation of
  remoting … Chrome is the reference implementation."*
- After `OnReady()`, the agent calls `StartFileSender()` → the **same `LoopingFileSender`
  that decodes + re-encodes**. So `cast_sender --remoting` today still re-encodes.

**Gap (content pipeline) — what to build:**
1. A **demuxer** producing *compressed* `AVPacket`s (libavformat, no decode) for the
   selected audio+video streams.
2. **Codec-config mapping** ffmpeg → `remoting.proto` `AudioDecoderConfig` /
   `VideoDecoderConfig` (codec id, profile/level, `extra_data`/codec_private, sample
   format, channel layout, coded/visible size, color space + HDR metadata).
3. A **DemuxerStream RPC server**: handle `ACQUIRE_DEMUXER`, emit
   `DemuxerStreamInitializeCallback`, service `DemuxerStreamReadUntil(count)` by
   delivering `count` frames, emit `RPC_RC_*` callbacks, handle FlushUntil (seek) and EOS.
4. **Compressed-frame RTP push**: wrap each `AVPacket` as an `EncodedFrame` (frame_id,
   rtp_timestamp from `timestamp_usec`, `dependency`=key/non-key) and `Sender::
   EnqueueFrame()` on the audio/video Sender — reusing the existing RTP/RTCP stack.
5. **Playback control** wiring: map `STARTPLAYINGFROM/SETPLAYBACKRATE/SETVOLUME/
   FLUSHUNTIL` to demux seek + pacing; surface `ONTIMEUPDATE/ONENDED` to the caller.

This is, in effect, a port of Chrome's `media/remoting` *sender* side (StreamProvider +
the renderer-controller RPC), minus the mojo plumbing.

## 4. Audio model extension (Dolby)

`remoting_capabilities.h::AudioCapability` is `{ kBaselineSet, kAac, kOpus }` — **no
AC-3/E-AC-3** (the comment flags it as a "rough set", TODO to expand). Chrome's actual
remoting checks a broader codec set. To remote Dolby we must:
- Map ffmpeg `AV_CODEC_ID_AC3`/`AC_EAC3` → `remoting.proto` `AudioDecoderConfig.codec`
  (the proto enum already models more codecs than the capability enum).
- Decide passthrough from the receiver's reported caps + an empirical accept test (§6),
  since `kBaselineSet` is the only signal the receiver may give for Dolby.

## 5. Fallback ladder (all libre, graceful degradation)

1. **Remoting passthrough** (this design) — any codec the receiver natively decodes:
   HEVC/HDR/DV + AC-3/E-AC-3/Atmos, zero transcode. *Primary, once validated.*
2. **DMR + audio-only remux** — video `-c copy` (HEVC 4K HDR), audio Dolby→AAC 5.1,
   progressive MP4 `+faststart`. No registration; loses Atmos object layer. *Interim/
   fallback when remoting is unavailable.*
3. **VAAPI H.264 1080p mirror** (`looping_file_sender.cc`, shipped) — last resort for
   anything the above can't carry (DRM/unremuxable). Forfeits 4K/HDR/lossless audio.

## 6. Empirical gates (only the TV can answer — defer to on-site)

The central risk: **does this TV's (closed) mirroring receiver accept a remoting
DemuxerStream from a non-Chrome sender, and feed its native decoder?** Chrome's receiver
supports it; whether it gates on sender identity / a specific RPC shape is unknown.

Ready-to-run probes (built, not yet executed):
- `cast_sender --probe-caps -n <tv-ip>:8009 <any-file>` → logs the receiver's
  `RemotingCapabilities` (does it advertise HEVC, 4k, the audio baseline?). **First test.**
- After the content pipeline exists: a real remoting attempt with a 1080p HEVC clip;
  watch for `RPC_ACQUIRE_DEMUXER` → `STARTPLAYINGFROM` → frames decoded on-screen vs a
  `RemotingError` / `kRemotingNotSupported`.

If the receiver refuses non-Chrome remoting, the protocol path is blocked on the receiver
side and we fall to ladder rung 2 (DMR + audio remux), still libre and far above the
1080p mirror.

## 7. Phasing

- **P0 (done):** identify the protocol gap; wire `--probe-caps`; ship the H.264 mirror
  safety net (R1–R4).
- **P1:** run `--probe-caps` on-site; decide go/no-go on remoting from the advertised caps
  and a minimal `--remoting` accept test.
- **P2 (if P1 green):** build the demuxer + DemuxerStream RPC server + compressed RTP push
  for **video-only HEVC** first (audio still Opus via the existing path) — smallest slice
  that proves native video passthrough.
- **P3:** add audio passthrough (AAC, then AC-3/E-AC-3), extend the capability/codec model.
- **P4:** playback control (seek/rate/volume) + position/end callbacks; nstream integration
  (a `cast_mode` selector: remoting → DMR-remux → mirror) and `--json` reporting.

## References (in-tree)
- `cast/streaming/remoting.proto` — RPC + DecoderBuffer/configs.
- `cast/streaming/remoting_capabilities.h` — capability enums (audio under-models Dolby).
- `cast/streaming/public/sender_session.{h,cc}` — `NegotiateRemoting`, `RequestCapabilities`.
- `cast/standalone_sender/remoting_sender.{h,cc}` — handshake-only stub to extend.
- `cast/standalone_sender/looping_file_cast_agent.cc` — session workflow; `--probe-caps`
  branch in `CreateAndStartSession` + `OnCapabilitiesDetermined`.
- `cast/streaming/public/sender.h` — `EnqueueFrame(EncodedFrame)` (the RTP push API).
