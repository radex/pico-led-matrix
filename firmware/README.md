**add graphics**

convert video

```
ffmpeg -i badapple.webm -ss 00:01:00 -t 30 -vf "fps=30,scale=20:20:force_original_aspect_ratio=increase,crop=20:20,format=gray" badapple-frames/badapple_%04d.png
```

move to `gfx` folder, then:

```
python3 scripts/gfx_convert.py
```
