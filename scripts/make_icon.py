#!/usr/bin/env python3
"""Original vector geometry rasterized at each ICO size, no source-image dependency."""
from PIL import Image, ImageDraw
from pathlib import Path
root=Path(__file__).resolve().parents[1]
images=[]
for size in [16,20,24,32,40,48,64,128,256]:
    s=4; im=Image.new('RGBA',(size*s,size*s),(0,0,0,0));d=ImageDraw.Draw(im);k=size*s
    d.rounded_rectangle((0,0,k-1,k-1),radius=k*.25,fill=(36,92,218))
    d.ellipse((k*.18,k*.18,k*.81,k*.81),fill=(255,255,255))
    d.ellipse((k*.40,k*.10,k*.94,k*.64),fill=(36,92,218))
    d.rounded_rectangle((k*.48,k*.23,k*.57,k*.53),radius=k*.04,fill=(255,255,255))
    im=im.resize((size,size),Image.Resampling.LANCZOS);images.append(im)
images[-1].save(root/'resources'/'quietguard.ico',format='ICO',sizes=[x.size for x in images],append_images=images[:-1])
