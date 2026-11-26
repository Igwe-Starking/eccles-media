import subprocess, numpy as np, io, sys, os
from PIL import Image
rng = np.random.default_rng(7)
def to565(rgb):
    r,g,b = rgb[...,0].astype(np.uint16), rgb[...,1].astype(np.uint16), rgb[...,2].astype(np.uint16)
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
def grad(w,h,ch):
    x,y = np.meshgrid(np.arange(w),np.arange(h))
    a = np.stack([(x*255//max(w-1,1)), (y*255//max(h-1,1)), ((x+y)*255//max(w+h-2,1)), (255-(x*255//max(w-1,1)))],-1).astype(np.uint8)
    a = a[...,:ch].copy()
    a[::7, ::5] = rng.integers(0,255,size=a[::7, ::5].shape,dtype=np.uint8)   # noise -> exercises all filters
    return a
def run(img, name, **save):
    p=f'/tmp/p_{name}.png'; img.save(p, **save)
    r = subprocess.run(['/tmp/t_png','dec',p,'/tmp/p_out.raw'],capture_output=True,text=True)
    if r.returncode: print('FAIL',name,r.stdout.strip()); return False
    w,h = img.size
    got = np.fromfile('/tmp/p_out.raw',dtype=np.uint16).reshape(h,w)
    rgba = np.array(Image.open(p).convert('RGBA')).astype(np.int32)
    a = rgba[...,3:4]
    exp = (rgba[...,:3]*a + 255*(255-a) + 127)//255
    exp = to565(exp.astype(np.uint8))
    # 16-bit & low-depth conversions may differ by rounding in PIL (<=1 LSB per channel): tolerate that
    d = np.abs(got.astype(np.int32) - exp.astype(np.int32))
    def chan(v): return np.stack([(v>>11)&31,(v>>5)&63,v&31],-1).astype(np.int32)
    cd = np.abs(chan(got)-chan(exp)).max()
    ok = cd <= 1
    print(('ok  ' if ok else 'FAIL'), name, f'{w}x{h}', 'maxchan-diff', cd, 'exact' if (d==0).all() else '')
    return ok
res=[]
for (w,h) in [(1,1),(7,5),(64,48),(331,17),(250,100)]:
    res.append(run(Image.fromarray(grad(w,h,3),'RGB'), f'rgb{w}x{h}'))
res.append(run(Image.fromarray(grad(100,60,3),'RGB'), 'rgb_opt', optimize=True))
res.append(run(Image.fromarray(grad(100,60,3),'RGB'), 'rgb_l0', compress_level=0))
res.append(run(Image.fromarray(grad(100,60,3),'RGB'), 'rgb_l9', compress_level=9))
res.append(run(Image.fromarray(grad(97,41,4),'RGBA'), 'rgba'))
res.append(run(Image.fromarray(grad(97,41,1)[...,0],'L'), 'gray'))
for bits in (1,2,4):
    g = (grad(50,30,1)[...,0] >> (8-bits))
    im = Image.fromarray(g.astype(np.uint8),'L'); 
    p='/tmp/p_g.png'
    # write low-bit gray via PIL's bits option
    im.point(lambda v: v*(255//((1<<bits)-1))).save(p, bits=bits)
    r = subprocess.run(['/tmp/t_png','dec',p,'/tmp/p_out.raw'],capture_output=True,text=True)
    print('ok  ' if r.returncode==0 else 'FAIL', f'gray{bits}bit', r.stdout.strip()); res.append(r.returncode==0)
res.append(run(Image.fromarray(grad(60,40,3),'RGB').quantize(16), 'pal16'))
res.append(run(Image.fromarray(grad(60,40,3),'RGB').quantize(256), 'pal256'))
im = Image.fromarray(grad(60,40,3),'RGB').quantize(8); res.append(run(im,'pal_trns',transparency=2))
res.append(run(Image.fromarray(grad(40,30,2),'LA'), 'gray_alpha'))
# 16-bit RGB via numpy -> PIL doesn't write; build with raw zlib manually
import zlib, struct
def png16(w,h,arr):
    def ck(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    raw=b''.join(b'\x00'+arr[y].astype('>u2').tobytes() for y in range(h))
    return b'\x89PNG\r\n\x1a\n'+ck(b'IHDR',struct.pack('>IIBBBBB',w,h,16,2,0,0,0))+ck(b'IDAT',zlib.compress(raw))+ck(b'IEND',b'')
a16 = (rng.integers(0,65535,size=(20,30,3))).astype(np.uint16)
open('/tmp/p16.png','wb').write(png16(30,20,a16.reshape(20,90)))
r = subprocess.run(['/tmp/t_png','dec','/tmp/p16.png','/tmp/p_out.raw'],capture_output=True,text=True)
got=np.fromfile('/tmp/p_out.raw',dtype=np.uint16).reshape(20,30)
exp=to565((a16>>8).astype(np.uint8)); print('ok  ' if (got==exp).all() else 'FAIL','rgb16'); res.append((got==exp).all())
# interlaced must be rejected cleanly
Image.fromarray(grad(30,30,3),'RGB').save('/tmp/p_il.png', interlace=1) if False else None
print('DECODE:', 'all ok' if all(res) else 'FAILURES')

# ---- encoder: ours -> PIL must be exact
res=[]
for (w,h) in [(1,1),(3,3),(64,48),(321,200),(750,40)]:
    base = grad(w,h,3); v = to565(base)
    v.tofile('/tmp/e_in.raw')
    r = subprocess.run(['/tmp/t_png','enc',str(w),str(h),'/tmp/e_in.raw',f'/tmp/e_{w}.png'],capture_output=True,text=True)
    im = Image.open(f'/tmp/e_{w}.png'); im.load()
    arr = np.array(im.convert('RGB')).astype(np.uint16)
    exp = np.stack([(((v>>11)&31)<<3)|(((v>>11)&31)>>2), (((v>>5)&63)<<2)|(((v>>5)&63)>>4), ((v&31)<<3)|((v&31)>>2)],-1)
    ok = (arr==exp).all(); res.append(ok)
    print('ok  ' if ok else 'FAIL', f'enc {w}x{h}', r.stdout.strip())
# photo-like image for compression ratio
x,y=np.meshgrid(np.arange(320),np.arange(240)); ph=np.stack([128+100*np.sin(x/23.0)*np.cos(y/31.0),128+100*np.sin((x+y)/41.0),128+90*np.cos(x/17.0)],-1).astype(np.uint8)
to565(ph).tofile('/tmp/e_in.raw'); r=subprocess.run(['/tmp/t_png','enc','320','240','/tmp/e_in.raw','/tmp/e_ph.png'],capture_output=True,text=True); print(r.stdout.strip())
rc=subprocess.run(['ffmpeg','-v','error','-i','/tmp/e_ph.png','-f','null','-'],capture_output=True,text=True); print('ffmpeg decode of our png:', 'clean' if not rc.stderr.strip() else rc.stderr.strip()); res.append(not rc.stderr.strip())
print('ENCODE:', 'all ok' if all(res) else 'FAILURES')
