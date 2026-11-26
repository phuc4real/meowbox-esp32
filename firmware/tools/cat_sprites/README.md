# cat_sprites: the MeowBox cat

`sprites.py` is the **only source**. Every sprite is drawn as text in that file, one character per pixel. The C code
in `firmware/components/cat_sprites/` is **generated**: don't edit it by hand.

The Meow Coding tile uses the adult cat: `SIT`, `BLINK`, `SLEEP1/2`, `HAPPY`, `SAD`, `ANGRY`, and the coding poses
`CODE1/2`, `THINK`, `WAIT`, which `sprites.py` builds from `SIT`/`BLINK` with the `LAPTOP` and `PAW` overlays. Props:
`ZZZ`, `SPARKLE`, `QUESTION`, `DOTS`. The virtual pet's other ages, poses and props are in git history (before 0.2.5).

## Editing a sprite

1. Open `sprites.py` and find `SPRITES["SIT"]` (or `PROPS["ZZZ"]`, `LAPTOP`...).
2. Change the characters. Cat sprites are 32x32, props 16x16 (the script checks the size).
3. Run:

```
python sprites.py            # check + write C + render previews
python sprites.py gen        # write C only
python sprites.py preview    # previews only (needs Pillow)
python sprites.py dump sit   # print a sprite to copy or compare
```

4. Look at `preview/cat_<fur>.png` (every pose), `preview/all.png` (all 6 furs), `preview/props.png` and
   `preview/screen_mock.png` (a 296x240 screen, the cat at x3).

## Characters

| char | slot | meaning | follows the fur? |
|---|---|---|---|
| `.` | 0 | transparent | |
| `O` | 1 | outline (light on the black cat) | yes |
| `F` `S` | 2, 3 | fur / fur shade | yes |
| `T` | 4 | tabby stripe (= fur on white and calico, so the stripes vanish) | yes |
| `P` `Q` | 5, 6 | calico orange / black patches (= fur on the others) | yes |
| `L` | 7 | belly, muzzle, paws | yes |
| `E` | 8 | eyes (amber on black, blue on white, green on gray) | yes |
| `W` `K` `D` | 9-11 | white, pink (ears, nose, cheeks, tongue), dark (pupils, prop outline) | **fixed** |
| `B` `R` `Y` `N` | 12-15 | blue, red, yellow, brown (objects) | **fixed** |

Props and overlays may only use the fixed characters (`W K D B R Y N`): they are drawn with the palette of the cat on
screen, and the script refuses fur characters in props.

## Tips

- Put `T` (tabby stripes) and `P`/`Q` (calico patches) in **different places** so every fur looks right.
- Faces use `O` for the mouth and closed eyes (the black cat has a light outline, so they still show); pupils use `D`.
- Colours are in `PALETTES` (hex); the C output converts them to RGB565.
