# Ordinary start recovery fixture

The fixture is recorded from the engine-side historical producer; ingress
is reconstructed from its ordered manifest; these are not bytes from App's
receiver.

`recovery-recorded.json` preserves the recorded socket bytes and native failure
for the baseline and candidate runs. The baseline has no start frames; the
candidate emits a body frame followed by a metrics recovery frame. Build-specific
identities in this recording describe that historical build.

The automatic recovery test separately builds and invokes the real native
strategy with the control and trigger CSV inputs. It checks two native calls,
fresh serials, the start-frame contract, and preservation of the native failure.
The recorded fixture does not establish acceptance of a current App receiver.
