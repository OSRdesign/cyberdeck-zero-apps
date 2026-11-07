# Deck Atlas test fixtures

`tiles/` is a z/x/y tile folder of six **synthetic** 256 px palette PNG tiles (zoom 15: x 16590-16591, y 11268-11269, a
2x2 block, and its parents at zoom 14, 8295/5634, and zoom 13, 4147/2817). They are not in the repository: the build
(`build.sh host`) writes them with `tests/tools/gen_fixture_tiles.py` (deterministic, standard library only). Each tile
is a grid, its own z/x/y as text, a few invented roads, an invented coastline and a park polygon in one shared
coordinate system, so neighbours and parents line up. Nothing is derived from real map data, and no real map tile (the
owner's set, or any OpenStreetMap-style tile) is ever kept in this repository.

The demonstration mode of the app (`DECKATLAS_DEMO=1`, the headless scenes) converts the folder with the engine's folder
import into the demonstration store, and the file browser scenes open it. `broken.mbtiles` is a one-line text file
("not an MBTiles file"), for the import error page.

To look at real tiles, set `DECKATLAS_TILES` (see `tests/real/run.sh`): the folder is only read, never copied here.
