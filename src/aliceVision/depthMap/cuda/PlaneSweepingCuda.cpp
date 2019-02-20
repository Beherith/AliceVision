// This file is part of the AliceVision project.
// Copyright (c) 2017 AliceVision contributors.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "PlaneSweepingCuda.hpp"
#include <aliceVision/system/Logger.hpp>
#include <aliceVision/mvsData/Matrix3x3.hpp>
#include <aliceVision/mvsData/Matrix3x4.hpp>
#include <aliceVision/mvsData/OrientedPoint.hpp>
#include <aliceVision/mvsUtils/common.hpp>
#include <aliceVision/mvsUtils/fileIO.hpp>
#include <aliceVision/depthMap/cuda/planeSweeping/plane_sweeping_cuda.hpp>
#include <aliceVision/depthMap/cuda/planeSweeping/host_utils.h>

#include <iostream>
#include <sstream>
#include <stdexcept>

namespace aliceVision {
namespace depthMap {

inline const uchar4 get(mvsUtils::ImagesCache::ImgPtr img, int x, int y)
{
    const Color floatRGB = img->at(x,y) * 255.0f;

    return make_uchar4( static_cast<unsigned char>(floatRGB.r),
                        static_cast<unsigned char>(floatRGB.g),
                        static_cast<unsigned char>(floatRGB.b),
                        1 );
}


static void cps_fillCamera(cameraStructBase& base, int c, mvsUtils::MultiViewParams* mp, int scale, const char* called_from )
{

    Matrix3x3 scaleM;
    scaleM.m11 = 1.0 / (float)scale;
    scaleM.m12 = 0.0;
    scaleM.m13 = 0.0;
    scaleM.m21 = 0.0;
    scaleM.m22 = 1.0 / (float)scale;
    scaleM.m23 = 0.0;
    scaleM.m31 = 0.0;
    scaleM.m32 = 0.0;
    scaleM.m33 = 1.0;
    Matrix3x3 K = scaleM * mp->KArr[c];

    Matrix3x3 iK = K.inverse();
    Matrix3x4 P = K * (mp->RArr[c] | (Point3d(0.0, 0.0, 0.0) - mp->RArr[c] * mp->CArr[c]));
    Matrix3x3 iP = mp->iRArr[c] * iK;

    base.C.x = mp->CArr[c].x;
    base.C.y = mp->CArr[c].y;
    base.C.z = mp->CArr[c].z;

    base.P[0] = P.m11;
    base.P[1] = P.m21;
    base.P[2] = P.m31;
    base.P[3] = P.m12;
    base.P[4] = P.m22;
    base.P[5] = P.m32;
    base.P[6] = P.m13;
    base.P[7] = P.m23;
    base.P[8] = P.m33;
    base.P[9] = P.m14;
    base.P[10] = P.m24;
    base.P[11] = P.m34;

    base.iP[0] = iP.m11;
    base.iP[1] = iP.m21;
    base.iP[2] = iP.m31;
    base.iP[3] = iP.m12;
    base.iP[4] = iP.m22;
    base.iP[5] = iP.m32;
    base.iP[6] = iP.m13;
    base.iP[7] = iP.m23;
    base.iP[8] = iP.m33;

    base.R[0] = mp->RArr[c].m11;
    base.R[1] = mp->RArr[c].m21;
    base.R[2] = mp->RArr[c].m31;
    base.R[3] = mp->RArr[c].m12;
    base.R[4] = mp->RArr[c].m22;
    base.R[5] = mp->RArr[c].m32;
    base.R[6] = mp->RArr[c].m13;
    base.R[7] = mp->RArr[c].m23;
    base.R[8] = mp->RArr[c].m33;

    base.iR[0] = mp->iRArr[c].m11;
    base.iR[1] = mp->iRArr[c].m21;
    base.iR[2] = mp->iRArr[c].m31;
    base.iR[3] = mp->iRArr[c].m12;
    base.iR[4] = mp->iRArr[c].m22;
    base.iR[5] = mp->iRArr[c].m32;
    base.iR[6] = mp->iRArr[c].m13;
    base.iR[7] = mp->iRArr[c].m23;
    base.iR[8] = mp->iRArr[c].m33;

    base.K[0] = K.m11;
    base.K[1] = K.m21;
    base.K[2] = K.m31;
    base.K[3] = K.m12;
    base.K[4] = K.m22;
    base.K[5] = K.m32;
    base.K[6] = K.m13;
    base.K[7] = K.m23;
    base.K[8] = K.m33;

    base.iK[0] = iK.m11;
    base.iK[1] = iK.m21;
    base.iK[2] = iK.m31;
    base.iK[3] = iK.m12;
    base.iK[4] = iK.m22;
    base.iK[5] = iK.m32;
    base.iK[6] = iK.m13;
    base.iK[7] = iK.m23;
    base.iK[8] = iK.m33;

    ps_initCameraMatrix( base );
}

static void cps_fillCameraData(mvsUtils::ImagesCache& ic, cameraStruct& cam, int c, mvsUtils::MultiViewParams* mp, StaticVectorBool* rcSilhoueteMap)
{
    // memcpyGrayImageFromFileToArr(cam->tex_hmh->getBuffer(), mp->indexes[c], mp, true, 1, 0);
    // memcpyRGBImageFromFileToArr(
    //	cam->tex_hmh_r->getBuffer(),
    //	cam->tex_hmh_g->getBuffer(),
    //	cam->tex_hmh_b->getBuffer(), mp->indexes[c], mp, true, 1, 0);

    // ic.refreshData(c);
    mvsUtils::ImagesCache::ImgPtr img = ic.getImg_sync( c );

    Pixel pix;
    if( rcSilhoueteMap == nullptr )
    {
        for(pix.y = 0; pix.y < mp->getHeight(c); pix.y++)
        {
            for(pix.x = 0; pix.x < mp->getWidth(c); pix.x++)
            {
                uchar4& pix_rgba = (*cam.tex_rgba_hmh)(pix.x, pix.y);
                const uchar4 pc = get( img, pix.x, pix.y ); //  ic.getPixelValue(pix, c);
                pix_rgba = pc;
            }
        }
    }
    else
    {
        for(pix.y = 0; pix.y < mp->getHeight(c); pix.y++)
        {
            for(pix.x = 0; pix.x < mp->getWidth(c); pix.x++)
            {
                uchar4& pix_rgba = (*cam.tex_rgba_hmh)(pix.x, pix.y);
                uchar4 pc = get( img, pix.x, pix.y );
                if( (*rcSilhoueteMap)[pix.y*mp->getWidth(c)+pix.x] ) 
                {
                    pc.w = 0; // disabled if pix has background color
                }
                pix_rgba = pc;
            }
        }
    }
}

PlaneSweepingCuda::PlaneSweepingCuda( int CUDADeviceNo,
                                      mvsUtils::ImagesCache&     ic,
                                      mvsUtils::MultiViewParams* _mp,
                                      int scales )
    : _scales( scales )
    , _nbest( 1 ) // TODO remove nbest ... now must be 1
    , _CUDADeviceNo( CUDADeviceNo )
    , _verbose( _mp->verbose )
    , _nbestkernelSizeHalf( 1 )
    , _nImgsInGPUAtTime( 2 )
    , _ic( ic )
{
    ps_testCUDAdeviceNo( _CUDADeviceNo );

    cudaError_t err;

    /* The caller knows all camera that will become rc cameras, but it does not
     * pass that information to this function.
     * It knows the nearest cameras for each of those rc cameras, but it doesn't
     * pass that information, either.
     * So, the only task of this function is to allocate an amount of memory that
     * will hold CUDA memory for camera structs and bitmaps.
     */

    mp = _mp;

    const int maxImageWidth = mp->getMaxImageWidth();
    const int maxImageHeight = mp->getMaxImageHeight();

    float oneimagemb = 4.0f * (((float)(maxImageWidth * maxImageHeight) / 1024.0f) / 1024.0f);
    for(int scale = 2; scale <= _scales; ++scale)
    {
        oneimagemb += 4.0 * (((float)((maxImageWidth / scale) * (maxImageHeight / scale)) / 1024.0) / 1024.0);
    }
    float maxmbGPU = 100.0f;
    _nImgsInGPUAtTime = (int)(maxmbGPU / oneimagemb);
    _nImgsInGPUAtTime = std::max(2, std::min(mp->ncams, _nImgsInGPUAtTime));

    doVizualizePartialDepthMaps = mp->userParams.get<bool>("grow.visualizePartialDepthMaps", false);
    useRcDepthsOrRcTcDepths = mp->userParams.get<bool>("grow.useRcDepthsOrRcTcDepths", false);

    minSegSize = mp->userParams.get<int>("fuse.minSegSize", 100);
    varianceWSH = mp->userParams.get<int>("global.varianceWSH", 4);

    subPixel = mp->userParams.get<bool>("global.subPixel", true);

    ALICEVISION_LOG_INFO("PlaneSweepingCuda:" << std::endl
                         << "\t- _nImgsInGPUAtTime: " << _nImgsInGPUAtTime << std::endl
                         << "\t- scales: " << _scales << std::endl
                         << "\t- subPixel: " << (subPixel ? "Yes" : "No") << std::endl
                         << "\t- varianceWSH: " << varianceWSH);

    // allocate global on the device
    ps_deviceAllocate(ps_texs_arr, _nImgsInGPUAtTime, maxImageWidth, maxImageHeight, _scales, _CUDADeviceNo);

    _camsBasesDev.allocate( CudaSize<2>(1, _nImgsInGPUAtTime) );
    _camsBasesHst.allocate( CudaSize<2>(1, _nImgsInGPUAtTime) );
    cams     .resize(_nImgsInGPUAtTime);
    camsRcs  .resize(_nImgsInGPUAtTime);
    camsTimes.resize(_nImgsInGPUAtTime);

    for( int rc = 0; rc < _nImgsInGPUAtTime; ++rc )
    {
        cams[rc].param_hst = &_camsBasesHst(0,rc);
        cams[rc].param_dev = &_camsBasesDev(0,rc);

        err = cudaStreamCreate( &cams[rc].stream );
        if( err != cudaSuccess )
        {
            ALICEVISION_LOG_WARNING("Failed to create a CUDA stream object for async sweeping");
            cams[rc].stream = 0;
        }
    }

    for(int rc = 0; rc < _nImgsInGPUAtTime; ++rc)
    {
        cams[rc].camId = -1;
        camsRcs[rc]   = -1;
        camsTimes[rc] = 0;
    }
}

PlaneSweepingCuda::~PlaneSweepingCuda(void)
{
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // deallocate global on the device
    ps_deviceDeallocate(ps_texs_arr, _CUDADeviceNo, _nImgsInGPUAtTime, _scales);

    for(int c = 0; c < cams.size(); c++)
    {
        delete cams[c].tex_rgba_hmh;

        cudaStreamDestroy( cams[c].stream );
    }

    mp = NULL;
}

void PlaneSweepingCuda::cameraToDevice( int rc, const StaticVector<int>& tcams )
{
    std::ostringstream ostr;

    ostr << "Called " << __FUNCTION__ << " with cameras" << std::endl
         << "    rc = " << rc << std::endl;
    for( auto it : tcams )
    {
        ostr << "    " << it << std::endl;
    }

    ALICEVISION_LOG_DEBUG( ostr.str() );
}

int PlaneSweepingCuda::addCam( int rc, int scale,
                               StaticVectorBool* rcSilhoueteMap,
                               const char* calling_func )
{
    // fist is oldest
    int id = camsRcs.indexOf(rc);
    if(id == -1)
    {
        // get oldest id
        int oldestId = camsTimes.minValId();
        cameraStruct& cam = cams[oldestId];

        if(cam.tex_rgba_hmh == nullptr)
        {
            cam.tex_rgba_hmh =
                new CudaHostMemoryHeap<uchar4, 2>(CudaSize<2>(mp->getMaxImageWidth(), mp->getMaxImageHeight()));
        }
        long t1 = clock();

        cps_fillCamera( _camsBasesHst(0,oldestId), rc, mp, scale, calling_func );
        cps_fillCameraData( _ic, cams[oldestId], rc, mp, rcSilhoueteMap );
        ps_deviceUpdateCam( ps_texs_arr, cams[oldestId], oldestId,
                            _CUDADeviceNo, _nImgsInGPUAtTime, _scales, mp->getMaxImageWidth(), mp->getMaxImageHeight(), varianceWSH);

        if(_verbose)
            mvsUtils::printfElapsedTime(t1, "copy image from disk to GPU ");

        camsRcs[oldestId] = rc;
        camsTimes[oldestId] = clock();
        id = oldestId;
    }
    else
    {
        /*
         * Revisit this:
         * It is not sensible to waste cycles on refilling the camera struct if the new one
         * is identical to the old one.
         */
        cps_fillCamera( _camsBasesHst(0,id), rc, mp, scale, calling_func );
        // cps_fillCameraData((cameraStruct*)(*cams)[id], rc, mp, H, _scales);
        // ps_deviceUpdateCam((cameraStruct*)(*cams)[id], id, _scales);

        camsTimes[id] = clock();
    }
    return id;
}

void PlaneSweepingCuda::getMinMaxdepths(int rc, const StaticVector<int>& tcams, float& minDepth, float& midDepth,
                                          float& maxDepth)
{
  const bool minMaxDepthDontUseSeeds = mp->userParams.get<bool>("prematching.minMaxDepthDontUseSeeds", false);
  const float maxDepthScale = static_cast<float>(mp->userParams.get<double>("prematching.maxDepthScale", 1.5f));

  if(minMaxDepthDontUseSeeds)
  {
    const float minCamDist = static_cast<float>(mp->userParams.get<double>("prematching.minCamDist", 0.0f));
    const float maxCamDist = static_cast<float>(mp->userParams.get<double>("prematching.maxCamDist", 15.0f));

    minDepth = 0.0f;
    maxDepth = 0.0f;
    for(int c = 0; c < tcams.size(); c++)
    {
        int tc = tcams[c];
        minDepth += (mp->CArr[rc] - mp->CArr[tc]).size() * minCamDist;
        maxDepth += (mp->CArr[rc] - mp->CArr[tc]).size() * maxCamDist;
    }
    minDepth /= static_cast<float>(tcams.size());
    maxDepth /= static_cast<float>(tcams.size());
    midDepth = (minDepth + maxDepth) / 2.0f;
  }
  else
  {
    std::size_t nbDepths;
    mp->getMinMaxMidNbDepth(rc, minDepth, maxDepth, midDepth, nbDepths);
    maxDepth = maxDepth * maxDepthScale;
  }
}

StaticVector<float>* PlaneSweepingCuda::getDepthsByPixelSize(int rc, float minDepth, float midDepth, float maxDepth,
                                                               int scale, int step, int maxDepthsHalf)
{
    float d = (float)step;

    OrientedPoint rcplane;
    rcplane.p = mp->CArr[rc];
    rcplane.n = mp->iRArr[rc] * Point3d(0.0, 0.0, 1.0);
    rcplane.n = rcplane.n.normalize();

    int ndepthsMidMax = 0;
    float maxdepth = midDepth;
    while((maxdepth < maxDepth) && (ndepthsMidMax < maxDepthsHalf))
    {
        Point3d p = rcplane.p + rcplane.n * maxdepth;
        float pixSize = mp->getCamPixelSize(p, rc, (float)scale * d);
        maxdepth += pixSize;
        ndepthsMidMax++;
    }

    int ndepthsMidMin = 0;
    float mindepth = midDepth;
    while((mindepth > minDepth) && (ndepthsMidMin < maxDepthsHalf * 2 - ndepthsMidMax))
    {
        Point3d p = rcplane.p + rcplane.n * mindepth;
        float pixSize = mp->getCamPixelSize(p, rc, (float)scale * d);
        mindepth -= pixSize;
        ndepthsMidMin++;
    }

    // getNumberOfDepths
    float depth = mindepth;
    int ndepths = 0;
    float pixSize = 1.0f;
    while((depth < maxdepth) && (pixSize > 0.0f) && (ndepths < 2 * maxDepthsHalf))
    {
        Point3d p = rcplane.p + rcplane.n * depth;
        pixSize = mp->getCamPixelSize(p, rc, (float)scale * d);
        depth += pixSize;
        ndepths++;
    }

    StaticVector<float>* out = new StaticVector<float>();
    out->reserve(ndepths);

    // fill
    depth = mindepth;
    pixSize = 1.0f;
    ndepths = 0;
    while((depth < maxdepth) && (pixSize > 0.0f) && (ndepths < 2 * maxDepthsHalf))
    {
        out->push_back(depth);
        Point3d p = rcplane.p + rcplane.n * depth;
        pixSize = mp->getCamPixelSize(p, rc, (float)scale * d);
        depth += pixSize;
        ndepths++;
    }

    // check if it is asc
    for(int i = 0; i < out->size() - 1; i++)
    {
        if((*out)[i] >= (*out)[i + 1])
        {

            for(int j = 0; j <= i + 1; j++)
            {
                ALICEVISION_LOG_TRACE("getDepthsByPixelSize: check if it is asc: " << (*out)[j]);
            }
            throw std::runtime_error("getDepthsByPixelSize not asc.");
        }
    }

    return out;
}

StaticVector<float>* PlaneSweepingCuda::getDepthsRcTc(int rc, int tc, int scale, float midDepth,
                                                        int maxDepthsHalf)
{
    OrientedPoint rcplane;
    rcplane.p = mp->CArr[rc];
    rcplane.n = mp->iRArr[rc] * Point3d(0.0, 0.0, 1.0);
    rcplane.n = rcplane.n.normalize();

    Point2d rmid = Point2d((float)mp->getWidth(rc) / 2.0f, (float)mp->getHeight(rc) / 2.0f);
    Point2d pFromTar, pToTar; // segment of epipolar line of the principal point of the rc camera to the tc camera
    getTarEpipolarDirectedLine(&pFromTar, &pToTar, rmid, rc, tc, mp);

    int allDepths = static_cast<int>((pToTar - pFromTar).size());
    if(_verbose == true)
    {
        ALICEVISION_LOG_DEBUG("allDepths: " << allDepths);
    }

    Point2d pixelVect = ((pToTar - pFromTar).normalize()) * std::max(1.0f, (float)scale);
    // printf("%f %f %i %i\n",pixelVect.size(),((float)(scale*step)/3.0f),scale,step);

    Point2d cg = Point2d(0.0f, 0.0f);
    Point3d cg3 = Point3d(0.0f, 0.0f, 0.0f);
    int ncg = 0;
    // navigate through all pixels of the epilolar segment
    // Compute the middle of the valid pixels of the epipolar segment (in rc camera) of the principal point (of the rc camera)
    for(int i = 0; i < allDepths; i++)
    {
        Point2d tpix = pFromTar + pixelVect * (float)i;
        Point3d p;
        if(triangulateMatch(p, rmid, tpix, rc, tc, mp)) // triangulate principal point from rc with tpix
        {
            float depth = orientedPointPlaneDistance(p, rcplane.p, rcplane.n); // todo: can compute the distance to the camera (as it's the principal point it's the same)
            if( mp->isPixelInImage(tpix, tc)
                && (depth > 0.0f)
                && checkPair(p, rc, tc, mp, mp->getMinViewAngle(), mp->getMaxViewAngle()) )
            {
                cg = cg + tpix;
                cg3 = cg3 + p;
                ncg++;
            }
        }
    }
    if(ncg == 0)
    {
        return new StaticVector<float>();
    }
    cg = cg / (float)ncg;
    cg3 = cg3 / (float)ncg;
    allDepths = ncg;

    if(_verbose == true)
    {
        ALICEVISION_LOG_DEBUG("All correct depths: " << allDepths);
    }

    Point2d midpoint = cg;
    if(midDepth > 0.0f)
    {
        Point3d midPt = rcplane.p + rcplane.n * midDepth;
        mp->getPixelFor3DPoint(&midpoint, midPt, tc);
    }

    // compute the direction
    float direction = 1.0f;
    {
        Point3d p;
        if(!triangulateMatch(p, rmid, midpoint, rc, tc, mp))
        {
            StaticVector<float>* out = new StaticVector<float>();
            return out;
        }

        float depth = orientedPointPlaneDistance(p, rcplane.p, rcplane.n);

        if(!triangulateMatch(p, rmid, midpoint + pixelVect, rc, tc, mp))
        {
            StaticVector<float>* out = new StaticVector<float>();
            return out;
        }

        float depthP1 = orientedPointPlaneDistance(p, rcplane.p, rcplane.n);
        if(depth > depthP1)
        {
            direction = -1.0f;
        }
    }

    StaticVector<float>* out1 = new StaticVector<float>();
    out1->reserve(2 * maxDepthsHalf);

    Point2d tpix = midpoint;
    float depthOld = -1.0f;
    int istep = 0;
    bool ok = true;

    // compute depths for all pixels from the middle point to on one side of the epipolar line
    while((out1->size() < maxDepthsHalf) && (mp->isPixelInImage(tpix, tc) == true) && (ok == true))
    {
        tpix = tpix + pixelVect * direction;

        Point3d refvect = mp->iCamArr[rc] * rmid;
        Point3d tarvect = mp->iCamArr[tc] * tpix;
        float rptpang = angleBetwV1andV2(refvect, tarvect);

        Point3d p;
        ok = triangulateMatch(p, rmid, tpix, rc, tc, mp);

        float depth = orientedPointPlaneDistance(p, rcplane.p, rcplane.n);
        if (mp->isPixelInImage(tpix, tc)
            && (depth > 0.0f) && (depth > depthOld)
            && checkPair(p, rc, tc, mp, mp->getMinViewAngle(), mp->getMaxViewAngle())
            && (rptpang > mp->getMinViewAngle())  // WARNING if vects are near parallel thaen this results to strange angles ...
            && (rptpang < mp->getMaxViewAngle())) // this is the propper angle ... beacause is does not depend on the triangluated p
        {
            out1->push_back(depth);
            // if ((tpix.x!=tpixold.x)||(tpix.y!=tpixold.y)||(depthOld>=depth))
            //{
            // printf("after %f %f %f %f %i %f %f\n",tpix.x,tpix.y,depth,depthOld,istep,ang,kk);
            //};
        }
        else
        {
            ok = false;
        }
        depthOld = depth;
        istep++;
    }

    StaticVector<float>* out2 = new StaticVector<float>();
    out2->reserve(2 * maxDepthsHalf);
    tpix = midpoint;
    istep = 0;
    ok = true;

    // compute depths for all pixels from the middle point to the other side of the epipolar line
    while((out2->size() < maxDepthsHalf) && (mp->isPixelInImage(tpix, tc) == true) && (ok == true))
    {
        Point3d refvect = mp->iCamArr[rc] * rmid;
        Point3d tarvect = mp->iCamArr[tc] * tpix;
        float rptpang = angleBetwV1andV2(refvect, tarvect);

        Point3d p;
        ok = triangulateMatch(p, rmid, tpix, rc, tc, mp);

        float depth = orientedPointPlaneDistance(p, rcplane.p, rcplane.n);
        if(mp->isPixelInImage(tpix, tc)
            && (depth > 0.0f) && (depth < depthOld) 
            && checkPair(p, rc, tc, mp, mp->getMinViewAngle(), mp->getMaxViewAngle())
            && (rptpang > mp->getMinViewAngle())  // WARNING if vects are near parallel thaen this results to strange angles ...
            && (rptpang < mp->getMaxViewAngle())) // this is the propper angle ... beacause is does not depend on the triangluated p
        {
            out2->push_back(depth);
            // printf("%f %f\n",tpix.x,tpix.y);
        }
        else
        {
            ok = false;
        }

        depthOld = depth;
        tpix = tpix - pixelVect * direction;
    }

    // printf("out2\n");
    StaticVector<float>* out = new StaticVector<float>();
    out->reserve(2 * maxDepthsHalf);
    for(int i = out2->size() - 1; i >= 0; i--)
    {
        out->push_back((*out2)[i]);
        // printf("%f\n",(*out2)[i]);
    }
    // printf("out1\n");
    for(int i = 0; i < out1->size(); i++)
    {
        out->push_back((*out1)[i]);
        // printf("%f\n",(*out1)[i]);
    }

    delete out2;
    delete out1;

    // we want to have it in ascending order
    if((*out)[0] > (*out)[out->size() - 1])
    {
        StaticVector<float>* outTmp = new StaticVector<float>();
        outTmp->reserve(out->size());
        for(int i = out->size() - 1; i >= 0; i--)
        {
            outTmp->push_back((*out)[i]);
        }
        delete out;
        out = outTmp;
    }

    // check if it is asc
    for(int i = 0; i < out->size() - 1; i++)
    {
        if((*out)[i] > (*out)[i + 1])
        {

            for(int j = 0; j <= i + 1; j++)
            {
                ALICEVISION_LOG_TRACE("getDepthsRcTc: check if it is asc: " << (*out)[j]);
            }
            ALICEVISION_LOG_WARNING("getDepthsRcTc: not asc");

            if(out->size() > 1)
            {
                qsort(&(*out)[0], out->size(), sizeof(float), qSortCompareFloatAsc);
            }
        }
    }

    if(_verbose == true)
    {
        ALICEVISION_LOG_DEBUG("used depths: " << out->size());
    }

    return out;
}

bool PlaneSweepingCuda::refineRcTcDepthMap(bool useTcOrRcPixSize, int nStepsToRefine, StaticVector<float>& out_simMap,
                                             StaticVector<float>& out_rcDepthMap, int rc, int tc, int scale, int wsh,
                                             float gammaC, float gammaP, float epipShift, int xFrom, int wPart)
{
    // int w = mp->getWidth(rc)/scale;
    int w = wPart;
    int h = mp->getHeight(rc) / scale;

    long t1 = clock();

    StaticVector<int> camsids(2);
    camsids[0] = addCam(rc, scale, nullptr, __FUNCTION__);

    if(_verbose)
        ALICEVISION_LOG_DEBUG("\t- rc: " << rc << std::endl << "\t- tcams: " << tc);

    camsids[1] = addCam(tc, scale, nullptr, __FUNCTION__);

    std::vector<cameraStruct> ttcams( camsids.size() );
    for(int i = 0; i < camsids.size(); i++)
    {
        ttcams[i] = cams[camsids[i]];
        ttcams[i].camId = camsids[i];
    }

    // sweep
    ps_refineRcDepthMap(ps_texs_arr, out_simMap.getDataWritable().data(), out_rcDepthMap.getDataWritable().data(), nStepsToRefine,
                        ttcams,
                        w, h, mp->getWidth(rc) / scale,
                        mp->getHeight(rc) / scale, scale - 1, _CUDADeviceNo, _nImgsInGPUAtTime, _scales, _verbose, wsh,
                        gammaC, gammaP, epipShift, useTcOrRcPixSize, xFrom);

    /*
    CudaHostMemoryHeap<float, 3> tmpSimVolume_hmh(CudaSize<3>(201, 201, nStepsToRefine));

    ps_refineRcTcDepthMapSGM(
            &tmpSimVolume_hmh,
            &simMap_hmh,
            &rcDepthMap_hmh,
            nStepsToRefine,
            rcDepthMap_hmh,
            ttcams, camsids->size(),
            w, h,
            scale-1, _scales,
            _verbose, wsh, gammaC, gammaP, epipShift,
            0.0001f, 1.0f
    );
    */

    if(_verbose)
        mvsUtils::printfElapsedTime(t1);

    return true;
}

void PlaneSweepingCuda::allocTempVolume( std::vector<CudaDeviceMemoryPitched<float, 3>*>& volSim_dmp,
                                         const int max_tcs,
                                         const int volDimX,
                                         const int volDimY,
                                         const int zDimsAtATime )
{
    volSim_dmp.resize( max_tcs );
    for( int ct=0; ct<max_tcs; ct++ )
    {
        // allocate twice the number of dimensions-at-a-time
        // first half: best values
        // second half: second best values
        volSim_dmp[ct] = new CudaDeviceMemoryPitched<float, 3>(CudaSize<3>(volDimX, volDimY, zDimsAtATime * 2));
    }
}

void PlaneSweepingCuda::freeTempVolume( std::vector<CudaDeviceMemoryPitched<float, 3>*>& volSim_dmp )
{
    for( auto ptr: volSim_dmp ) delete ptr;
    volSim_dmp.clear();
}

/* Be very careful with volume indexes:
 * volume is indexed with the same index as tc. The values of tc can be quite different.
 * depths is indexed with the index_set elements
 */
void PlaneSweepingCuda::sweepPixelsToVolume( std::vector<CudaDeviceMemoryPitched<float, 3>*>& volSim_dmp,
                                             const int volDimX,
                                             const int volDimY,
                                             const int volStepXY,
                                             std::vector<OneTC>& tcs,
                                             const int zDimsAtATime,
                                             const std::vector<float>& rc_depths,
                                             int rc,
                                             const StaticVector<int>& tc_in,
                                             StaticVectorBool* rcSilhoueteMap,
                                             int wsh, float gammaC, float gammaP,
                                             int scale, int step,
                                             float epipShift )
{
    const int max_tcs = _nImgsInGPUAtTime - 1;

    auto it  = tcs.begin();
    auto end = tcs.end();

    while( it != end )
    {
        std::vector<OneTC> sub_tcs;

        for( int i=0; i<max_tcs && it!=end; i++ )
        {
            sub_tcs.emplace_back( *it );
            it++;
        }

        sweepPixelsToVolumeSubset( volSim_dmp,
                                   volDimX, volDimY, volStepXY,
                                   sub_tcs,
                                   zDimsAtATime,
                                   rc_depths,
                                   rc,
                                   tc_in,
                                   rcSilhoueteMap,
                                   wsh,
                                   gammaC, gammaP, scale, step, epipShift );
    }
    cudaDeviceSynchronize();
}

void PlaneSweepingCuda::sweepPixelsToVolumeSubset(
    std::vector<CudaDeviceMemoryPitched<float, 3>*>& volSim_dmp,
    const int volDimX,
    const int volDimY,
    const int volStepXY,
    std::vector<OneTC>& tcs,
    const int zDimsAtATime,
    const std::vector<float>& rc_depths,
    int rc,
    const StaticVector<int>& tc_in,
    StaticVectorBool* rcSilhoueteMap,
    int wsh, float gammaC, float gammaP,
    int scale, int step,
    float epipShift )
{
    clock_t t1 = tic();

    const int stepLessWidth  = mp->getWidth(rc) / scale;
    const int stepLessHeight = mp->getHeight(rc) / scale;

    const int max_tcs = tcs.size();

    if(_verbose)
        ALICEVISION_LOG_DEBUG("sweepPixelsVolume:" << std::endl
                                << "\t- scale: " << scale << std::endl
                                << "\t- step: " << step << std::endl
                                << "\t- volStepXY: " << volStepXY << std::endl
                                << "\t- volDimX: " << volDimX << std::endl
                                << "\t- volDimY: " << volDimY );

    cameraStruct              rcam;
    std::vector<cameraStruct> tcams( max_tcs );

    const int camid = addCam(rc, scale, rcSilhoueteMap, __FUNCTION__ );
    cams[camid].camId = camid;
    rcam = cams[camid];

    for( int ct=0; ct<max_tcs; ct++ )
    {
        const int camid = addCam(tcs[ct].getTCIndex(), scale, nullptr, __FUNCTION__ );
        cams[camid].camId = camid;
        tcams[ct] = cams[camid];
    }

    if(_verbose)
    {
        std::ostringstream ostr;
        ostr << "rc: " << rc << " tcams: ";
        for( int ct=0; ct<max_tcs; ct++ )
        {
            ostr << " " << tcs[ct].getTCIndex();
        }

        ALICEVISION_LOG_DEBUG( ostr.str() );
    }

    // last synchronous step
    cudaDeviceSynchronize();
    _camsBasesDev.copyFrom( _camsBasesHst );

    // copy the vector of depths to GPU
    // TODO - move out - don't do this here
    const float* depth_data = rc_depths.data();
    CudaDeviceMemory<float> depths_dev( depth_data, rc_depths.size() );

    {
      const int max_tcs = tcams.size();
      pr_printfDeviceMemoryInfo();
      double mbytes = max_tcs * volSim_dmp[0]->getBytesPadded();
      mbytes /= (1024.0 * 1024.0);
      ALICEVISION_LOG_DEBUG(__FUNCTION__ << ": total size of volume maps for "<< max_tcs <<" images in GPU memory: approx "<< mbytes <<" MB");
    }

    ps_computeSimilarityVolume(
            ps_texs_arr,     // indexed with tcams[].camId
            volSim_dmp,
            rcam, tcams,
            stepLessWidth, stepLessHeight,
            volStepXY, volDimX, volDimY,
            zDimsAtATime,
            depths_dev,
            tcs,
            wsh,
            _nbestkernelSizeHalf,
            scale - 1,
            _scales, _verbose,
            false, _nbest,
            gammaC, gammaP, subPixel, epipShift);

    if(_verbose)
    {
        printf("sweepPixelsToVolumeSubset elapsed time: %f ms \n", toc(t1));
    }
}

/**
 * @param[inout] volume input similarity volume (after Z reduction)
 */
bool PlaneSweepingCuda::SGMoptimizeSimVolume(int rc, StaticVector<unsigned char>* volume, 
                                               int volDimX, int volDimY, int volDimZ, 
                                               int volStepXY, int scale,
                                               unsigned char P1, unsigned char P2)
{
    if(_verbose)
        ALICEVISION_LOG_DEBUG("SGM optimizing volume:" << std::endl
                              << "\t- volDimX: " << volDimX << std::endl
                              << "\t- volDimY: " << volDimY << std::endl
                              << "\t- volDimZ: " << volDimZ);

    long t1 = clock();

    ps_SGMoptimizeSimVolume(ps_texs_arr,
                            cams[addCam(rc, scale, nullptr, __FUNCTION__ )],
                            volume->getDataWritable().data(), volDimX, volDimY, volDimZ, volStepXY,
                            _verbose, P1, P2, scale - 1, // TODO: move the '- 1' inside the function
                            _CUDADeviceNo, _nImgsInGPUAtTime, _scales);

    if(_verbose)
        mvsUtils::printfElapsedTime(t1);

    return true;
}

// make_float3(avail,total,used)
Point3d PlaneSweepingCuda::getDeviceMemoryInfo()
{
    size_t iavail;
    size_t itotal;
    cudaMemGetInfo(&iavail, &itotal);
    size_t iused = itotal - iavail;

    double avail = (double)iavail / (1024.0 * 1024.0);
    double total = (double)itotal / (1024.0 * 1024.0);
    double used = (double)iused / (1024.0 * 1024.0);

    return Point3d(avail, total, used);
}

bool PlaneSweepingCuda::fuseDepthSimMapsGaussianKernelVoting(int w, int h, StaticVector<DepthSim>& oDepthSimMap,
                                                               const StaticVector<StaticVector<DepthSim>*>& dataMaps,
                                                               int nSamplesHalf, int nDepthsToRefine, float sigma)
{
    long t1 = clock();

    // sweep
    std::vector<CudaHostMemoryHeap<float2, 2>*> dataMaps_hmh(dataMaps.size());
    for(int i = 0; i < dataMaps.size(); i++)
    {
        dataMaps_hmh[i] = new CudaHostMemoryHeap<float2, 2>(CudaSize<2>(w, h));
        for(int y = 0; y < h; y++)
        {
            for(int x = 0; x < w; x++)
            {
                float2& data_hmh = (*dataMaps_hmh[i])(x, y);
                const DepthSim& data = (*dataMaps[i])[y * w + x];
                data_hmh.x = data.depth;
                data_hmh.y = data.sim;
            }
        }
    }

    CudaHostMemoryHeap<float2, 2> oDepthSimMap_hmh(CudaSize<2>(w, h));

    ps_fuseDepthSimMapsGaussianKernelVoting(&oDepthSimMap_hmh, dataMaps_hmh, dataMaps.size(), nSamplesHalf,
                                            nDepthsToRefine, sigma, w, h, _verbose);

    for(int y = 0; y < h; y++)
    {
        for(int x = 0; x < w; x++)
        {
            const float2& oDepthSim_hmh = oDepthSimMap_hmh(x, y);
            DepthSim& oDepthSim = oDepthSimMap[y * w + x];
            oDepthSim.depth = oDepthSim_hmh.x;
            oDepthSim.sim = oDepthSim_hmh.y;
        }
    }

    for(int i = 0; i < dataMaps.size(); i++)
    {
        delete dataMaps_hmh[i];
    }

    if(_verbose)
        mvsUtils::printfElapsedTime(t1);

    return true;
}

bool PlaneSweepingCuda::optimizeDepthSimMapGradientDescent(StaticVector<DepthSim>& oDepthSimMap,
                                                           StaticVector<const StaticVector<DepthSim>*>& dataMaps, int rc,
                                                           int nSamplesHalf, int nDepthsToRefine, float sigma,
                                                           int nIters, int yFrom, int hPart)
{
    if(_verbose)
        ALICEVISION_LOG_DEBUG("optimizeDepthSimMapGradientDescent.");

    int scale = 1;
    int w = mp->getWidth(rc);
    int h = hPart;

    long t1 = clock();

    StaticVector<int> camsids;
    camsids.push_back(addCam(rc, scale, nullptr, __FUNCTION__ ));
    if(_verbose)
        printf("rc: %i, ", rc);

    std::vector<cameraStruct> ttcams( camsids.size() );
    for(int i = 0; i < camsids.size(); i++)
    {
        ttcams[i]       = cams[camsids[i]];
        ttcams[i].camId = camsids[i];
    }

    // sweep
    std::vector<CudaHostMemoryHeap<float2, 2>*> dataMaps_hmh(dataMaps.size());
    for(int i = 0; i < dataMaps.size(); i++)
    {
        const StaticVector<DepthSim>& depthSimMap = *dataMaps[i];
        dataMaps_hmh[i] = new CudaHostMemoryHeap<float2, 2>(CudaSize<2>(w, h));
        CudaHostMemoryHeap<float2, 2>& dataMap_hmh = *dataMaps_hmh[i];
        for(int y = 0; y < h; y++)
        {
            for(int x = 0; x < w; x++)
            {
                int jO = (y + yFrom) * w + x;
                float2& h_data = dataMap_hmh(x, y);
                const DepthSim& data = depthSimMap[jO];
                h_data.x = data.depth;
                h_data.y = data.sim;
            }
        }
    }

    CudaHostMemoryHeap<float2, 2> oDepthSimMap_hmh(CudaSize<2>(w, h));

    ps_optimizeDepthSimMapGradientDescent(ps_texs_arr, &oDepthSimMap_hmh, dataMaps_hmh,
                                          dataMaps.size(), nSamplesHalf, nDepthsToRefine, nIters, sigma, ttcams,
                                          camsids.size(), w, h, scale - 1, _CUDADeviceNo, _nImgsInGPUAtTime, _scales,
                                          _verbose, yFrom);

    for(int y = 0; y < h; y++)
    {
        for(int x = 0; x < w; x++)
        {
            int jO = (y + yFrom) * w + x;
            DepthSim& oDepthSim = oDepthSimMap[jO];
            const float2& h_oDepthSim = oDepthSimMap_hmh(x, y);

            oDepthSim.depth = h_oDepthSim.x;
            oDepthSim.sim = h_oDepthSim.y;
        }
    }

    for(int i = 0; i < dataMaps.size(); i++)
    {
        delete dataMaps_hmh[i];
    }

    if(_verbose)
        mvsUtils::printfElapsedTime(t1);

    return true;
}

bool PlaneSweepingCuda::getSilhoueteMap(StaticVectorBool* oMap, int scale, int step, const rgb maskColor, int rc)
{
    if(_verbose)
        ALICEVISION_LOG_DEBUG("getSilhoueteeMap: rc: " << rc);

    int w = mp->getWidth(rc) / scale;
    int h = mp->getHeight(rc) / scale;

    long t1 = clock();

    int camId = addCam(rc, scale, nullptr, __FUNCTION__ );

    uchar4 maskColorRgb;
    maskColorRgb.x = maskColor.r;
    maskColorRgb.y = maskColor.g;
    maskColorRgb.z = maskColor.b;
    maskColorRgb.w = 1.0f;

    CudaHostMemoryHeap<bool, 2> omap_hmh(CudaSize<2>(w / step, h / step));

    ps_getSilhoueteMap( ps_texs_arr, &omap_hmh, w, h, scale - 1,
                        step, camId, maskColorRgb, _verbose );

    for(int i = 0; i < (w / step) * (h / step); i++)
    {
        (*oMap)[i] = omap_hmh.getBuffer()[i];
    }

    if(_verbose)
        mvsUtils::printfElapsedTime(t1);

    return true;
}

int listCUDADevices(bool verbose)
{
    return ps_listCUDADevices(verbose);
}

} // namespace depthMap
} // namespace aliceVision
