# Core patches

Optional. The module builds and runs on an unpatched core; each patch here
removes one documented limitation.

| Patch | Removes | Applies to |
|---|---|---|
| `azerothcore-melee-outcome-gates.patch` | glancing and crushing blows gated on the creature's real level rather than the level the hook corrected | upstream `master` and the mod-playerbots fork, verified 2026-09-16 |

```bash
cd azerothcore-wotlk
git apply modules/mod-zone-scaling/patches/azerothcore-melee-outcome-gates.patch
```

Each patch is written so that a core with no module loaded behaves exactly as
before it was applied, which is what makes them candidates for upstream.
