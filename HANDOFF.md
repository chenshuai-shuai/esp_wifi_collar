# Handoff — `esp_wifi_collar` (ESP32-C3)

> 交接对象：下一个接手 gRPC 语音对话链路的同事  
> 当前固件版本：`v0.0.12-oom-fix-queue48`（`main/firmware_version.h`）  
> 上一个 git commit：`840a2db` (音频链路 + SpeexDSP 降噪增强)  
> 本文档只描述**相对 840a2db 的改动**，不重复已有 commit log 里的内容。

---

## 1. 本轮工作的目标与成果

- **目标**：让 ESP32-C3 像 Android 伙伴端一样，以 gRPC (HTTP/2 明文 h2c) 与
  `traini-grpc-collar-dev-nlb-...:50051/traini.ConversationService/StreamConversation`
  建立**长连接双向流**，把麦克风 PCM24k/16-bit 实时推上去、并把服务器返回的 TTS 音频播出来。
- **起点**：老的 `services/conversation_service.c`（2100 行）只是自测链路，只能
  发 150 个 chunk 就停，而且发送缓冲在栈上，容易 stack overflow；整个文件里混了
  chunk 生成、pb 封包、nghttp2 session、重连、selftest loop，难以维护。
- **现状（build=12 编译通过）**：
  1. 老 `conversation_service` 彻底删掉，重写为独立组件 `components/conversation`，
     文件只有一个 `conversation_client.c`（~1200 行）。
  2. protobuf 生成从 CMake 硬编码流程抽离成 `components/proto_gen`，配合
     `protocol/traini.proto` + 新增的 `protocol/traini.options`（nanopb 字段大小）。
  3. HTTP/2 栈切换到 **官方 `espressif/nghttp`** 的 nghttp2（通过
     `components/conversation/idf_component.yml` 和 `managed_components/` 引入）。
  4. **nanopb** 通过 IDF component manager 拉入，不再手工放源码。
  5. app_manager 现在是一条干净的状态机（CONV_START / TALK / STOP 控制台命令驱动），
     负责把麦克风 PCM → `conversation_client_send_audio()`，并把下行音频
     回调给 speaker。
  6. 新增 `main/firmware_version.h`：每次 build 手动 bump 一个数字，
     `app_main` 启动时打印一行 `FW-VER: ...`，避免烧到旧 bin 自己都不知道。
  7. 大量 h2/tcp 诊断 log（`h2-stall`, `post-connect probe`, hex-dump 等）。
- **目前**：TCP + h2c 握手、SETTINGS、HEADERS 全部 OK，服务器侧 `post-connect
  probe: server spoke within ~900 ms (healthy)`。**上行音频还没跑通**——
  最新一次 build=11 日志显示 `build_audio_frame` 因为 OOM 而一直失败（`free_heap=2720 B`）。
  build=12 已把 TCP 缓冲和发送队列深度回退到合理值，等用户实测 log。

---

## 2. 工程结构总览（相对 840a2db 的变化）

