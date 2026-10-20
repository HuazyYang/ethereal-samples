import ctypes as c
from pathlib import Path
import numpy as np
import json
import argparse
import os

parser=argparse.ArgumentParser(description='Compare retained CUDA and portable exact-byte compositing')
parser.add_argument('--ptx',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
options=parser.parse_args()
cuda=c.WinDLL('nvcuda.dll') if os.name=='nt' else c.CDLL('libcuda.so.1')
def api(name,args):
    fn=getattr(cuda,name)
    fn.argtypes=args
    fn.restype=c.c_int
    def call(*values):
        result=fn(*values)
        if result: raise RuntimeError(f'{name}: CUDA error {result}')
    return call
ptr=c.c_void_p
init=api('cuInit',[c.c_uint]); init(0)
retain=api('cuDevicePrimaryCtxRetain',[c.POINTER(ptr),c.c_int])
context=ptr(); retain(c.byref(context),0)
api('cuCtxSetCurrent',[ptr])(context)
module=ptr()
api('cuModuleLoad',[c.POINTER(ptr),c.c_char_p])(c.byref(module),str(options.ptx.resolve()).encode())
function=ptr(); api('cuModuleGetFunction',[c.POINTER(ptr),ptr,c.c_char_p])(c.byref(function),module,b'gvdbCompositeOver')
count=256**3
i=np.arange(count,dtype=np.uint32)
s=i&255; d=(i>>8)&255; a=i>>16
source=s|((255-s)<<8)|(s<<16)|(a<<24)
destination=d|(d<<8)|((255-d)<<16)|(d<<24)
deviceSource=c.c_uint64(); deviceDestination=c.c_uint64()
allocate=api('cuMemAlloc_v2',[c.POINTER(c.c_uint64),c.c_size_t])
allocate(c.byref(deviceSource),source.nbytes); allocate(c.byref(deviceDestination),destination.nbytes)
upload=api('cuMemcpyHtoD_v2',[c.c_uint64,ptr,c.c_size_t])
upload(deviceSource,source.ctypes.data,source.nbytes); upload(deviceDestination,destination.ctypes.data,destination.nbytes)
n=c.c_uint(count)
args=(ptr*3)(c.addressof(deviceDestination),c.addressof(deviceSource),c.addressof(n))
api('cuLaunchKernel',[ptr,c.c_uint,c.c_uint,c.c_uint,c.c_uint,c.c_uint,c.c_uint,c.c_uint,ptr,c.POINTER(ptr),ptr])(
    function,count//256,1,1,256,1,1,0,None,args,None)
api('cuCtxSynchronize',[])()
result=np.empty(count,np.uint32)
api('cuMemcpyDtoH_v2',[ptr,c.c_uint64,c.c_size_t])(result.ctypes.data,deviceDestination,result.nbytes)
expected=np.zeros(count,np.uint32)
for channel in range(4):
    numerator=((source>>(channel*8))&255)*255+((destination>>(channel*8))&255)*(255-a)
    expected|=np.minimum(255,numerator//255)<<(channel*8)
diff=np.abs(result.view(np.uint8).astype(np.int16)-expected.view(np.uint8).astype(np.int16))
report={'pixels':count,'changedPixels':int(np.count_nonzero(result!=expected)),
    'changedChannelBytes':int(np.count_nonzero(diff)),'maxChannelDifference':int(diff.max()),
    'meanAbsoluteChannelDifference':float(diff.mean()),'legacyPtx':str(options.ptx)}
options.output.write_text(json.dumps(report,indent=2))
print(report)
assert report['maxChannelDifference']<=1
free=api('cuMemFree_v2',[c.c_uint64]); free(deviceSource); free(deviceDestination)
api('cuModuleUnload',[ptr])(module)
api('cuDevicePrimaryCtxRelease_v2',[c.c_int])(0)
