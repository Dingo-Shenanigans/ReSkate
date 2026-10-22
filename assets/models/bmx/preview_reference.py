"""Offline geometry QA only. This is not a game screenshot."""
from pathlib import Path
import math
import struct
import sys
root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / "build/bmx-unity-deps"))
from PIL import Image, ImageDraw

def sub(a,b): return tuple(x-y for x,y in zip(a,b))
def dot(a,b): return sum(x*y for x,y in zip(a,b))
def unit(a):
    length = math.sqrt(dot(a,a)) or 1
    return tuple(x/length for x in a)
def cross(a,b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
camera=(2.3,1.7,2.4); forward=unit(sub((0,.55,0),camera)); right=unit(cross(forward,(0,1,0))); up=cross(right,forward)
image=Image.new("RGB",(1000,760),(18,25,37)); draw=ImageDraw.Draw(image)
triangles=[]
def emit(points,part):
    color=(43,155,207) if part not in (2,3) else (49,54,61)
    normal=unit(cross(sub(points[1],points[0]),sub(points[2],points[0]))); shade=.38+.62*abs(dot(normal,unit((.4,.8,-.3))))
    screen=[]; depth=0
    for p in points:
        delta=sub(p,camera); z=dot(delta,forward); depth+=z
        screen.append((500+dot(delta,right)*950/z,445-dot(delta,up)*950/z))
    triangles.append((depth,screen,tuple(int(c*shade) for c in color)))
payload=(Path(__file__).parent/"reference/bike_geometry.bin").read_bytes()
magic,version,stride,count,*bounds=struct.unpack_from("<8sIII6f",payload)
assert magic==b"RSBMX001" and version==1 and stride==40 and len(payload)==44+count*40
for at in range(44,len(payload),stride):
    part,*values=struct.unpack_from("<I9f",payload,at)
    emit([tuple(values[i:i+3]) for i in (0,3,6)],part)
def tube(a,b,r,part):
    axis=unit(sub(b,a));u=unit(cross(axis,(0,1,0) if abs(axis[1])<.9 else (1,0,0)));v=cross(axis,u)
    for i in range(8):
        offsets=[tuple(r*(u[k]*math.cos(j*math.pi/4)+v[k]*math.sin(j*math.pi/4)) for k in range(3)) for j in (i,i+1)]
        p=[tuple(a[k]+o[k] for k in range(3)) for o in offsets];q=[tuple(b[k]+o[k] for k in range(3)) for o in offsets]
        emit((p[0],q[0],q[1]),part);emit((p[0],q[1],p[1]),part)
for z,part in ((-.525,2),(.525,3)):
    for i in range(24):
        a=i*math.pi/12;b=(i+1)*math.pi/12
        p=(0,.34+math.cos(a)*.275,z+math.sin(a)*.275);q=(0,.34+math.cos(b)*.275,z+math.sin(b)*.275)
        tube(p,q,.009,part)
        if i%2==0:tube((0,.34,z),p,.0025,part)
for a,b,r,part in (((0,.76,.36),(0,.86,.39),.018,1),((0,.55,-.22),(0,.76,-.22),.016,0),((0,.76,-.32),(0,.76,-.09),.055,0),
                   ((-.105,.35,-.12),(.105,.35,-.12),.023,4),((-.1,.35,-.12),(-.1,.35,.045),.014,4),((.1,.35,-.12),(.1,.35,-.285),.014,4),
                   ((-.15,.35,.045),(-.05,.35,.045),.025,4),((.05,.35,-.285),(.15,.35,-.285),.025,4)):
    tube(a,b,r,part)
for _,points,color in sorted(triangles,reverse=True): draw.polygon(points,fill=color)
draw.text((24,22),"LOCAL BMX STREETS GEOMETRY - OFFLINE ASSEMBLY QA",fill=(232,238,247))
draw.text((24,45),"Prototype placements + runtime supplemental geometry. Not a game screenshot.",fill=(160,182,201))
destination=Path(__file__).parent/"reference/assembly-preview.png";image.save(destination);print(destination)
