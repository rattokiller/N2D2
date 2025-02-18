/*
    (C) Copyright 2015 CEA LIST. All Rights Reserved.
    Contributor(s): Olivier BICHLER (olivier.bichler@cea.fr)

    This software is governed by the CeCILL-C license under French law and
    abiding by the rules of distribution of free software.  You can  use,
    modify and/ or redistribute the software under the terms of the CeCILL-C
    license as circulated by CEA, CNRS and INRIA at the following URL
    "http://www.cecill.info".

    As a counterpart to the access to the source code and  rights to copy,
    modify and redistribute granted by the license, users are provided only
    with a limited warranty  and the software's author,  the holder of the
    economic rights,  and the successive licensors  have only  limited
    liability.

    The fact that you are presently reading this means that you have had
    knowledge of the CeCILL-C license and that you accept its terms.
*/

#include "Cell/ROIPoolingCell_Frame.hpp"
#include "DeepNet.hpp"
#include "GradientCheck.hpp"
#include "StimuliProvider.hpp"

N2D2::Registrar<N2D2::ROIPoolingCell>
N2D2::ROIPoolingCell_Frame::mRegistrar("Frame",
                                       N2D2::ROIPoolingCell_Frame::create);

N2D2::ROIPoolingCell_Frame::ROIPoolingCell_Frame(const DeepNet& deepNet, 
                                                 const std::string& name,
                                                 StimuliProvider& sp,
                                                 unsigned int outputsWidth,
                                                 unsigned int outputsHeight,
                                                 unsigned int nbOutputs,
                                                 ROIPooling pooling)
    : Cell(deepNet, name, nbOutputs),
      ROIPoolingCell(deepNet, name, sp, outputsWidth, outputsHeight, nbOutputs, pooling),
      Cell_Frame<Float_T>(deepNet, name, nbOutputs)
{
    // ctor
    mInputs.matchingDims({});
    mDiffOutputs.matchingDims({});
}

void N2D2::ROIPoolingCell_Frame::initialize()
{
    if (mInputs.size() < 2) {
        throw std::runtime_error("At least two inputs are required for"
                                 " ROIPoolingCell " + mName);
    }

    if (mInputs[0].dimX() * mInputs[0].dimY() * mInputs[0].dimZ() != 4) {
        throw std::runtime_error("The first input must have a XYZ size of 4 for"
                                 " ROIPoolingCell " + mName);
    }

    unsigned int kRef = 1;
    const unsigned int inputBatch = mInputs[kRef].dimB();
    unsigned int nbChannels = 0;

    for (unsigned int k = kRef, size = mInputs.size(); k < size; ++k) {
        if (mInputs[k].size() == 0) {
            throw std::runtime_error("Zero-sized input for ROIPoolingCell "
                                      + mName);
        }

        if (mInputs[k].dimB() != inputBatch) {
            throw std::runtime_error("Input batch size must match for"
                                     " ROIPoolingCell" + mName);
        }

        if (mArgMax.size() == (k - 1)) {
            mArgMax.push_back(new Tensor<PoolCell_Frame_Kernels::ArgMax>(
                mOutputs.dims()));
        }

        nbChannels += mInputs[k].dimZ();
    }

    if (nbChannels != mOutputs.dimZ()) {
        throw std::runtime_error("The number of output channels must match the "
            "total number of input channels for ROIPoolingCell" + mName);
    }
    mParentProposals = mInputs[kRef-1].dimB()/mInputs[kRef].dimB();
}

