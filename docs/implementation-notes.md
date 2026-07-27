# Implementation notes

## Subscription recovery and shutdown locking

Jobs and Secure Tunneling use a subscription-lifecycle mutex to serialize
recovery subscription queueing with `stop()`. Recovery holds this mutex across
the `Subscribe*` calls, and `stop()` acquires the same mutex before teardown, so
shutdown waits for the queueing phase to finish.

Recovery callbacks use separate recovery-state mutexes and generation checks to
track progress and reject stale work. The current design assumes that the
pinned SDK/CRT `Subscribe*` path queues MQTT work without synchronously
re-entering connection-resumed handlers. Revalidate this assumption when
dependency pins, forwarding wrappers, or callback routing change.

## Linux release compatibility baseline

Ubuntu 22.04 (Jammy) is the minimum Linux userspace target by design.
Pull-request validation and manual-release artifact builds therefore use the
same Jammy runner baseline and Release build recipe.
