# Local partition regression fixtures

`remix-forward-regions-test --partition-fixture FILE` checks trial reflection
capture sites against a saved visibility partition. With no arguments, the
executable runs its normal synthetic regression tests.

This optional diagnostic runs the production occupancy, site spreading and room
fitting code. It prints JSON containing candidate enclosure counts, clearance,
distance to the nearest of 128 spread automatic sites, and fitted boxes. It does
not validate render geometry on the GPU or establish reflection quality.

Fixtures are local test data, not a supported runtime interchange format. Do not
commit extracted map content. The little-endian layout is:

| Field | Storage |
| --- | --- |
| Magic | Eight bytes `RMXPART1` |
| Node, leaf, cluster, trial site counts | Four unsigned 32-bit integers |
| Metres per scene unit | One 32-bit float |
| Search bounds | Minimum XYZ, maximum XYZ; six 32-bit floats |
| Partition nodes | Plane XYZW floats, two signed child indices, two reserved signed integers; 32 bytes each |
| Leaf clusters | One signed 32-bit integer per leaf; -1 means solid/outside |
| Trial positions | Three 32-bit floats per site |

Node zero is the root. Nonnegative children index nodes; negative children encode
leaf index as `-1 - child`. Plane orientation must agree with the partition's
child order. Search bounds should match production scene bounds for comparisons;
raw map model bounds can include a distant skybox.

The reader rejects malformed sizes, cycles reachable from the root, invalid
indices, nonfinite coordinates and invalid bounds before fitting. Counts and file
size are bounded. Fixture input does not bypass occupancy checks.
The JSON output also includes each fitted box's `influence` bounds, using the
production default 0.5-metre feather width and detail tolerance. The top-level
`searchBoundsEmpty` and `searchBoundsDetailCompatible` flags assess the supplied
search box with strict occupancy and the same 0.5-metre detail policy. These help
compare enclosing authored volumes with conservative automatic fits; they do not
establish capture visibility or reflection quality.
