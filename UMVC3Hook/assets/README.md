# Plugin-shipped art

`space_stars_512.dxt1` is the star layer of the lobby's space backdrop
(`code/space_bg.cpp`): one 512x512 tile, raw DXT1 blocks (128 x 128 blocks of
8 bytes, no header). It is embedded in the plugin as the `SPACE_STARS`
resource.

Source: "Blue Nebula 6" from *Seamless Space Backgrounds* by Screaming Brain
Studios, https://opengameart.org/content/seamless-space-backgrounds, released
under CC0 / public domain ("free to use in any and all projects, commercial or
non-commercial, with no restrictions, with or without credit"). It was
recoloured towards dark blue and purple, brightened, and scaled down from
1024x1024. The
image tiles seamlessly in both directions.

`BaiJamjuree-Medium.ttf` and `BaiJamjuree-Bold.ttf` are the installers' font
(`tools/timestone_install.cpp`, `tools/timestone_install_linux.cpp`): Medium
for the text, Bold for titles and buttons. Both programs carry the files
whole. Bai Jamjuree is by Cadson Demak, from
https://github.com/google/fonts/tree/main/ofl/baijamjuree, unmodified, under
the SIL Open Font License 1.1 (text in `THIRDPARTYNOTICES`). It covers Latin
and Thai; other characters fall back to a system font.

Nothing here comes from the game.

## Rebuilding the star tile

- `space_stars_source.png` - the 1024x1024 seamless source picture (the CC0
  nebula above, recoloured).
- `space_stars_512.dxt1` - what the plugin embeds. Regenerate it with
  `python3 tools/make_space_stars.py` (options: source, output, `--gain`,
  `--preview out.png`), then `make -j8`.
- `space_grid.png` - one grid cell drawn by the plugin's authors to the line
  profile `space_bg.cpp` uses (core 0.46/0.74/0.86 at alpha 230, soft halo).
  Reference only: the grid is drawn as lines in code and this file is not
  embedded.
