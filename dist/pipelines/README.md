# Pipeline manifests

`<TITLE ID>.pmf`: the Vulkan pipelines play sessions have met for a game, without any game code:
each shader is an offset and length into the game's own shader package, plus the numbers the
shader translator and the pipeline key need. The game export rebuilds them from the player's
own copy of the game into the exported shader cache, so they compile at boot instead of the
first time they are drawn. Made and checked with `tools/a32recomp/pipeline_manifest.py`
(`make` from session caches, `check` for a byte-exact round trip).

- `0100770008DD8000.pmf`: Monster Hunter Generations Ultimate 1.4.0 (package
  `nativeNX/sa/NX/root.arc` -> `sc\NX\root`).
