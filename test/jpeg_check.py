import subprocess, numpy as np, math
from PIL import Image
rng = np.random.default_rng(3)
def to565(rgb):
    r,g,b = [rgb[...,i].astype(np.uint16) for i in range(3)]
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
def from565(v):
    r=(v>>11)&31; g=(v>>5)&63; b=v&31
    return np.stack([(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)],-1).astype(np.float64)
def psnr(a,b):
    m=((a-b)**2).mean(); return 99.0 if m==0 else 10*math.log10(255*255/m)
def photo(w,h):
    x,y=np.meshgrid(np.arange(w),np.arange(h))
    a=np.stack([128+100*np.sin(x/23.0)*np.cos(y/31.0),128+100*np.sin((x+y)/41.0),128+90*np.cos(x/17.0+y/53.0)],-1)
    a+=rng.normal(0,6,a.shape); a[h//3:h//3+9, w//4:w//4+40]=[250,20,20]; a[h//2:h//2+3,:]=[10,10,240]
    return np.clip(a,0,255).astype(np.uint8)
ok_all=True
def dec(name, im, sh=0, **kw):
    global ok_all
    p=f'/tmp/j_{name}.jpg'; im.save(p, **kw)
    r=subprocess.run(['/tmp/t_jpeg','dec',p,'/tmp/j_out.raw',str(sh)],capture_output=True,text=True)
    if r.returncode: print('FAIL',name,r.stdout.strip()); ok_all=False; return
    ref=Image.open(p); ref.load()
    w,h=ref.size; ow,oh=(w+(1<<sh)-1)>>sh,(h+(1<<sh)-1)>>sh
    got=from565(np.fromfile('/tmp/j_out.raw',dtype=np.uint16).reshape(oh,ow))
    refrgb=np.array(ref.convert('RGB')).astype(np.float64)
    if sh:
        f=1<<sh; ph=(-h)%f; pw=(-w)%f
        refrgb=np.pad(refrgb,((0,ph),(0,pw),(0,0)),mode='edge')
        refrgb=refrgb.reshape(oh,f,ow,f,3).mean((1,3))
    ps=psnr(got,refrgb)
    sub = ref.layer if False else None
    exact = None
    if sh==0 and kw.get('subsampling') in (1,2):
        pix = 'yuvj422p' if kw['subsampling']==1 else 'yuvj420p'
        raw = subprocess.run(['ffmpeg','-v','error','-i',p,'-pix_fmt',pix,'-f','rawvideo','-'],capture_output=True).stdout
        a = np.frombuffer(raw,dtype=np.uint8); cw=(w+1)//2; ch=(h+1)//2 if kw['subsampling']==2 else h
        Y=a[:w*h].reshape(h,w).astype(np.int32); U=a[w*h:w*h+cw*ch].reshape(ch,cw).astype(np.int32); V=a[w*h+cw*ch:].reshape(ch,cw).astype(np.int32)
        ys,xs=np.mgrid[0:h,0:w]; cy=ys>>1 if kw['subsampling']==2 else ys
        cb=U[cy,xs>>1]-128; cr=V[cy,xs>>1]-128
        rr=Y+((91881*cr+32768)>>16); gg=Y-((22554*cb+46802*cr+32768)>>16); bb=Y+((116130*cb+32768)>>16)
        e565 = to565(np.clip(np.stack([rr,gg,bb],-1),0,255).astype(np.uint8))
        g565 = np.fromfile('/tmp/j_out.raw',dtype=np.uint16).reshape(h,w)
        cd = lambda v: np.stack([(v>>11)&31,(v>>5)&63,v&31],-1).astype(int)
        exact = np.abs(cd(g565)-cd(e565)).max()
    thr = 36 if sh==0 else 25
    ok = (exact is not None and exact<=1) or (exact is None and (ps>thr or w*h<16))   # 1-pixel images are dominated by RGB565 quantisation
    if exact is not None: ps_note = f'exact-vs-nearest maxdiff {exact}'
    ok_all &= ok
    print('ok  ' if ok else 'FAIL', f'{name:22s} {w}x{h} scale1/{1<<sh}  PSNR vs libjpeg {ps:5.1f} dB', (f'| max 565-channel diff vs exact nearest-upsample ref = {exact}' if exact is not None else ''))
base=Image.fromarray(photo(320,240),'RGB')
dec('444_q90',base,subsampling=0,quality=90)
dec('420_q85',base,subsampling=2,quality=85)
dec('422_q85',base,subsampling=1,quality=85)
dec('gray',base.convert('L'),quality=88)
dec('420_q30',base,subsampling=2,quality=30)
dec('420_opt',base,subsampling=2,quality=80,optimize=True)
dec('odd_333x177',Image.fromarray(photo(333,177),'RGB'),subsampling=2,quality=85)
dec('tiny_5x3',Image.fromarray(photo(5,3),'RGB'),subsampling=2,quality=85)
dec('1x1',Image.fromarray(photo(1,1),'RGB'),quality=85)
try: dec('restart',base,subsampling=2,quality=85,restart_marker_blocks=7)
except TypeError: print('(PIL lacks restart option)')
for sh in (1,2,3): dec('420_scale',base,sh=sh,subsampling=2,quality=90)
dec('444_scale2',base,sh=2,subsampling=0,quality=90)
subprocess.run(['ffmpeg','-v','error','-y','-f','lavfi','-i','testsrc=size=176x144:rate=1:duration=1','-frames:v','1','-c:v','mjpeg','-q:v','4','/tmp/j_ffodd.jpg'])
r=subprocess.run(['/tmp/t_jpeg','dec','/tmp/j_ffodd.jpg','/tmp/j_out.raw','0'],capture_output=True,text=True)
ref=np.array(Image.open('/tmp/j_ffodd.jpg').convert('RGB')).astype(np.float64); got=from565(np.fromfile('/tmp/j_out.raw',dtype=np.uint16).reshape(144,176))
okf = r.returncode==0 and psnr(got,ref)>36; ok_all&=okf; print('ok  ' if okf else 'FAIL','ffmpeg 1x2-sampled MJPEG frame     176x144  PSNR vs libjpeg',round(psnr(got,ref),1),'dB')
# progressive must be rejected, not misdecoded
base.save('/tmp/j_prog.jpg',progressive=True,quality=85)
r=subprocess.run(['/tmp/t_jpeg','dec','/tmp/j_prog.jpg','/tmp/x.raw','0'],capture_output=True,text=True); print('progressive ->',r.stdout.strip(), '(expected open err -3)'); ok_all &= 'err -3' in r.stdout
# fuzz: corrupted files must never crash
d=open('/tmp/j_420_q85.jpg','rb').read(); import random; random.seed(2); crashes=0
for k in range(300):
    b=bytearray(d)
    for _ in range(6): b[random.randrange(2,len(b))]=random.randrange(256)
    open('/tmp/j_fuzz.jpg','wb').write(b)
    rr=subprocess.run(['/tmp/t_jpeg','dec','/tmp/j_fuzz.jpg','/tmp/x.raw',str(random.randrange(4))],capture_output=True,text=True)
    if rr.returncode not in (0,2,3): crashes+=1
for cut in (10,200,len(d)//3,len(d)-3):
    open('/tmp/j_fuzz.jpg','wb').write(d[:cut]); rr=subprocess.run(['/tmp/t_jpeg','dec','/tmp/j_fuzz.jpg','/tmp/x.raw','0'],capture_output=True,text=True)
    if rr.returncode not in (0,2,3): crashes+=1
print('fuzz: 304 corrupted/truncated JPEGs, crashes:',crashes); ok_all &= crashes==0
print('DECODE:', 'all ok' if ok_all else 'FAILURES')

# ---- encoder: decode our output with PIL AND ffmpeg
ok2=True
for (w,h,sub,q) in [(320,240,3,85),(320,240,1,90),(320,240,0,85),(333,177,3,75),(17,9,3,85),(1,1,1,85),(750,64,3,70),(64,64,3,10),(64,64,3,100)]:
    im=photo(w,h); v=to565(im); v.tofile('/tmp/e_in.raw')
    r=subprocess.run(['/tmp/t_jpeg','enc',str(w),str(h),'/tmp/e_in.raw','/tmp/e_out.jpg',str(q),str(sub)],capture_output=True,text=True)
    try:
        d=Image.open('/tmp/e_out.jpg'); d.load(); arr=np.array(d.convert('RGB')).astype(np.float64)
    except Exception as ex: print('FAIL PIL could not decode',w,h,sub,ex); ok2=False; continue
    src=from565(v)
    if sub==0: src=np.repeat(np.array(Image.fromarray(src.astype(np.uint8)).convert('L'))[...,None],3,-1).astype(np.float64)
    ps=psnr(arr,src)
    # reference: PIL's own encoder at same settings
    Image.fromarray(src.astype(np.uint8)).save('/tmp/e_ref.jpg',quality=q,subsampling={3:2,1:0,0:0}[sub])
    ref_sz=len(open('/tmp/e_ref.jpg','rb').read())
    sz=len(open('/tmp/e_out.jpg','rb').read())
    rc=subprocess.run(['ffmpeg','-v','error','-i','/tmp/e_out.jpg','-f','null','-'],capture_output=True,text=True)
    Image.fromarray(src.astype(np.uint8)).save('/tmp/e_ref2.jpg',quality=q,subsampling={3:2,1:0,0:0}[sub])
    refps = psnr(np.array(Image.open('/tmp/e_ref2.jpg').convert('RGB')).astype(np.float64), src)
    good = (ps >= refps-0.3 or w*h<16) and not rc.stderr.strip() and sz <= int(ref_sz*1.05)+64
    ok2&=good
    print('ok  ' if good else 'FAIL', f'enc {w}x{h} sub={sub} q={q}: PSNR {ps:5.2f} dB (libjpeg {refps:5.2f}), {sz} B (libjpeg {ref_sz} B), ffmpeg {"clean" if not rc.stderr.strip() else rc.stderr.strip()[:60]}')
print('ENCODE:', 'all ok' if ok2 else 'FAILURES')