void N2D2::ROIPoolingCell_Frame::propagate(bool inference)
{
    mInputs.synchronizeDBasedToH();

    const float alpha = 1.0f;
    float beta = 0.0f;

    const Tensor<Float_T>& proposals = tensor_cast<Float_T>(mInputs[0]);
    unsigned int outputOffset = 0;

    for (unsigned int k = 1, size = mInputs.size(); k < size; ++k) {
        const Tensor<Float_T>& input = tensor_cast<Float_T>(mInputs[k]);

        const double xRatio = std::ceil(mStimuliProvider.getSizeX()
                                        / (double)input.dimX());
        const double yRatio = std::ceil(mStimuliProvider.getSizeY()
                                        / (double)input.dimY());

        if (mPooling == Max) {
#if defined(_OPENMP) && _OPENMP >= 200805
#pragma omp parallel for collapse(2) if (size > 16)
#else
#pragma omp parallel for if (mOutputs.dimB() > 4 && size > 16)
#endif
            for (int batchPos = 0; batchPos < (int)mOutputs.dimB(); ++batchPos)
            {
                for (unsigned int channel = 0; channel < input.dimZ();
                    ++channel)
                {
                    const Tensor<Float_T>& proposal = proposals[batchPos];
                    const unsigned int inputBatch = batchPos / proposals.dimB();

                    Float_T x = proposal(0) / xRatio;
                    Float_T y = proposal(1) / yRatio;
                    Float_T w = proposal(2) / xRatio;
                    Float_T h = proposal(3) / yRatio;

                    assert(w >= 0);
                    assert(h >= 0);

                    // Crop ROI to image boundaries
                    if (x < 0) {
                        w+= x;
                        x = 0;
                    }
                    if (y < 0) {
                        h+= y;
                        y = 0;
                    }
                    if (x + w > (int)input.dimX())
                        w = input.dimX() - x;
                    if (y + h > (int)input.dimY())
                        h = input.dimY() - y;

                    const Float_T poolWidth = w / mOutputs.dimX();
                    const Float_T poolHeight = h / mOutputs.dimY();

                    assert(poolWidth >= 0);
                    assert(poolHeight >= 0);

                    for (unsigned int oy = 0; oy < mOutputs.dimY(); ++oy) {
                        for (unsigned int ox = 0; ox < mOutputs.dimX(); ++ox) {
                            const unsigned int sxMin = (unsigned int)(x
                                                    + ox * poolWidth);
                            const unsigned int sxMax = (unsigned int)(x
                                                    + (ox + 1) * poolWidth);
                            const unsigned int syMin = (unsigned int)(y
                                                    + oy * poolHeight);
                            const unsigned int syMax = (unsigned int)(y
                                                    + (oy + 1) * poolHeight);

                            // For each channel, compute the pool value
                            Float_T poolValue = 0.0;

                            unsigned int ixMax = 0;
                            unsigned int iyMax = 0;
                            bool valid = false;

                            for (unsigned int sy = syMin; sy < syMax; ++sy) {
                                for (unsigned int sx = sxMin; sx < sxMax; ++sx)
                                {
                                    const Float_T value = input(sx,
                                                            sy,
                                                            channel,
                                                            inputBatch);

                                    if (!valid || value > poolValue) {
                                        poolValue = value;
                                        valid = true;

                                        ixMax = sx;
                                        iyMax = sy;
                                    }
                                }
                            }

                            mArgMax[k-1](ox, oy, channel, batchPos)
                                = PoolCell_Frame_Kernels::ArgMax(ixMax,
                                                                 iyMax,
                                                                 channel,
                                                                 valid);

                            mOutputs(ox, oy, outputOffset + channel, batchPos)
                                = alpha * poolValue
                                  + beta * mOutputs(ox, oy, outputOffset
                                                        + channel, batchPos);
                        }
                    }
                }
            }
        }
        else if (mPooling == Average) {
#if defined(_OPENMP) && _OPENMP >= 200805
#pragma omp parallel for collapse(2) if (size > 16)
#else
#pragma omp parallel for if (mOutputs.dimB() > 4 && size > 16)
#endif
            for (int batchPos = 0; batchPos < (int)mOutputs.dimB(); ++batchPos)
            {
                for (unsigned int channel = 0; channel < input.dimZ();
                    ++channel)
                {
                    const Tensor<Float_T>& proposal = proposals[batchPos];
                    const unsigned int inputBatch = batchPos / proposals.dimB();

                    Float_T x = proposal(0) / xRatio;
                    Float_T y = proposal(1) / yRatio;
                    Float_T w = proposal(2) / xRatio;
                    Float_T h = proposal(3) / yRatio;

                    // Crop ROI to image boundaries
                    if (x < 0) {
                        w+= x;
                        x = 0;
                    }
                    if (y < 0) {
                        h+= y;
                        y = 0;
                    }
                    if (x + w > (int)input.dimX())
                        w = input.dimX() - x;
                    if (y + h > (int)input.dimY())
                        h = input.dimY() - y;

                    const Float_T poolWidth = w / mOutputs.dimX();
                    const Float_T poolHeight = h / mOutputs.dimY();

                    for (unsigned int oy = 0; oy < mOutputs.dimY(); ++oy) {
                        for (unsigned int ox = 0; ox < mOutputs.dimX(); ++ox) {
                            const unsigned int sxMin = (unsigned int)(x
                                                    + ox * poolWidth);
                            const unsigned int sxMax = (unsigned int)(x
                                                    + (ox + 1) * poolWidth);
                            const unsigned int syMin = (unsigned int)(y
                                                    + oy * poolHeight);
                            const unsigned int syMax = (unsigned int)(y
                                                    + (oy + 1) * poolHeight);

                            // For each channel, compute the pool value
                            Float_T poolValue = 0.0;
                            unsigned int poolCount = 0;

                            for (unsigned int sy = syMin; sy < syMax; ++sy) {
                                for (unsigned int sx = sxMin; sx < sxMax; ++sx)
                                {
                                    poolValue += input(sx,
                                                            sy,
                                                            channel,
                                                            inputBatch);
                                }
                            }

                            poolCount += (sxMax - sxMin)*(syMax - syMin);

                            mOutputs(ox, oy, outputOffset + channel, batchPos)
                                = alpha * ((poolCount > 0) ?
                                              (poolValue / poolCount) : 0.0)
                                  + beta * mOutputs(ox, oy, outputOffset
                                                        + channel, batchPos);
                        }
                    }
                }
            }
        }
        else {
            // Bilinear
            Float_T xOffset = 0.0;
            Float_T yOffset = 0.0;

            if (mFlip) {
                xOffset = (mStimuliProvider.getSizeX() - 1) / xRatio
                            - (input.dimX() - 1);
                yOffset = (mStimuliProvider.getSizeY() - 1) / yRatio
                            - (input.dimY() - 1);
            }

#if defined(_OPENMP) && _OPENMP >= 200805
#pragma omp parallel for collapse(2) if (size > 16)
#else
#pragma omp parallel for if (mOutputs.dimB() > 4 && size > 16)
#endif
            for (int batchPos = 0; batchPos < (int)mOutputs.dimB(); ++batchPos)
            {
                for (unsigned int channel = 0; channel < input.dimZ();
                    ++channel)
                {
                    const Tensor<Float_T>& proposal = proposals[batchPos];
                    const unsigned int inputBatch = batchPos / proposals.dimB();

                    Float_T x = proposal(0) / xRatio - xOffset;
                    Float_T y = proposal(1) / yRatio - yOffset;
                    Float_T w = proposal(2) / xRatio;
                    Float_T h = proposal(3) / yRatio;

                    // Crop ROI to image boundaries
                    if (x < 0) {
                        w+= x;
                        x = 0;
                    }
                    if (y < 0) {
                        h+= y;
                        y = 0;
                    }
                    if (x + w > (int)input.dimX())
                        w = input.dimX() - x;
                    if (y + h > (int)input.dimY())
                        h = input.dimY() - y;

                    Float_T xPoolRatio, yPoolRatio;

                    if (mPooling == BilinearTF) {
                        xPoolRatio = w / (mOutputs.dimX() - 1);
                        yPoolRatio = h / (mOutputs.dimY() - 1);
                    }
                    else {
                        xPoolRatio = w / mOutputs.dimX();
                        yPoolRatio = h / mOutputs.dimY();
                    }

                    for (unsigned int oy = 0; oy < mOutputs.dimY(); ++oy) {
                        for (unsigned int ox = 0; ox < mOutputs.dimX(); ++ox) {
                            Float_T sx, sy;

                            if (mPooling == BilinearTF) {
                                sx = std::min<Float_T>(x + ox * xPoolRatio,
                                    input.dimX() - 1);
                                sy = std::min<Float_T>(y + oy * yPoolRatio,
                                    input.dimY() - 1);
                            }
                            else {
                                // -0.5 + (ox + 0.5) and not ox because the
                                // interpolation is done relative to the CENTER
                                // of the pixels
                                sx = x + Utils::clamp<Float_T>(
                                    -0.5 + (ox + 0.5) * xPoolRatio, 0, w - 1);
                                sy = y + Utils::clamp<Float_T>(
                                    -0.5 + (oy + 0.5) * yPoolRatio, 0, h - 1);
                            }

                            const unsigned int sx0 = (int)(sx);
                            const unsigned int sy0 = (int)(sy);
                            const Float_T dx = sx - sx0;
                            const Float_T dy = sy - sy0;

                            const bool invalid = mIgnorePad ?
                                    (((sx0 + 1 < input.dimX() )
                                        && (sy0 + 1 < input.dimY() ))  ?
                                        false : true) : false;


                            const Float_T i00 = (!invalid) ?
                                    input(sx0, sy0, channel, inputBatch)
                                    : 0.0;

                            const Float_T i10 = (sx0 + 1 < input.dimX()) && (!invalid) ?
                                        input(sx0 + 1, sy0, channel, inputBatch)
                                        : 0.0;
                            const Float_T i01 = (sy0 + 1 < input.dimY()) && (!invalid) ?
                                        input(sx0, sy0 + 1, channel, inputBatch)
                                        : 0.0;
                            const Float_T i11 = (sx0 + 1 < input.dimX() && sy0 + 1
                                                    < input.dimY()) && (!invalid) ?
                                                input(sx0 + 1, sy0 + 1, channel, inputBatch)
                                                : 0.0;

                            const Float_T value
                                = i00 * (1 - dx) * (1 - dy)
                                + i10 * dx * (1 - dy)
                                + i01 * (1 - dx) * dy
                                + i11 * (dx * dy);

                            mOutputs(ox, oy, outputOffset + channel, batchPos)
                                = alpha * value
                                  + beta * mOutputs(ox, oy, outputOffset
                                                        + channel, batchPos);
                        }
                    }
                }
            }
        }

        outputOffset += input.dimZ();
    }

    Cell_Frame<Float_T>::propagate(inference);
    mDiffInputs.clearValid();
}

