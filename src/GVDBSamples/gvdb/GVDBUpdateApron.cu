#include <gvdb/GVDB.cuh>

#define CMAX(x,y,z)  (imax3(x,y,z)-imin3(x,y,z))

#define RGBA2INT(r,g,b,a)	( (uint((a)*255.0f)<<24) | (uint((b)*255.0f)<<16) | (uint((g)*255.0f)<<8) | uint((r)*255.0f) )
#define CLR2INT(c)			( (uint((c.w)*255.0f)<<24) | (uint((c.z)*255.0f)<<16) | (uint((c.y)*255.0f)<<8) | uint((c.x)*255.0f) )
#define INT2CLR(c)			( make_float4( float(c & 0xFF)/255.0f, float((c>>8) & 0xFF)/255.0f, float((c>>16) & 0xFF)/255.0f, float((c>>24) & 0xFF)/255.0f ))

#define T_UCHAR			0		// channel types
#define	T_FLOAT			3
#define T_INT			6

// Function template for updating the apron without UpdateApronFaces' neighbor
// table. Each of the voxels of the apron computes its world-space
// (= index-space) position and looks up what its value should be, sampling
// from a neighboring brick, or using the boundary value if no brick contains
// the voxel.
// 
// This should be called using blocks with an x dimension of 6, arranged in a
// grid along the x axis. Each yz plane of the block will fill in the voxels
// for a different face.
template<class T>
__device__ void UpdateApron(VDBInfo* gvdb, const uchar channel, const int brickCount, const int paddedBrickRes, const T boundaryValue)
{
	// The brick this block processes.
	const int brick = blockIdx.x;
	if (brick > brickCount) return;

	// Determine which voxel of the apron to compute and write.
	uint3 brickVoxel; // In the local coordinate space of the brick
	{
		const int side = threadIdx.x; // Side of the brick, from 0 to 5
		uint2 suv = make_uint2(blockIdx.y*blockDim.y + threadIdx.y, blockIdx.z*blockDim.z + threadIdx.z); // Position on the side of the brick
		if (suv.x >= paddedBrickRes || suv.y >= paddedBrickRes || side >= 6) return;

		switch (side) {
		case 0:		brickVoxel = make_uint3(0, suv.x, suv.y);			break;
		case 1:		brickVoxel = make_uint3(suv.x, 0, suv.y);			break;
		case 2:		brickVoxel = make_uint3(suv.x, suv.y, 0);			break;
		case 3:		brickVoxel = make_uint3(paddedBrickRes - 1, suv.x, suv.y);	break;
		case 4:		brickVoxel = make_uint3(suv.x, paddedBrickRes - 1, suv.y);	break;
		case 5:		brickVoxel = make_uint3(suv.x, suv.y, paddedBrickRes - 1);	break;
		}
	}

	// Compute the position of the voxel in the atlas
	uint3 atlasVoxel; // In the coordinate space of the entire atlas
	{
		VDBNode* node = getNode(gvdb, 0, brick);
		if (node == 0x0) return; // This brick ID didn't correspond to a known brick, which is invalid
		// (The (1,1,1) here accounts for the 1 unit of apron padding)
		atlasVoxel = brickVoxel + make_uint3(node->mValue) - make_uint3(1, 1, 1);
	}

	// Get the value of the voxel by converting to index-space and then
	// sampling the value at that index
	T value;
	{
		float3 worldPos;
		if (!getAtlasToWorld(gvdb, atlasVoxel, worldPos)) return;
		float3 offs, vmin; uint64 nodeID;
		VDBNode* node = getNodeAtPoint(gvdb, worldPos, &offs, &vmin, &nodeID);

		if (node == 0x0) {
			// Out of range, use the boundary value
			value = boundaryValue;
		}
		else {
			offs += (worldPos - vmin); // Get the atlas position
			value = surf3Dread<T>(gvdb->volOut[channel], uint(offs.x) * sizeof(T), uint(offs.y), uint(offs.z));
		}
	}

	// Write to the apron voxel
	surf3Dwrite(value, gvdb->volOut[channel], atlasVoxel.x * sizeof(T), atlasVoxel.y, atlasVoxel.z);
}

extern "C" __global__ void gvdbUpdateApronF (VDBInfo* gvdb, uchar chan, int brickcnt, int brickres, int brickwid, float boundval)
{
	UpdateApron<float>(gvdb, chan, brickcnt, brickres, boundval);
}

extern "C" __global__ void gvdbUpdateApronF4 ( VDBInfo* gvdb, uchar chan, int brickcnt, int brickres, int brickwid, float boundval)
{
	UpdateApron<float4>(gvdb, chan, brickcnt, brickres, make_float4(boundval, boundval, boundval, boundval));
}

extern "C" __global__ void gvdbUpdateApronC ( VDBInfo* gvdb, uchar chan, int brickcnt, int brickres, int brickwid, float boundval)
{
	UpdateApron<uchar>(gvdb, chan, brickcnt, brickres, boundval);
}

extern "C" __global__ void gvdbUpdateApronC4 ( VDBInfo* gvdb, uchar chan, int brickcnt, int brickres, int brickwid, float boundval)
{
	UpdateApron<uchar4>(gvdb, chan, brickcnt, brickres, make_uchar4(boundval, boundval, boundval, boundval));
}

