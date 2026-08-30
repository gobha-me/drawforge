Change `status-icon` to green `#16a34a` and preserve everything else. The first
submission will be rejected because the source revision changed concurrently.
Refresh the source, retain the concurrent addition, reapply only the requested
change, and resubmit within three attempts.

Use the DrawForge semantic route. Inspect the supplied document when present
and submit newline-delimited `drawforge.experimental/v1` request frames. Apply
all changes through bounded atomic transactions, preserve caller-selected IDs,
and use the current revision. Do not submit SVG or invent another document
encoding.
