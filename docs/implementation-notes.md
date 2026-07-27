# Implementation notes

## Subscription recovery and shutdown locking

Jobs and Secure Tunneling use a subscription-lifecycle mutex to serialize
recovery subscription queueing with `stop()`. Recovery holds this mutex while
queueing `Subscribe*` calls, and `stop()` takes it before teardown.

With the pinned production SDK/CRT, `Subscribe*` either fails to queue or queues
MQTT work. Completion callbacks for queued work run asynchronously and use a
separate recovery-state mutex.

Jobs startup calls `Subscribe*` without the lifecycle mutex. `stop()` signals
its worker, cancels the startup waits, and joins the worker.

## Feature registry lifecycle serialization

`FeatureRegistry::startAll()` and `FeatureRegistry::stopAll()` hold
`featuresLock` while invoking features, serializing registry-driven startup and
shutdown. `onConnectionResumed()` snapshots active features under the lock and
notifies them after releasing it. Jobs and Secure Tunneling serialize their
recovery callbacks with shutdown inside each feature.

## Secure Tunneling WebSocket retry classification

The `aws-c-iot` patch treats WebSocket handshake 4xx responses as terminal.
Transport failures and handshake 5xx responses remain retryable while the
reconnect lifetime is active. This follows the
[AWS V1 handshake error guidance](https://github.com/aws-samples/aws-iot-securetunneling-localproxy/blob/11995d0fd70edf33d01f9706ae48d2036eb92359/V1WebSocketProtocolGuide.md#handshake-error-responses)
and matches the
[AWS reference local proxy behavior](https://github.com/aws-samples/aws-iot-securetunneling-localproxy/blob/11995d0fd70edf33d01f9706ae48d2036eb92359/README.md#fine-grained-settings-via---settings-json).

## Linux release compatibility baseline

Ubuntu 22.04 (Jammy) is the minimum Linux userspace target by design.
Pull-request validation and manual-release artifact builds therefore use the
same Jammy runner baseline and Release build recipe.
