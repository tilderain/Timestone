#!/usr/bin/env python3
"""Create synthetic TSARES2 fixtures, with no real player inputs or names."""
import pathlib, struct, sys
out=pathlib.Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
config=struct.pack('<18i10B2i2B',25,39,43,27,35,50,3,1,4,4,3,3,0,0,0,2,1,0,0,0,1,0,1,1,1,1,1,1,105,99,0,0)
assert len(config)==92
for i,frames in enumerate((6571,6066),1):
 h=struct.pack('<8s6IQ2I',b'TSARES2\0',2,140,i,0,0,0,i,8,0)+config
 assert len(h)==140
 body=b''.join(struct.pack('<II',j%32,(j*3)%32) for j in range(frames))
 (out/f'fixture{i}.tsr').write_bytes(h+body)