```
esp_wifi_collar/
├── CMakeLists.txt
├── main/
│   ├── app_main.c              [改] 仅 +14 行：启动打印 FW-VER、拉起 conversation_client
│   ├── firmware_version.h      [新] 只有 3 个宏，build 号手动维护，给 boot log 用
│   ├── Kconfig.projbuild       [重写] 大改：conversation/mic/speaker/denoise/host 全部归一
│   └── CMakeLists.txt          [改] 多一个 INCLUDE 自己目录（为了 firmware_version.h）
│
├── protocol/
│   ├── traini.proto            [沿用 840a2db]
│   └── traini.options          [新] nanopb fixed-size / callback 标注
│
├── components/
│   ├── conversation/           [新组件]  ★ 本轮工作核心
│   │   ├── CMakeLists.txt
│   │   ├── idf_component.yml   ← 声明 nanopb、espressif/nghttp 依赖
│   │   ├── include/conversation/conversation_client.h
│   │   └── conversation_client.c  ← gRPC client 全部实现（见 §3）
│   │
│   ├── proto_gen/              [新组件]  ← 把 .proto 编译成 nanopb .pb.c/.pb.h
│   │   └── CMakeLists.txt      （build 时自动调用 nanopb_generator.py）
│   │
│   ├── services/
│   │   ├── conversation_service.c         [删]
│   │   ├── include/services/conversation_service.h  [删]
│   │   ├── CMakeLists.txt                 [改] 去掉 conversation_service
│   │   ├── cloud_service.c                [改] 仅小修：不再因 conversation
│   │   │                                        重探测时误清 reachable
│   │   ├── service_manager.c              [改] 去掉 conversation_service 启动点
│   │   └── wifi_service.c                 [改] +40 行：把 got_ip 通过 kernel msgbus
│   │                                              发给 conversation_client
│   │
│   ├── app/
│   │   ├── CMakeLists.txt                 [改] 依赖 conversation 组件
│   │   └── app_manager.c                  [大改 ±645 行] 重写成：
│   │       - 控制台命令: CONV_START / TALK / STOP
│   │       - mic-uplink task: 把 PCM chunk (960 B / 20 ms) 喂给
│   │         conversation_client_send_audio()
│   │       - 下行: conversation_client listener → speaker_output_push_pcm()
│   │
│   ├── platform_hal/log_control.c         [改] +99 行：日志白名单，
│   │                                             main/conv_cli/wifi_svc 强制 INFO
│   │
│   ├── bsp/                               [沿用 840a2db]
│   ├── audio_dsp/                         [沿用 840a2db]
│   ├── kernel/                            [沿用 840a2db]
│   └── platform_hal/ ...                  [沿用 840a2db]
│
├── managed_components/        [新] IDF component manager 自动拉下的 nanopb / nghttp
├── dependencies.lock          [新] 锁定 managed_components 版本
│
├── sdkconfig                  [改]（见 §4 配置变更）
│
├── HANDOFF.md                 [新] 本文档
├── ESP_GRPC_CONVERSATION_NOTES.md [删]（老的自测笔记，已无用）
├── ESP_NRF_SYSTEM_CONTRACT.md      [沿用]
├── NRF_ESP_FLASH_BRIDGE_SPEC.md    [沿用]
└── tools/                          [沿用]
```

---

## 3. `components/conversation/conversation_client.c` —— 关键代码地图

> 这是整个工程现在最复杂的一个文件，按**自顶向下**阅读顺序列出：

| 段落 | 行为 | 要点 |
|---|---|---|
| `typedef conv_state_t s_conv` | 全局单例 | `configured / wifi_ready / transport_ready / stream_ready / conv_state / tx_queue / cur_frame`，高-低两层状态机共用 |
| `build_audio_frame()` | 把 960 B PCM 封成 `traini.AudioChunk` → 5 字节 gRPC length-prefix → malloc 返回 | **当前卡点：malloc 失败→返回 ESP_ERR_NO_MEM（修复后不会再误发 END_STREAM）** |
| `h2_data_source_read_cb()` | nghttp2 要 DATA 时调用 | 队列空→`NGHTTP2_ERR_DEFERRED`；build_audio_frame 失败→丢这一 chunk 返回 `DEFERRED`（**build=11 的 critical fix**，原来误发 EOF 导致 stream half-closed 永久卡死）|
| `h2_on_frame_recv_cb()` | 收 SETTINGS/HEADERS/WINDOW_UPDATE/GOAWAY | 区分 `stream_id == 0` 的连接级控制帧和 stream 级帧；以前误把 SETTINGS ACK 当 END_STREAM |
| `h2_on_data_chunk_recv_cb()` → `handle_downlink()` → `conv_emit_audio_output()` | 下行 DATA → gRPC length-prefix 切帧 → nanopb decode ConversationEvent（oneof） → 按 audio_output / audio_complete / error 分发给 listener | 自动识别 base64 回退为 raw PCM |
| `tcp_connect_blocking()` | `getaddrinfo` + `connect` + **post-connect select 探测 2 s**（决定性诊断：`healthy` vs `SERVER SILENT`） | 失败时 `conv_state → IDLE` |
| `conv_open_transport()` / `conv_open_rpc()` | 懒连接：只有用户 CONV_START 时才 TCP+h2；每个 CONV_START 一个新 stream | SETTINGS_INITIAL_WINDOW_SIZE=1 MiB, connection-level local window=1 MiB |
| `conv_task()` | 常驻 worker task | 轮询：wifi_ready? → open_transport? → open_rpc? → `h2_pump_io()` → 30 s 周期 PING → 每秒 `h2-stall:` 诊断行 |
| `conversation_client_send_audio()` | 生产者：mic-uplink task 调用 | DROP_OLDEST 策略，与 Android 一致 |
| `conversation_client_send_end_rpc()` | **独立**的一元 RPC `EndConversation`，会自己开 TCP+h2 发一次就关 | 不走长连接，避免复用 stream 状态 |

