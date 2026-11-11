#!/usr/bin/env python3
"""Render original vector/code branding. No Amazon artwork is bundled."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / 'assets/launcher/fonts/Inter-SemiBold.ttf'
BG = '#0b0e1a'
PURPLE = '#b49aff'
WHITE = '#f4f3fb'


def render(name, width, height, square=False):
    scale = 3
    image = Image.new('RGB', (width*scale,height*scale), BG)
    d = ImageDraw.Draw(image)
    if square:
        x,y,r = width*.5,height*.37,height*.25
        tx,ty,size = 38,174,52
        subtitle = ''
    else:
        x,y,r = height*.52,height*.5,height*.29
        tx,ty,size = height*.98,height*.19,int(height*.36)
        subtitle = 'Cloud gaming. On Switch.' if width > 300 else ''
        if width <= 300: tx,ty,size = 85,52,37
    def circle(cx,cy,radius,color):
        d.ellipse(tuple(v*scale for v in (cx-radius,cy-radius,cx+radius,cy+radius)),fill=color)
    circle(x,y,r,PURPLE)
    circle(x+r*.40,y-r*.25,r*.88,BG)
    circle(x+r*.93,y-r*.77,r*.10,WHITE)
    font = ImageFont.truetype(str(FONT),int(size*scale))
    d.text((tx*scale,ty*scale),'LunaNX',font=font,fill=WHITE)
    if subtitle:
        d.text((tx*scale,height*.64*scale),subtitle,
               font=ImageFont.truetype(str(FONT),int(height*.115*scale)),fill=PURPLE)
    image=image.resize((width,height),Image.Resampling.LANCZOS)
    output=ROOT/'assets/branding';output.mkdir(parents=True,exist_ok=True)
    image.save(output/(name+'.png'))
    if square: image.save(output/'icon.jpg',quality=95,subsampling=0)
    text=f'<text x="{tx}" y="{ty+size}" fill="{WHITE}" font-family="Inter, sans-serif" font-weight="600" font-size="{size}">LunaNX</text>'
    if subtitle:
        text+=f'<text x="{tx}" y="{height*.755}" fill="{PURPLE}" font-family="Inter, sans-serif" font-size="{height*.115}">{subtitle}</text>'
    svg=f'''<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">
<rect width="{width}" height="{height}" fill="{BG}"/>
<circle cx="{x}" cy="{y}" r="{r}" fill="{PURPLE}"/>
<circle cx="{x+r*.40}" cy="{y-r*.25}" r="{r*.88}" fill="{BG}"/>
<circle cx="{x+r*.93}" cy="{y-r*.77}" r="{r*.10}" fill="{WHITE}"/>
{text}</svg>'''
    (output/(name+'.svg')).write_text(svg+'\n')


if __name__=='__main__':
    render('icon',256,256,True)
    render('store-icon',256,150)
    render('banner',848,208)
    print('Rendered NRO icon (256x256 JPEG), store icon (256x150 PNG), banner (848x208 PNG) and editable SVG sources.')
