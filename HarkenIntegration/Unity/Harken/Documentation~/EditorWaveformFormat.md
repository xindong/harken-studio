# Editor waveform publication status

The standalone `HarkenEditorWaveforms.json` format and its legacy manifest
pairing are retired. The SDK no longer parses them, and the offline
`harken-editor-metadata` tool has been removed.

Current Pack companions may include optional `metadata` and `waveforms`.
Runtime playback does not depend on either field. Unity waveform browsing and
preview remain suspended until those tools consume current companions.
`HarkenWaveformPublication.Parse` returns `VN_CAPABILITY_UNAVAILABLE`.

Re-export resources with the current Studio. No Pack or waveform migration is
provided; Studio project migration remains supported independently.