---

## 4. `sdkconfig` 重要改动速查

| Key | 变化 | 原因 |
|---|---|---|
| `CONFIG_COLLAR_CONV_HOST` | 空 → `traini-grpc-collar-dev-nlb-...amazonaws.com` | 目标 NLB |
| `CONFIG_COLLAR_CONV_PORT` | 新增 `50051` | gRPC h2c |
| `CONFIG_COLLAR_CONV_USER_ID` | 新增 `"CollarOne"` | header 里一直发 |
| `CONFIG_COLLAR_CONV_AUDIO_BASE64` | 新增 → `n` | **对齐 Android（实测 Android 直接送 raw PCM bytes，不 base64）** |
| `CONFIG_COLLAR_CONV_AUDIO_SAMPLE_RATE/CHANNELS/BIT_DEPTH/ENCODING` | 新增 | 24k / 1 / 16 / "pcm16" |
| `CONFIG_COLLAR_CONV_RETRY_INTERVAL_SEC` | 新增 `10` | 连失败退避 |
| `CONFIG_LWIP_TCP_MSS` | 1440 → **1200** | 保险，防 PPPoE/NLB 路径 MTU 黑洞 |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | 5760 → ~~12288~~ → **5760** | build=10 改大；build=11 日志显示 OOM；**build=12 已回退** |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | 同上 | 同上 |
| `CONFIG_COLLAR_CLOUD_PROBE_*` | 沿用 | cloud probe 路径 |
| `CONFIG_COLLAR_MICROPHONE_STREAM_ENABLE` | 默认 `n` | 过去 UDP 抓包排查专用，生产下关闭 |

> `CONV_TX_QUEUE_DEPTH` 不是 Kconfig，是 `conversation_client.c` 里的
> 宏，当前是 **48**（build=12）。之前试过 128 / 256 都因为 ESP32-C3 RAM 吃紧
> 触发 malloc 失败。

---

## 5. build / flash 流程

```bash
cd esp_wifi_collar
idf.py set-target esp32c3   # 只第一次
idf.py build

# 下载模式（复用了 nRF 共享 UART 桥）
python3 tools/nrf_shared_uart_flash.py --port /dev/ttyUSB0 download
esptool.py --chip esp32c3 --port /dev/ttyUSB0 -b 115200 --no-stub \
    --before no_reset --after no_reset write_flash \
    --flash_mode dio --flash_freq 40m --flash_size 2MB \
    0x0    build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0x10000 build/esp_wifi_collar.bin
python3 tools/nrf_shared_uart_flash.py --port /dev/ttyUSB0 boot
idf.py -p /dev/ttyUSB0 monitor
```

> **强烈建议每次改代码就 bump `FW_VERSION_BUILD`。** 开机日志会打
> `FW-VER: v0.0.N-<codename> (build=N)`，一眼确认烧到的是不是新 bin。
> 这次排查有好几轮时间花在“怀疑没烧进去”上。

---

## 6. 启动 → 一次对话的典型日志（期望路径）

