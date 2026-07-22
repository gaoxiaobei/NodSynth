# NodSynth

NodSynth is a node-based polyphonic software synthesizer. The current milestone implements the platform-independent graph model and compiler.

## Build and test

```sh
cmake --preset dev
cmake --build --preset dev -j2
ctest --preset dev
./build/dev/nod_graphcheck --scenario valid
./build/dev/nod_graphcheck --scenario type-error
./build/dev/nod_graphcheck --scenario cycle
```

The realtime audio engine and JUCE application are intentionally outside this milestone; see the approved MVP design under `docs/superpowers/specs/`.
