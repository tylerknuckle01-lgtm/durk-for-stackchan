import numpy as np, os
from PIL import Image, ImageDraw
rng=np.random.default_rng(7); W,H=320,240
y,x=np.mgrid[0:H,0:W].astype(np.float32)
def noise(scale):
    acc=np.zeros((H,W),np.float32); amp=1
    for o in range(5):
        s=max(2,int(scale/(2**o)))
        g=rng.random((H//s+3,W//s+3))
        im=Image.fromarray((g*255).astype(np.uint8)).resize(((W//s+3)*s,(H//s+3)*s),Image.BICUBIC)
        acc+=np.array(im,np.float32)[:H,:W]/255*amp; amp*=0.5
    return acc/acc.max()
n1,n2=noise(80),noise(60)
band=np.exp(-((y-0.55*x-40)/70)**2)
neb1=np.clip(n1*band*1.6-0.35,0,1)
neb2=np.clip(n2*np.exp(-((x-250)**2+(y-60)**2)/6000)*1.8-0.3,0,1)
img=np.zeros((H,W,3),np.float32)+[4,4,14]
img+=neb1[...,None]*[90,40,140]
img+=neb2[...,None]*[30,90,150]
img+=(neb1*neb2)[...,None]*[120,60,80]
mask=np.exp(-((y-120)/60)**2)*np.exp(-((x-160)/120)**2)
img*=(1-0.55*mask)[...,None]
for _ in range(220):
    sx,sy=rng.integers(0,W),rng.integers(0,H); b=rng.uniform(80,255)
    img[sy,sx]=np.maximum(img[sy,sx],[b,b,min(255,b*1.05)])
for _ in range(12):
    sx,sy=rng.integers(1,W-1),rng.integers(1,H-1); img[sy,sx]=255
    for dx,dy in((1,0),(-1,0),(0,1),(0,-1)): img[sy+dy,sx+dx]=np.maximum(img[sy+dy,sx+dx],120)
img=np.clip(img,0,255); d=rng.random((H,W,3))
r=np.clip(np.floor(img[...,0]/255*31+d[...,0]),0,31).astype(np.uint16)
g=np.clip(np.floor(img[...,1]/255*63+d[...,1]),0,63).astype(np.uint16)
b=np.clip(np.floor(img[...,2]/255*31+d[...,2]),0,31).astype(np.uint16)
((r<<11)|(g<<5)|b).astype('<u2').tofile('galaxy.bin')
im=Image.fromarray(np.stack([r*255//31,g*255//63,b*255//31],-1).astype(np.uint8)); dr=ImageDraw.Draw(im)
for cx,cy in((90,93),(230,96)): dr.ellipse([cx-8,cy-8,cx+8,cy+8],fill='white')
dr.rectangle([138,146,188,150],fill='white'); im.resize((640,480),Image.NEAREST).save('preview.png')
open('galaxy.json','w').write('{"name":"galaxy","fg":"#FFFFFF","bg":"#04040E","bg_image":"galaxy.bin","stars":20,"twinkle":true}\n')
print(os.path.getsize('galaxy.bin'))