void N2D2::ROIPoolingCell_Frame::backPropagate()
{
    if (!mDiffInputs.isValid())
        return;

    Cell_Frame<Float_T>::backPropagate();

    const Float_T alpha = 1.0;

    const Tensor<Float_T>& proposals = tensor_cast_nocopy<Float_T>(mInputs[0]);
    unsigned int outputOffset = 0;

    for (unsigned int k = 1, size = mInputs.size(); k < size; ++k) {
        if (mDiffOutputs[k].empty()) {
            outputOffset += mDiffOutputs[k].dimZ();
            continue;
        }

        //const Float_T beta = (mDiffOutputs[k].isValid()) ? 1.0 : 0.0;

        Tensor<Float_T> diffOutput = (mDiffOutputs[k].isValid())
            ? tensor_cast<Float_T>(mDiffOutputs[k])
            : tensor_cast_nocopy<Float_T>(mDiffOutputs[k]);

        if (!mDiffOutputs[k].isValid()) {
            diffOutput.fill(0.0);
            mDiffOutputs[k].setValid();
        }

        const double xRatio = std::ceil(mStimuliProvider.getSizeX()
                                        / (double)mInputs[k].dimX());
        const double yRatio = std::ceil(mStimuliProvider.getSizeY()
                                        / (double)mInputs[k].dimY());

        if (mPooling == Max) {
#if defined(_OPENMP) && _OPENMP >= 200805
#pragma omp parallel for collapse(2) if (size > 16)
#else
#pragma omp parallel for if (mDiffInputs.dimB() > 4 && size > 16)
#endif
            for (int batchPos = 0; batchPos < (int)mDiffInputs.dimB();
                ++batchPos)
            {
                for (unsigned int channel = 0; channel < mDiffOutputs[k].dimZ();
                     ++channel)
                {
                    const Tensor<Float_T>& proposal = proposals[batchPos];
                    const unsigned int inputBatch = batchPos / proposals.dimB();
                    //const Float_T betaBatch = (batchPos % proposals.dimB() == 0)
                    //                            ? beta : 1.0;

                    Float_T x = proposal(0) / xRatio;
                    Float_T y = proposal(1) / yRatio;
                    Float_T w = proposal(2) / xRatio;
                    Float_T h = proposal(3) / yRatio;

                    // Crop ROI to image boundaries
                    if (x < 0) {
                        w+= x;
                        x = 0;
                    }
                    if (y < 0) {
                        h+= y;
                        y = 0;
                    }
                    if (x + w > (int)mInputs[k].dimX())
                        w = mInputs[k].dimX() - x;
                    if (y + h > (int)mInputs[k].dimY())
                        h = mInputs[k].dimY() - y;

                    const Float_T poolWidth = w / mOutputs.dimX();
                    const Float_T poolHeight = h / mOutputs.dimY();

                    const unsigned int ixMin = (unsigned int)(x);
                    const unsigned int ixMax = (unsigned int)(x + w);
                    const unsigned int iyMin = (unsigned int)(y);
                    const unsigned int iyMax = (unsigned int)(y + h);

                    assert(ixMin <= ixMax);
                    assert(ixMax < mDiffOutputs[k].dimX());
                    assert(iyMin <= iyMax);
                    assert(iyMax < mDiffOutputs[k].dimY());

                    for (unsigned int iy = 0; iy < mDiffOutputs[k].dimY(); ++iy)
                    {
                        for (unsigned int ix = 0; ix < mDiffOutputs[k].dimX();
                            ++ix)
                        {
                            if (ix >= ixMin && ix < ixMax
                                && iy >= iyMin && iy < iyMax)
                            {
                                const unsigned int ox = (unsigned int)
                                    ((ix - ixMin + 0.5) / poolWidth);
                                const unsigned int oy = (unsigned int)
                                    ((iy - iyMin + 0.5) / poolHeight);

                                assert(ox < mOutputs.dimX());
                                assert(oy < mOutputs.dimY());

                                const PoolCell_Frame_Kernels::ArgMax inputMax
                                    = mArgMax[k-1](ox, oy, channel, batchPos);

                                if (ix == inputMax.ix
                                    && iy == inputMax.iy
                                    && inputMax.valid)
                                {
                                    #pragma omp atomic
                                    diffOutput(ix, iy, channel, inputBatch)
                                        += alpha * mDiffInputs(ox, oy,
                                            outputOffset + channel, batchPos);
                                }
                            }

                            //mDiffOutputs[k](ix, iy, channel, inputBatch)
                            //    = alpha * poolGradient
                            //      + betaBatch * mDiffOutputs[k](ix, iy, channel,
                            //                               inputBatch);
                        }
                    }
                }
            }
        }
        else {
            throw std::runtime_error("ROIPoolingCell_Frame::backPropagate():"
                                     " only Max pooling back-"
                                     "propagation is implemented");
        }

        mDiffOutputs[k] = diffOutput;
        //mDiffOutputs[k].setValid();

        outputOffset += mDiffOutputs[k].dimZ();
    }

    mDiffOutputs.synchronizeHToD();
}

