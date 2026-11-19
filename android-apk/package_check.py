import struct
import zipfile
from pathlib import Path

LIB='lib/arm64-v8a/libmadobs_preview.so'
def elf(data):
    if len(data)<64 or data[:7]!=b'\x7fELF\x02\x01\x01':raise ValueError('ELF')
    if struct.unpack_from('<HH',data,16)!=(3,183):raise ValueError('target')
    off=struct.unpack_from('<Q',data,32)[0];size,count=struct.unpack_from('<HH',data,54)
    if off<64 or size!=56 or not 0<count<1024 or off+size*count>len(data):raise ValueError('headers')
    loads=0
    for i in range(count):
        typ,flags,pos,va,pa,filesz,memsz,align=struct.unpack_from('<IIQQQQQQ',data,off+i*size)
        if pos+filesz>len(data):raise ValueError('range')
        if typ==3:raise ValueError('shared library must not have interpreter')
        if typ==1:
            loads+=1
            if filesz>memsz or align<16384 or align&(align-1) or pos%align!=va%align:raise ValueError('alignment')
    if not loads:raise ValueError('LOAD')

def apk(path,expected):
    data=path.read_bytes()
    with zipfile.ZipFile(path) as z:
        entries=z.infolist();names=[e.filename for e in entries]
        if len(names)!=len(set(names)) or LIB not in names or 'AndroidManifest.xml' not in names:raise ValueError('entries')
        if any(n.endswith('.dex') or n.startswith('META-INF/') or (n.startswith('lib/') and n!=LIB) for n in names):raise ValueError('unexpected payload')
        entry=z.getinfo(LIB)
        if entry.compress_type!=zipfile.ZIP_STORED or entry.flag_bits&1:raise ValueError('stored library required')
        off=entry.header_offset
        if data[off:off+4]!=b'PK\x03\x04':raise ValueError('local header')
        n,x=struct.unpack_from('<HH',data,off+26)
        if (off+30+n+x)%16384:raise ValueError('ZIP alignment')
        payload=z.read(entry)
        if payload!=expected:raise ValueError('library mismatch')
        elf(payload)
