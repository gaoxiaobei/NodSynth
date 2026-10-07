# libebur128

Unmodified upstream implementation, version 1.2.6.

- Repository: https://github.com/jiixyj/libebur128
- Pinned commit: `67b33abe1558160ed76ada1322329b0e9e058b02` (tag `v1.2.6`).
- Source archive: https://codeload.github.com/jiixyj/libebur128/zip/67b33abe1558160ed76ada1322329b0e9e058b02
- Library license: MIT, see COPYING.
- Bundled queue implementation: BSD license, see licenses/queue.txt and source header.

NodSynth builds the C source as a static library with its internal queue header.
No network access is needed during configuration or builds. This is an offline
meter; the histogram mode bounds its integrated/LRA history storage. Short-term
and true-peak results retain their separate LUFS, LU and dBTP units.