```
FW-VER: v0.0.12-oom-fix-queue48 (build=12)
...
I wifi_svc: got IP 192.168.x.x
I conv_cli: Wi-Fi ready -> arming conversation client
I app_mgr: console: cmd='ESP:CONV_START'
I conv_cli: conv-state: IDLE -> CONNECTING
I conv_cli: start_conversation: id=sess-18544
I conv_cli: conv-state: CONNECTING -> TALKING
I conv_cli: TCP connecting to traini-...:50051 ...
I conv_cli: post-connect probe: server spoke within 881ms (healthy)
I conv_cli: h2c session ready (ok=1 fail=0)
I conv_cli: RPC stream opened id=1 ...
I app_mgr: mic-uplink: sent=1 chunks (seq=1, 20ms each)
I conv_cli: h2-stall: sent/s=40 tx_q>0 wr=1 rd=1 rem_win=... rx>9
I conv_cli: HTTP/2 HEADERS received on stream 1
I conv_cli: h2 DATA: NN B
I conv_cli: DL event: audio_output seq=0 payload=... pcm=...
I conv_cli: DL event: audio_complete total=K
I conv_cli: grpc-status trailer: 0
```

---

## 7. 已验证 / 未验证

| 环节 | 状态 |
|---|---|
| Wi-Fi 配网 & STA got_ip | ✅ |
| cloud probe（NLB 可达性） | ✅ |
| DNS 解析 + TCP 三次握手 | ✅ |
| HTTP/2 client-preface + SETTINGS 握手 | ✅（build=10 post-connect probe=healthy）|
| `nghttp2_submit_request` 成功，HEADERS 发出去 | ✅ |
| 服务器回 HEADERS + grpc-status | ⚠️ 日志里见过 `HTTP/2 HEADERS received`，但偶发 |
| 上行 DATA 稳定流 | ❌ 前 5 个 chunk 能发，第 6 个开始 OOM → 丢帧（build=12 应修复）|
| 下行 audio_output | ❌（上行没稳定就等不到回放）|
| EndConversation unary RPC | ⚠️ 代码已写 (`conversation_client_send_end_rpc`)，还没真跑过 |

---

## 8. 下一步建议（按优先级）

1. **烧 build=12，发 CONV_START 讲 5~10 秒**，观察：
   - `free_heap` 稳不稳（`h2-stall:` 诊断 / `build_audio_frame failed` 告警）；
   - `sent/s` 有没有持续 > 0；
   - `h2 DATA:` 有没有下行。
2. 如果 OOM 还在：
   - 临时把 `CONFIG_COLLAR_CONV_AUDIO_BASE64` 保持 off（已是），确认省的那 33% 到位；
   - 考虑把 `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32` 降到 16；
   - 或在 `build_audio_frame` 里做一个预分配 pool（frame_len 上限已知），避免每帧 malloc/free。
3. 下行稳定后：
   - 补 `EndConversation` unary 调用路径（现在 `end_conversation` 只 half-close）；
   - 补 barge-in（`audio_start_cb` 已在 first audio_output 触发）。
4. 可维护性：
   - `conversation_client.c` 已经 1200 行；下一轮可以把 "end_rpc_*" 那组 unary RPC 抽到单独 `conversation_end_rpc.c`。

---

## 9. 坑位备忘（别再踩）

- **ESP-IDF 的 lwIP 没有 `TCP_MAXSEG` socket option**，想限 MSS 只能改
  `CONFIG_LWIP_TCP_MSS`。
- **nghttp2 session 不是线程安全**：不要在 mic-uplink task 里直接调 `nghttp2_session_send/resume_data`
  会 race。现在 `send_audio()` 调 `resume_data` 是“探测”，**真正** resume 在 `conv_task`
  的 pump tick 里再调一次（注释里写清楚了）。
- **`data_flags |= NGHTTP2_DATA_FLAG_EOF`** 等价于 Android 里 `onCompleted()`，
  任何 build 失败路径都不要走它，否则 stream 就废了（`want_write` 永远 0）。
- **TCP/HTTP2 窗口和队列深度加大要一起看 heap**。ESP32-C3 只有 ~400 KB RAM，
  任何“提升稳定性”的大 buffer 叠加都可能反过来触发 OOM。
- **ESP32-C3 是单核**，`conv_task` 优先级 7。如果再加业务 task 要注意不要
  长时间占 CPU 导致 h2 pump 跑不动。