void N2D2::ROIPoolingCell_Frame::update()
{
    Cell_Frame<float>::update();
}

void N2D2::ROIPoolingCell_Frame::checkGradient(double epsilon, double maxError)
{
    GradientCheck<Float_T> gc(epsilon, maxError);

    mInputs[0].setValid();
    gc.initialize(mInputs,
                  mOutputs,
                  mDiffInputs,
                  std::bind(&ROIPoolingCell_Frame::propagate, this, false),
                  std::bind(&ROIPoolingCell_Frame::backPropagate, this),
                  (mPooling == Max));
    mInputs[0].clearValid();

    for (unsigned int k = 0; k < mInputs.size(); ++k) {
        if (mDiffOutputs[k].empty()) {
            std::cout << Utils::cwarning << "Empty diff. outputs #" << k
                    << " for cell " << mName
                    << ", could not check the gradient!" << Utils::cdef
                    << std::endl;
            continue;
        }

        std::stringstream name;
        name << mName + "_mDiffOutputs[" << k << "]";

        gc.check(name.str(), mInputs[k], mDiffOutputs[k]);
    }
}

N2D2::ROIPoolingCell_Frame::~ROIPoolingCell_Frame()
{
    for (unsigned int k = 0, size = mArgMax.size(); k < size; ++k)
        delete &mArgMax[k];
}
