# ESP32-C3 Conversation Service Notes

This project aligns the future ESP32 Wi-Fi conversation client with the Android app
repository `Android_nRF_BLE`.

## Reused gRPC Contract

- Proto file: `protocol/traini.proto`
- Service: `traini.ConversationService`
- RPCs:
  - `StreamConversation(stream AudioChunk) returns (stream ConversationEvent)`
  - `EndConversation(EndConversationRequest) returns (SessionSummary)`

## Default Endpoint

- Host: `traini-grpc-collar-dev-nlb-06c70e7556ee7ab5.elb.us-east-1.amazonaws.com`
- Port: `50051`

## Metadata

Android currently sends these gRPC metadata headers:

- `user-id`
- `session-id`

ESP should keep the same metadata contract when the actual gRPC client is implemented.

## Audio Format Defaults

Android configures the conversation stream as:

- sample rate: `24000`
- channels: `1`
- bit depth: `16`
- encoding: `pcm16`

## Audio Payload Behavior

Android currently uses:

- uplink audio: raw bytes, no base64
- downlink audio: base64 decode enabled
- downlink auto-detect base64: enabled

## Current ESP Status

The ESP project currently includes:

- Wi-Fi provisioning and STA networking
- cloud TCP reachability probe
- conversation service configuration and readiness skeleton

The actual HTTP/2 gRPC streaming transport is not implemented yet.
