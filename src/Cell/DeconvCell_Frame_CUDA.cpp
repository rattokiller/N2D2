/*
    (C) Copyright 2013 CEA LIST. All Rights Reserved.
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

#ifdef CUDA

#include "Filler/Filler.hpp"
#include "Filler/NormalFiller.hpp"
#include "GradientCheck.hpp"
#include "Cell/DeconvCell_Frame_CUDA.hpp"
#include "DeepNet.hpp"
#include "third_party/half.hpp"

template <>
N2D2::Registrar<N2D2::DeconvCell>
N2D2::DeconvCell_Frame_CUDA<half_float::half>::mRegistrar("Frame_CUDA",
        N2D2::DeconvCell_Frame_CUDA<half_float::half>::create,
        N2D2::Registrar<N2D2::DeconvCell>::Type<half_float::half>());

template <>
N2D2::Registrar<N2D2::DeconvCell>
N2D2::DeconvCell_Frame_CUDA<float>::mRegistrar("Frame_CUDA",
        N2D2::DeconvCell_Frame_CUDA<float>::create,
        N2D2::Registrar<N2D2::DeconvCell>::Type<float>());

template <>
N2D2::Registrar<N2D2::DeconvCell>
N2D2::DeconvCell_Frame_CUDA<double>::mRegistrar("Frame_CUDA",
        N2D2::DeconvCell_Frame_CUDA<double>::create,
        N2D2::Registrar<N2D2::DeconvCell>::Type<double>());

template <class T>
N2D2::DeconvCell_Frame_CUDA<T>::DeconvCell_Frame_CUDA(
    const DeepNet& deepNet, 
    const std::string& name,
    const std::vector<unsigned int>& kernelDims,
    unsigned int nbOutputs,
    const std::vector<unsigned int>& strideDims,
    const std::vector<int>& paddingDims,
    const std::vector<unsigned int>& dilationDims,
    const std::shared_ptr<Activation>& activation)
    : Cell(deepNet, name, nbOutputs),
      DeconvCell(deepNet, name,
                 kernelDims,
                 nbOutputs,
                 strideDims,
                 paddingDims,
                 dilationDims),
      Cell_Frame_CUDA<T>(deepNet, name, nbOutputs, activation),
      // IMPORTANT: Do not change the value of the parameters here! Use
      // setParameter() or loadParameters().
      mBias(std::make_shared<CudaTensor<T> >()),
      mDiffBias({1, 1, getNbOutputs(), 1}),
      mWorkspaceSize(0)
{
    if (strideDims.size() != kernelDims.size()) {
        throw std::domain_error("DeconvCell_Frame_CUDA: the number of dimensions"
                                " of stride must match the number of"
                                " dimensions of the kernel.");
    }

    if (paddingDims.size() != kernelDims.size()) {
        throw std::domain_error("DeconvCell_Frame_CUDA: the number of dimensions"
                                " of padding must match the number of"
                                " dimensions of the kernel.");
    }

    if (dilationDims.size() != kernelDims.size()) {
        throw std::domain_error("DeconvCell_Frame_CUDA: the number of dimensions"
                                " of dilation must match the number of"
                                " dimensions of the kernel.");
    }

    mWeightsFiller = std::make_shared<NormalFiller<T> >(0.0, 0.05);
    mBiasFiller = std::make_shared<NormalFiller<T> >(0.0, 0.05);
    mWeightsSolver = std::make_shared<SGDSolver_Frame_CUDA<T> >();
    mBiasSolver = std::make_shared<SGDSolver_Frame_CUDA<T> >();

    CHECK_CUDNN_STATUS(cudnnCreateConvolutionDescriptor(&mConvDesc));

    int count;
    CHECK_CUDA_STATUS(cudaGetDeviceCount(&count));

    mWorkspace.resize(count, NULL);
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::initialize()
{
    if (!mNoBias) {
        if (mBias->empty()) {
            mBias->resize({1, 1, getNbOutputs(), 1});
            mBiasFiller->apply((*mBias));
            mBias->synchronizeHToD();
        }
        else {
            if (mBias->dimX() != 1 || mBias->dimY() != 1
                || mBias->dimZ() != getNbOutputs() || mBias->dimB() != 1)
            {
                throw std::runtime_error("DeconvCell_Frame_CUDA<T>::initialize():"
                    " in cell " + mName + ", wrong size for shared bias");
            }
        }
    }

    const std::vector<int> strides(mStrideDims.rbegin(), mStrideDims.rend());
    const std::vector<int> paddings(mPaddingDims.rbegin(), mPaddingDims.rend());
    const std::vector<int> upscales(mDilationDims.rbegin(),
                                    mDilationDims.rend());

    CHECK_CUDNN_STATUS(
        cudnnSetConvolutionNdDescriptor(mConvDesc,
                                        mKernelDims.size(),
                                        &paddings[0],
                                        &strides[0],
                                        &upscales[0],
                                        CUDNN_CROSS_CORRELATION,
                                        CudaContext::data_type<T>::value));

    unsigned int nbChannels = 0;

    mNbGroups.clear();
    mWeightsSolvers.clear();
    mDiffSharedSynapses.clear();

    for (unsigned int k = 0, size = mFilterDesc.size(); k < size; ++k)
        cudnnDestroyFilterDescriptor(mFilterDesc[k]);
    mFilterDesc.clear();

    for (unsigned int k = 0, size = mInputs.size(); k < size; ++k) {
        if (mInputs[k].size() == 0) {
            throw std::runtime_error("Zero-sized input for DeconvCell "
                                     + mName);
        }

        mNbGroups.push_back(getNbGroups(mMapping.rows(nbChannels,
                                                mInputs[k].dimZ())));
        mWeightsSolvers.push_back(mWeightsSolver->clone());

        if (k >= mSharedSynapses.size()) {
            typename std::map<unsigned int,
                std::pair<CudaInterface<T>*, unsigned int> >::iterator
                    it = mExtSharedSynapses.find(k);

            std::vector<size_t> kernelDims(mKernelDims.begin(),
                                           mKernelDims.end());

#if CUDNN_VERSION >= 7000
            if (mNbGroups[k] > 1)
                kernelDims.push_back(getNbOutputs() / mNbGroups[k]);
            else
#endif
                kernelDims.push_back(getNbOutputs());

            kernelDims.push_back(mInputs[k].dimZ());

            if (it != mExtSharedSynapses.end()) {
                CudaTensor<T>* extWeights
                    = &((*(*it).second.first)[(*it).second.second]);

                if (!std::equal(kernelDims.begin(), kernelDims.end(),
                                extWeights->dims().begin()))
                {
                    std::stringstream errorStr;
                    errorStr << "DeconvCell_Frame_CUDA<T>::initialize(): in cell "
                        << mName << ", mismatch between external weights dim. ("
                        << extWeights->dims() << ") and expected dim. ("
                        << kernelDims << ")";

                    throw std::runtime_error(errorStr.str());
                }

                mSharedSynapses.push_back(extWeights);
            }
            else {
                // Weight filler expect dimZ as input and dimB as output
                std::vector<size_t> fillerKernelDims(kernelDims);
                std::swap(fillerKernelDims.back(),
                        fillerKernelDims[kernelDims.size() - 2]);

                CudaTensor<T>* sharedSynapses = new CudaTensor<T>(fillerKernelDims);
                mWeightsFiller->apply(*sharedSynapses);
                // Inverse dimZ and dimB for Deconv
                sharedSynapses->reshape(kernelDims);

                mSharedSynapses.push_back(sharedSynapses, 0);

#if CUDNN_VERSION >= 7000
                if (mNbGroups[k] > 1)
                    cudnnSetConvolutionGroupCount(mConvDesc, mNbGroups[k]);
                else
#endif
                if (mNbGroups[k] == 0) {
                    // Set the non-connected kernels coefficients to 0
                    for (unsigned int output = 0; output < getNbOutputs(); ++output) {
                        for (unsigned int channel = 0; channel < mInputs[k].dimZ();
                            ++channel) {
                            if (!isConnection(nbChannels + channel, output))
                                mSharedSynapses.back()[channel][output]
                                                                    .fill(T(0.0));
                        }
                    }
                }

                mSharedSynapses.back().synchronizeHToD();
            }
        }

        nbChannels += mInputs[k].dimZ();

        mDiffSharedSynapses.push_back(
            new CudaTensor<T>(mSharedSynapses[k].dims()), 0);

        mFilterDesc.push_back(cudnnFilterDescriptor_t());

        const std::vector<int> cudaKernelDims(mSharedSynapses[k].dims().rbegin(),
                                            mSharedSynapses[k].dims().rend());

        CHECK_CUDNN_STATUS(cudnnCreateFilterDescriptor(&mFilterDesc.back()));
#if CUDNN_VERSION >= 5000
        CHECK_CUDNN_STATUS(cudnnSetFilterNdDescriptor(mFilterDesc.back(),
                                                      CudaContext::data_type<T>::value,
                                                      CUDNN_TENSOR_NCHW,
                                                      cudaKernelDims.size(),
                                                      &cudaKernelDims[0]));
#else
        CHECK_CUDNN_STATUS(cudnnSetFilterNdDescriptor(mFilterDesc.back(),
                                                      CudaContext::data_type<T>::value,
                                                      cudaKernelDims.size(),
                                                      &cudaKernelDims[0]));
#endif

        // Need to cast mInputs[k] so that getCudnnTensorDesc() returns the
        // right data type. No need to actually copy any data.
        std::shared_ptr<CudaDeviceTensor<T> > input
            = cuda_device_tensor_cast_nocopy<T>(mInputs[k]);

#if CUDNN_VERSION >= 7000
        int maxAlgoIterations = 0;
        cudnnGetConvolutionForwardAlgorithmMaxCount(CudaContext::cudnnHandle(),
                                                    &maxAlgoIterations);
        if (maxAlgoIterations == 0)
            throw std::runtime_error("No available CUDNN ConvolutionForwardAlgorithm for cell  " + mName);

        int returnAlgoCounts = 0;

        std::vector<cudnnConvolutionFwdAlgoPerf_t> returnFwdAlgo(maxAlgoIterations);
/**************************************************************************************************************
https://docs.nvidia.com/deeplearning/sdk/cudnn-developer-guide/index.html#cudnnFindConvolutionForwardAlgorithm
This function attempts all cuDNN algorithms (including CUDNN_TENSOR_OP_MATH and CUDNN_DEFAULT_MATH
versions of algorithms where CUDNN_TENSOR_OP_MATH may be available) for cudnnConvolutionForward(),
using memory allocated via cudaMalloc(), and outputs performance metrics to a user-allocated array
of cudnnConvolutionFwdAlgoPerf_t. These metrics are written in sorted fashion where the first element
has the lowest compute time. The total number of resulting algorithms can be queried through
the API cudnnGetConvolutionForwardMaxCount().
***************************************************************************************************************/

        CHECK_CUDNN_STATUS(cudnnFindConvolutionForwardAlgorithm(
                            CudaContext::cudnnHandle(),
                            mOutputs.getCudnnTensorDesc(),
                            mFilterDesc.back(),
                            mConvDesc,
                            input->getCudnnTensorDesc(),
                            maxAlgoIterations,
                            &returnAlgoCounts,
                            &returnFwdAlgo[0]));
        // std::cout << "Layer " << mName << "(" << k  << ")"
        //     << " cuDNN forward algorithm heuristic results: " << std::endl;

        for(unsigned int fwdAlgo = 0; fwdAlgo < (unsigned int) maxAlgoIterations; ++fwdAlgo)
        {


            std::string algoName
                                = (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_GEMM)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_GEMM"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_DIRECT)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_DIRECT"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_FFT)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_FFT"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_FFT_TILING)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_FFT_TILING"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD_NONFUSED)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD_NONFUSED"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_COUNT)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_COUNT"
                                : "Undetermined Algorithm";


            // std::cout << "----> Forward convolution algorithm: " << algoName
            //     << " [" << returnFwdAlgo[fwdAlgo].time << " ms][" << returnFwdAlgo[fwdAlgo].memory / 1.0e6 << " MB]"
            //     << std::endl;
        }
        mFwdAlgo.push_back(returnFwdAlgo[0].algo);
#else

        mFwdAlgo.push_back(cudnnConvolutionFwdAlgo_t());


        CHECK_CUDNN_STATUS(cudnnGetConvolutionForwardAlgorithm(
            CudaContext::cudnnHandle(),
            mOutputs.getCudnnTensorDesc(),
            mFilterDesc.back(),
            mConvDesc,
            input->getCudnnTensorDesc(),
            CUDNN_CONVOLUTION_FWD_PREFER_FASTEST,
            0,
            &mFwdAlgo.back()));

#endif

#if CUDNN_VERSION >= 7000

        maxAlgoIterations = 0;
        cudnnGetConvolutionBackwardFilterAlgorithmMaxCount(CudaContext::cudnnHandle(),
                                                    &maxAlgoIterations);
        if (maxAlgoIterations == 0)
            throw std::runtime_error("No available CUDNN ConvolutionBackwardFilterAlgorithm for cell  " + mName);

        returnAlgoCounts = 0;
        std::vector< cudnnConvolutionBwdFilterAlgoPerf_t > returnBwdFilterAlgo(maxAlgoIterations);


        CHECK_CUDNN_STATUS(cudnnFindConvolutionBackwardFilterAlgorithm(
                            CudaContext::cudnnHandle(),
                            mOutputs.getCudnnTensorDesc(),
                            input->getCudnnTensorDesc(),
                            mConvDesc,
                            mFilterDesc.back(),
                            maxAlgoIterations,
                            &returnAlgoCounts,
                            &returnBwdFilterAlgo[0]));

        for(unsigned int bwdAlgo = 0; bwdAlgo < (unsigned int) maxAlgoIterations; ++bwdAlgo)
        {
            std::string algoName
                                = (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_FFT)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_FFT"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_3)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_3"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_WINOGRAD)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_WINOGRAD"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_WINOGRAD_NONFUSED)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_WINOGRAD_NONFUSED"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_FFT_TILING)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_FFT_TILING"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_COUNT)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_COUNT"
                                : "Undetermined Algorithm";


            // std::cout << "----> Backward filter convolution algorithm: " << algoName
            //     << " [" << returnBwdFilterAlgo[bwdAlgo].time << " ms][" << returnBwdFilterAlgo[bwdAlgo].memory / 1.0e6 << " MB]"
            //     << std::endl;
        }
        mBwdFilterAlgo.push_back(returnBwdFilterAlgo[0].algo);

        maxAlgoIterations = 0;
        cudnnGetConvolutionBackwardDataAlgorithmMaxCount(CudaContext::cudnnHandle(),
                                                    &maxAlgoIterations);
        if (maxAlgoIterations == 0)
            throw std::runtime_error("No available CUDNN ConvolutionBackwardDataAlgorithm for cell  " + mName);

        returnAlgoCounts = 0;
        std::vector< cudnnConvolutionBwdDataAlgoPerf_t > returnBwdDataAlgo(maxAlgoIterations);

        CHECK_CUDNN_STATUS(cudnnFindConvolutionBackwardDataAlgorithm(
                            CudaContext::cudnnHandle(),
                            mFilterDesc.back(),
                            input->getCudnnTensorDesc(),
                            mConvDesc,
                            mOutputs.getCudnnTensorDesc(),
                            maxAlgoIterations,
                            &returnAlgoCounts,
                            &returnBwdDataAlgo[0]));
        for(unsigned int bwdAlgo = 0; bwdAlgo < (unsigned int) maxAlgoIterations; ++bwdAlgo)
        {
            std::string algoName
                                = (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_0)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_0"
                                : (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_1)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_1"
                                : (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT"
                                : (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT_TILING)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT_TILING"
                                : (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD"
                                : (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD_NONFUSED)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD_NONFUSED"
                                : (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_COUNT)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_COUNT"
                                : "Undetermined Algorithm";


            // std::cout << "----> Backward data convolution algorithm: " << algoName
            //     << " [" << returnBwdDataAlgo[bwdAlgo].time << " ms][" << returnBwdDataAlgo[bwdAlgo].memory / 1.0e6 << " MB]"
            //     << std::endl;
        }

        mBwdDataAlgo.push_back(returnBwdDataAlgo[0].algo);

#else

#if CUDNN_VERSION >= 5000 && CUDNN_VERSION < 7000
        mBwdFilterAlgo.push_back(cudnnConvolutionBwdFilterAlgo_t());

        CHECK_CUDNN_STATUS(cudnnGetConvolutionBackwardFilterAlgorithm(
            CudaContext::cudnnHandle(),
            mOutputs.getCudnnTensorDesc(),
            input->getCudnnTensorDesc(),
            mConvDesc,
            mFilterDesc.back(),
            CUDNN_CONVOLUTION_BWD_FILTER_PREFER_FASTEST,
            0,
            &mBwdFilterAlgo.back()));

        mBwdDataAlgo.push_back(cudnnConvolutionBwdDataAlgo_t());

        CHECK_CUDNN_STATUS(cudnnGetConvolutionBackwardDataAlgorithm(
            CudaContext::cudnnHandle(),
            mFilterDesc.back(),
            input->getCudnnTensorDesc(),
            mConvDesc,
            mOutputs.getCudnnTensorDesc(),
            CUDNN_CONVOLUTION_BWD_DATA_PREFER_FASTEST,
            0,
            &mBwdDataAlgo.back()));
#endif

#endif


        size_t workspaceSize = 0;

        CHECK_CUDNN_STATUS(cudnnGetConvolutionForwardWorkspaceSize(
            CudaContext::cudnnHandle(),
            mOutputs.getCudnnTensorDesc(),
            mFilterDesc.back(),
            mConvDesc,
            input->getCudnnTensorDesc(),
            mFwdAlgo.back(),
            &workspaceSize));

        if (workspaceSize > mWorkspaceSize)
            mWorkspaceSize = workspaceSize;

#if CUDNN_VERSION >= 5000
        CHECK_CUDNN_STATUS(cudnnGetConvolutionBackwardFilterWorkspaceSize(
            CudaContext::cudnnHandle(),
            // same arguments as cudnnGetConvolutionBackwardFilterAlgorithm()
            // -->
            mOutputs.getCudnnTensorDesc(),
            input->getCudnnTensorDesc(),
            mConvDesc,
            mFilterDesc.back(),
            // <--
            mBwdFilterAlgo.back(),
            &workspaceSize));

        if (workspaceSize > mWorkspaceSize)
            mWorkspaceSize = workspaceSize;

        CHECK_CUDNN_STATUS(cudnnGetConvolutionBackwardDataWorkspaceSize(
            CudaContext::cudnnHandle(),
            // same arguments as cudnnGetConvolutionBackwardDataAlgorithm() -->
            mFilterDesc.back(),
            input->getCudnnTensorDesc(),
            mConvDesc,
            mOutputs.getCudnnTensorDesc(),
            // <--
            mBwdDataAlgo.back(),
            &workspaceSize));
#endif

        if (workspaceSize > mWorkspaceSize)
            mWorkspaceSize = workspaceSize;
    }

    int dev;
    CHECK_CUDA_STATUS(cudaGetDevice(&dev));

    mBias->broadcastAnyTo(dev);
    mSharedSynapses.broadcastAnyTo(dev);

    if (mWorkspaceSize > 0) {
         assert(dev < (int)mWorkspace.size());

        if (mWorkspace[dev] != NULL)
            cudaFree(mWorkspace[dev]);

        CHECK_CUDA_STATUS(cudaMalloc(&mWorkspace[dev], mWorkspaceSize));
    }
}







template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::initializeParameters(unsigned int nbInputChannels, unsigned int nbInputs)
{
    // BEGIN: addition to initialize()
    
    // NOTE: Mapping has to be initialized here because required by cuDNN
    if (mMapping.empty()) {
        mMapping.append(Tensor<bool>({getNbOutputs(), nbInputs*nbInputChannels}, true));
    }
    // TODO: This is only required because getNbChannels() uses the input tensor dimensions to infer the number of input channels. 
    // However, this requires a reinitialization of the input dims which is unsafe
    setInputsDims({nbInputChannels});
    // END: addition to initialize()
    
    if (!mNoBias) {
        if (mBias->empty()) {
            mBias->resize({1, 1, getNbOutputs(), 1});
            mBiasFiller->apply((*mBias));
            mBias->synchronizeHToD();
        }
        else {
            if (mBias->dimX() != 1 || mBias->dimY() != 1
                || mBias->dimZ() != getNbOutputs() || mBias->dimB() != 1)
            {
                throw std::runtime_error("DeconvCell_Frame_CUDA<T>::initializeParameters():"
                    " in cell " + mName + ", wrong size for shared bias");
            }
        }
    }

    const std::vector<int> strides(mStrideDims.rbegin(), mStrideDims.rend());
    const std::vector<int> paddings(mPaddingDims.rbegin(), mPaddingDims.rend());
    const std::vector<int> upscales(mDilationDims.rbegin(),
                                    mDilationDims.rend());

    CHECK_CUDNN_STATUS(
        cudnnSetConvolutionNdDescriptor(mConvDesc,
                                        mKernelDims.size(),
                                        &paddings[0],
                                        &strides[0],
                                        &upscales[0],
                                        CUDNN_CROSS_CORRELATION,
                                        CudaContext::data_type<T>::value));

    unsigned int nbChannels = 0;

    for (unsigned int k = 0, size = nbInputs; k < size; ++k) {

        if (k < mNbGroups.size()) {
            nbChannels += nbInputChannels;
            continue;  // already initialized, skip!
        }

        mNbGroups.push_back(getNbGroups(mMapping.rows(nbChannels,
                                                   nbInputChannels)));

        mWeightsSolvers.push_back(mWeightsSolver->clone());

        typename std::map<unsigned int,
            std::pair<CudaInterface<T>*, unsigned int> >::iterator
                it = mExtSharedSynapses.find(k);

        std::vector<size_t> kernelDims(mKernelDims.begin(), mKernelDims.end());

#if CUDNN_VERSION >= 7000
        if (mNbGroups[k] > 1)
            kernelDims.push_back(getNbOutputs() / mNbGroups[k]);
        else
            kernelDims.push_back(getNbOutputs());
#endif

        kernelDims.push_back(nbInputChannels);

        if (it != mExtSharedSynapses.end()) {
            CudaTensor<T>* extWeights
                = &((*(*it).second.first)[(*it).second.second]);

            if (!std::equal(kernelDims.begin(), kernelDims.end(),
                            extWeights->dims().begin()))
            {
                std::stringstream errorStr;
                errorStr << "DeconvCell_Frame_CUDA<T>::initialize(): in cell "
                    << mName << ", mismatch between external weights dim. ("
                    << extWeights->dims() << ") and expected dim. ("
                    << kernelDims << ")";

                throw std::runtime_error(errorStr.str());
            }

            mSharedSynapses.push_back(extWeights);
        }
        else {
            // Weight filler expect dimZ as input and dimB as output
            std::vector<size_t> fillerKernelDims(kernelDims);
            std::swap(fillerKernelDims.back(),
                      fillerKernelDims[kernelDims.size() - 2]);

            CudaTensor<T>* sharedSynapses = new CudaTensor<T>(fillerKernelDims);
            mWeightsFiller->apply(*sharedSynapses);
            // Inverse dimZ and dimB for Deconv
            sharedSynapses->reshape(kernelDims);

            mSharedSynapses.push_back(sharedSynapses, 0);

#if CUDNN_VERSION >= 7000
            if (mNbGroups[k] > 1)
                cudnnSetConvolutionGroupCount(mConvDesc, mNbGroups[k]);
            else
#endif
            if (mNbGroups[k] == 0) {
                // Set the non-connected kernels coefficients to 0
                for (unsigned int output = 0; output < getNbOutputs(); ++output) {
                    for (unsigned int channel = 0; channel < nbInputChannels;
                         ++channel) {
                        if (!isConnection(nbChannels + channel, output))
                            mSharedSynapses.back()[channel][output]
                                                                .fill(T(0.0));
                    }
                }
            }

            mSharedSynapses.back().synchronizeHToD();
        }

        mDiffSharedSynapses.push_back(new CudaTensor<T>(kernelDims), 0);

        mFilterDesc.push_back(cudnnFilterDescriptor_t());

        const std::vector<int> cudaKernelDims(kernelDims.rbegin(),
                                              kernelDims.rend());

        CHECK_CUDNN_STATUS(cudnnCreateFilterDescriptor(&mFilterDesc.back()));
#if CUDNN_VERSION >= 5000
        CHECK_CUDNN_STATUS(cudnnSetFilterNdDescriptor(mFilterDesc.back(),
                                                      CudaContext::data_type<T>::value,
                                                      CUDNN_TENSOR_NCHW,
                                                      cudaKernelDims.size(),
                                                      &cudaKernelDims[0]));
#else
        CHECK_CUDNN_STATUS(cudnnSetFilterNdDescriptor(mFilterDesc.back(),
                                                      CudaContext::data_type<T>::value,
                                                      cudaKernelDims.size(),
                                                      &cudaKernelDims[0]));
#endif

        nbChannels += nbInputChannels;

    }

}


template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::check_input()
{
    if (mInputs.size() != mSharedSynapses.size()) {
          throw std::runtime_error("mInputs.size() != mSharedSynapses.size() for cell " + mName + 
          ". Please verify that the number of input tensors given to the cell is"
          " equal to the number of inputs defined for the cell.");
    }
    for (unsigned int k = 0, size = mInputs.size(); k < size; ++k) {
        if (mInputs[k].dimZ() != mSharedSynapses[k].dimZ()){
            std::cout << "mInputs.dimZ(): " << mInputs[k].dimZ() << std::endl;
            std::cout << "mSharedSynapses.dimZ(): " << mSharedSynapses[k].dimZ() << std::endl;
            std::stringstream ss;
            ss << "Unmatching dimension Z"
            " between input and weight " << k << " for cell " + mName;
            throw std::runtime_error(ss.str());
        }
    }
}


template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::initializeDataDependent() 
{
    // NOTE: this is addition to initialize()
    Cell_Frame_CUDA<T>::initializeDataDependent();
    
    check_input();

    unsigned int nbChannels = 0;

    for (unsigned int k = 0, size = mInputs.size(); k < size; ++k) {
        if (mInputs[k].size() == 0) {
            throw std::runtime_error("Zero-sized input for DeconvCell "
                                     + mName);
        }

       // Need to cast mInputs[k] so that getCudnnTensorDesc() returns the
        // right data type. No need to actually copy any data.
        std::shared_ptr<CudaDeviceTensor<T> > input
            = cuda_device_tensor_cast_nocopy<T>(mInputs[k]);

#if CUDNN_VERSION >= 7000
        int maxAlgoIterations = 0;
        cudnnGetConvolutionForwardAlgorithmMaxCount(CudaContext::cudnnHandle(),
                                                    &maxAlgoIterations);
        if (maxAlgoIterations == 0)
            throw std::runtime_error("No available CUDNN ConvolutionForwardAlgorithm for cell  " + mName);

        int returnAlgoCounts = 0;

        std::vector<cudnnConvolutionFwdAlgoPerf_t> returnFwdAlgo(maxAlgoIterations);
/**************************************************************************************************************
https://docs.nvidia.com/deeplearning/sdk/cudnn-developer-guide/index.html#cudnnFindConvolutionForwardAlgorithm
This function attempts all cuDNN algorithms (including CUDNN_TENSOR_OP_MATH and CUDNN_DEFAULT_MATH
versions of algorithms where CUDNN_TENSOR_OP_MATH may be available) for cudnnConvolutionForward(),
using memory allocated via cudaMalloc(), and outputs performance metrics to a user-allocated array
of cudnnConvolutionFwdAlgoPerf_t. These metrics are written in sorted fashion where the first element
has the lowest compute time. The total number of resulting algorithms can be queried through
the API cudnnGetConvolutionForwardMaxCount().
***************************************************************************************************************/

        CHECK_CUDNN_STATUS(cudnnFindConvolutionForwardAlgorithm(
                            CudaContext::cudnnHandle(),
                            mOutputs.getCudnnTensorDesc(),
                            mFilterDesc.back(),
                            mConvDesc,
                            input->getCudnnTensorDesc(),
                            maxAlgoIterations,
                            &returnAlgoCounts,
                            &returnFwdAlgo[0]));
        // std::cout << "Layer " << mName << "(" << k  << ")"
        //     << " cuDNN forward algorithm heuristic results: " << std::endl;

        for(unsigned int fwdAlgo = 0; fwdAlgo < (unsigned int) maxAlgoIterations; ++fwdAlgo)
        {


            std::string algoName
                                = (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_GEMM)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_GEMM"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_DIRECT)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_DIRECT"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_FFT)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_FFT"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_FFT_TILING)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_FFT_TILING"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD_NONFUSED)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD_NONFUSED"
                                : (returnFwdAlgo[fwdAlgo].algo
                                        == CUDNN_CONVOLUTION_FWD_ALGO_COUNT)
                                    ? "CUDNN_CONVOLUTION_FWD_ALGO_COUNT"
                                : "Undetermined Algorithm";


            // std::cout << "----> Forward convolution algorithm: " << algoName
            //     << " [" << returnFwdAlgo[fwdAlgo].time << " ms][" << returnFwdAlgo[fwdAlgo].memory / 1.0e6 << " MB]"
            //     << std::endl;
        }
        mFwdAlgo.push_back(returnFwdAlgo[0].algo);
#else

        mFwdAlgo.push_back(cudnnConvolutionFwdAlgo_t());


        CHECK_CUDNN_STATUS(cudnnGetConvolutionForwardAlgorithm(
            CudaContext::cudnnHandle(),
            mOutputs.getCudnnTensorDesc(),
            mFilterDesc.back(),
            mConvDesc,
            input->getCudnnTensorDesc(),
            CUDNN_CONVOLUTION_FWD_PREFER_FASTEST,
            0,
            &mFwdAlgo.back()));

#endif

#if CUDNN_VERSION >= 7000

        maxAlgoIterations = 0;
        cudnnGetConvolutionBackwardFilterAlgorithmMaxCount(CudaContext::cudnnHandle(),
                                                    &maxAlgoIterations);
        if (maxAlgoIterations == 0)
            throw std::runtime_error("No available CUDNN ConvolutionBackwardFilterAlgorithm for cell  " + mName);

        returnAlgoCounts = 0;
        std::vector< cudnnConvolutionBwdFilterAlgoPerf_t > returnBwdFilterAlgo(maxAlgoIterations);


        CHECK_CUDNN_STATUS(cudnnFindConvolutionBackwardFilterAlgorithm(
                            CudaContext::cudnnHandle(),
                            mOutputs.getCudnnTensorDesc(),
                            input->getCudnnTensorDesc(),
                            mConvDesc,
                            mFilterDesc.back(),
                            maxAlgoIterations,
                            &returnAlgoCounts,
                            &returnBwdFilterAlgo[0]));

        for(unsigned int bwdAlgo = 0; bwdAlgo < (unsigned int) maxAlgoIterations; ++bwdAlgo)
        {
            std::string algoName
                                = (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_FFT)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_FFT"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_3)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_3"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_WINOGRAD)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_WINOGRAD"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_WINOGRAD_NONFUSED)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_WINOGRAD_NONFUSED"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_FFT_TILING)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_FFT_TILING"
                                : (returnBwdFilterAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_COUNT)
                                    ? "CUDNN_CONVOLUTION_BWD_FILTER_ALGO_COUNT"
                                : "Undetermined Algorithm";


            // std::cout << "----> Backward filter convolution algorithm: " << algoName
            //     << " [" << returnBwdFilterAlgo[bwdAlgo].time << " ms][" << returnBwdFilterAlgo[bwdAlgo].memory / 1.0e6 << " MB]"
            //     << std::endl;
        }
        mBwdFilterAlgo.push_back(returnBwdFilterAlgo[0].algo);

        maxAlgoIterations = 0;
        cudnnGetConvolutionBackwardDataAlgorithmMaxCount(CudaContext::cudnnHandle(),
                                                    &maxAlgoIterations);
        if (maxAlgoIterations == 0)
            throw std::runtime_error("No available CUDNN ConvolutionBackwardDataAlgorithm for cell  " + mName);

        returnAlgoCounts = 0;
        std::vector< cudnnConvolutionBwdDataAlgoPerf_t > returnBwdDataAlgo(maxAlgoIterations);

        CHECK_CUDNN_STATUS(cudnnFindConvolutionBackwardDataAlgorithm(
                            CudaContext::cudnnHandle(),
                            mFilterDesc.back(),
                            input->getCudnnTensorDesc(),
                            mConvDesc,
                            mOutputs.getCudnnTensorDesc(),
                            maxAlgoIterations,
                            &returnAlgoCounts,
                            &returnBwdDataAlgo[0]));
        for(unsigned int bwdAlgo = 0; bwdAlgo < (unsigned int) maxAlgoIterations; ++bwdAlgo)
        {
            std::string algoName
                                = (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_0)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_0"
                                : (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_1)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_1"
                                : (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT"
                                : (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT_TILING)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT_TILING"
                                : (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD"
                                : (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD_NONFUSED)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD_NONFUSED"
                                : (returnBwdDataAlgo[bwdAlgo].algo
                                        == CUDNN_CONVOLUTION_BWD_DATA_ALGO_COUNT)
                                    ? "CUDNN_CONVOLUTION_BWD_DATA_ALGO_COUNT"
                                : "Undetermined Algorithm";


            // std::cout << "----> Backward data convolution algorithm: " << algoName
            //     << " [" << returnBwdDataAlgo[bwdAlgo].time << " ms][" << returnBwdDataAlgo[bwdAlgo].memory / 1.0e6 << " MB]"
            //     << std::endl;
        }

        mBwdDataAlgo.push_back(returnBwdDataAlgo[0].algo);

#else

#if CUDNN_VERSION >= 5000 && CUDNN_VERSION < 7000
        mBwdFilterAlgo.push_back(cudnnConvolutionBwdFilterAlgo_t());

        CHECK_CUDNN_STATUS(cudnnGetConvolutionBackwardFilterAlgorithm(
            CudaContext::cudnnHandle(),
            mOutputs.getCudnnTensorDesc(),
            input->getCudnnTensorDesc(),
            mConvDesc,
            mFilterDesc.back(),
            CUDNN_CONVOLUTION_BWD_FILTER_PREFER_FASTEST,
            0,
            &mBwdFilterAlgo.back()));

        mBwdDataAlgo.push_back(cudnnConvolutionBwdDataAlgo_t());

        CHECK_CUDNN_STATUS(cudnnGetConvolutionBackwardDataAlgorithm(
            CudaContext::cudnnHandle(),
            mFilterDesc.back(),
            input->getCudnnTensorDesc(),
            mConvDesc,
            mOutputs.getCudnnTensorDesc(),
            CUDNN_CONVOLUTION_BWD_DATA_PREFER_FASTEST,
            0,
            &mBwdDataAlgo.back()));
#endif

#endif


        size_t workspaceSize = 0;

        CHECK_CUDNN_STATUS(cudnnGetConvolutionForwardWorkspaceSize(
            CudaContext::cudnnHandle(),
            mOutputs.getCudnnTensorDesc(),
            mFilterDesc.back(),
            mConvDesc,
            input->getCudnnTensorDesc(),
            mFwdAlgo.back(),
            &workspaceSize));

        if (workspaceSize > mWorkspaceSize)
            mWorkspaceSize = workspaceSize;

#if CUDNN_VERSION >= 5000
        CHECK_CUDNN_STATUS(cudnnGetConvolutionBackwardFilterWorkspaceSize(
            CudaContext::cudnnHandle(),
            // same arguments as cudnnGetConvolutionBackwardFilterAlgorithm()
            // -->
            mOutputs.getCudnnTensorDesc(),
            input->getCudnnTensorDesc(),
            mConvDesc,
            mFilterDesc.back(),
            // <--
            mBwdFilterAlgo.back(),
            &workspaceSize));

        if (workspaceSize > mWorkspaceSize)
            mWorkspaceSize = workspaceSize;

        CHECK_CUDNN_STATUS(cudnnGetConvolutionBackwardDataWorkspaceSize(
            CudaContext::cudnnHandle(),
            // same arguments as cudnnGetConvolutionBackwardDataAlgorithm() -->
            mFilterDesc.back(),
            input->getCudnnTensorDesc(),
            mConvDesc,
            mOutputs.getCudnnTensorDesc(),
            // <--
            mBwdDataAlgo.back(),
            &workspaceSize));
#endif

        if (workspaceSize > mWorkspaceSize)
            mWorkspaceSize = workspaceSize;

        nbChannels += mInputs[k].dimZ();
    }
    int dev;
    CHECK_CUDA_STATUS(cudaGetDevice(&dev));
    if (mWorkspaceSize > 0) {
        if (mWorkspace[dev] != NULL)
            cudaFree(mWorkspace[dev]);

        CHECK_CUDA_STATUS(cudaMalloc(&mWorkspace[dev], mWorkspaceSize));
    }
}





template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::propagate(bool inference)
{
    check_input();

    mInputs.synchronizeHBasedToD();

    /**
     * 1.0
     * Corps de la procédure de convolution via CuDNN
     * Pour plus de détails, cf. doc : cuDNN Library
     */
    const typename Cuda::cudnn_scaling_type<T>::type alpha = 1.0f;
    typename Cuda::cudnn_scaling_type<T>::type beta = 0.0f;

    int dev;
    CHECK_CUDA_STATUS(cudaGetDevice(&dev));

    for (unsigned int k = 0, size = mInputs.size(); k < size; ++k) {
        if (k > 0)
            beta = 1.0f;

        std::shared_ptr<CudaDeviceTensor<T> > input
            = cuda_device_tensor_cast<T>(mInputs[k]);

#if CUDNN_VERSION >= 5000
        CHECK_CUDNN_STATUS(
            cudnnConvolutionBackwardData(CudaContext::cudnnHandle(),
                                         &alpha,
                                         mFilterDesc[k],
                                         mSharedSynapses[k].getDevicePtr(),
                                         input->getCudnnTensorDesc(),
                                         input->getDevicePtr(),
                                         mConvDesc,
                                         mBwdDataAlgo[k],
                                         mWorkspace[dev],
                                         mWorkspaceSize,
                                         &beta,
                                         mOutputs.getCudnnTensorDesc(),
                                         mOutputs.getDevicePtr()));
#else
        CHECK_CUDNN_STATUS(
            cudnnConvolutionBackwardData(CudaContext::cudnnHandle(),
                                         &alpha,
                                         mFilterDesc[k],
                                         mSharedSynapses[k].getDevicePtr(),
                                         input->getCudnnTensorDesc(),
                                         input->getDevicePtr(),
                                         mConvDesc,
                                         &beta,
                                         mOutputs.getCudnnTensorDesc(),
                                         mOutputs.getDevicePtr()));
#endif
    }

    if (!mNoBias) {
/**
 * 2.0
 * Ajoute le biais au tenseur de destination.
 */
#if CUDNN_VERSION >= 5000
        CHECK_CUDNN_STATUS(cudnnAddTensor(CudaContext::cudnnHandle(),
                                          &alpha,
                                          mBias->getCudnnTensorDesc(),
                                          mBias->getDevicePtr(),
                                          &alpha,
                                          mOutputs.getCudnnTensorDesc(),
                                          mOutputs.getDevicePtr()));
#else
        CHECK_CUDNN_STATUS(cudnnAddTensor(CudaContext::cudnnHandle(),
                                          CUDNN_ADD_SAME_C,
                                          &alpha,
                                          mBias->getCudnnTensorDesc(),
                                          mBias->getDevicePtr(),
                                          &alpha,
                                          mOutputs.getCudnnTensorDesc(),
                                          mOutputs.getDevicePtr()));
#endif
    }

    Cell_Frame_CUDA<T>::propagate(inference);
    mDiffInputs.clearValid();
    mDiffSharedSynapses.clearValid();
    mDiffBias.clearValid();
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::backPropagate()
{
    if (!mDiffInputs.isValid())
        return;

    Cell_Frame_CUDA<T>::backPropagate();

    const typename Cuda::cudnn_scaling_type<T>::type alpha = 1.0f;

    const unsigned int kernelSize = (!mKernelDims.empty())
        ? std::accumulate(mKernelDims.begin(), mKernelDims.end(),
                          1U, std::multiplies<unsigned int>())
        : 0U;

    unsigned int nbChannels = 0;

    int dev;
    CHECK_CUDA_STATUS(cudaGetDevice(&dev));

    for (unsigned int k = 0, size = mInputs.size(); k < size; ++k) {
        const typename Cuda::cudnn_scaling_type<T>::type beta
            = (mWeightsSolvers[k]->isNewIteration()) ? 0.0f : 1.0f;

        std::shared_ptr<CudaDeviceTensor<T> > input
            = cuda_device_tensor_cast_nocopy<T>(mInputs[k]);

#if CUDNN_VERSION >= 5000
        CHECK_CUDNN_STATUS(cudnnConvolutionBackwardFilter(
            CudaContext::cudnnHandle(),
            &alpha,
            mDiffInputs.getCudnnTensorDesc(),
            mDiffInputs.getDevicePtr(),
            input->getCudnnTensorDesc(),
            input->getDevicePtr(),
            mConvDesc,
            mBwdFilterAlgo[k],
            mWorkspace[dev],
            mWorkspaceSize,
            &beta,
            mFilterDesc[k],
            mDiffSharedSynapses[k].getDevicePtr()));
#else
        CHECK_CUDNN_STATUS(cudnnConvolutionBackwardFilter(
            CudaContext::cudnnHandle(),
            &alpha,
            mDiffInputs.getCudnnTensorDesc(),
            mDiffInputs.getDevicePtr(),
            input->getCudnnTensorDesc(),
            input->getDevicePtr(),
            mConvDesc,
            &beta,
            mFilterDesc[k],
            mDiffSharedSynapses[k].getDevicePtr()));
#endif

#if CUDNN_VERSION >= 7000
        if (mNbGroups[k] > 1) {
            // Nothing to do!
        }
        else
#endif
        if (mNbGroups[k] == 0) {
            // Set the non-connected kernels diff to 0
            unsigned int offset = 0;

            for (unsigned int channel = 0; channel < mInputs[k].dimZ();
                ++channel)
            {
                for (unsigned int output = 0; output < getNbOutputs(); ++output)
                {
                    if (!isConnection(nbChannels + channel, output)) {
                        thrust_fill<T>(mDiffSharedSynapses[k].getDevicePtr()
                                            + offset,
                                       kernelSize,
                                       T(0.0));
                    }

                    offset += kernelSize;
                }
            }
        }

        mDiffSharedSynapses[k].setValid();
        nbChannels += mInputs[k].dimZ();
    }

    if (!mNoBias) {
        const typename Cuda::cudnn_scaling_type<T>::type beta
            = (mBiasSolver->isNewIteration()) ? 0.0f : 1.0f;

        CHECK_CUDNN_STATUS(
            cudnnConvolutionBackwardBias(CudaContext::cudnnHandle(),
                                         &alpha,
                                         mDiffInputs.getCudnnTensorDesc(),
                                         mDiffInputs.getDevicePtr(),
                                         &beta,
                                         mDiffBias.getCudnnTensorDesc(),
                                         mDiffBias.getDevicePtr()));

        mDiffBias.setValid();
    }

    /** Si il ne s'agit pas de la première couche */
    if (mBackPropagate) {
        for (unsigned int k = 0, size = mInputs.size(); k < size; ++k) {
            if (mDiffOutputs[k].empty())
                continue;

            const typename Cuda::cudnn_scaling_type<T>::type beta
                = (mDiffOutputs[k].isValid()) ? 1.0f : 0.0f;

            std::shared_ptr<CudaDeviceTensor<T> > diffOutput
                = (mDiffOutputs[k].isValid())
                    ? cuda_device_tensor_cast<T>(mDiffOutputs[k])
                    : cuda_device_tensor_cast_nocopy<T>(mDiffOutputs[k]);

            CHECK_CUDNN_STATUS(
                cudnnConvolutionForward(CudaContext::cudnnHandle(),
                                        &alpha,
                                        mDiffInputs.getCudnnTensorDesc(),
                                        mDiffInputs.getDevicePtr(),
                                        mFilterDesc[k],
                                        mSharedSynapses[k].getDevicePtr(),
                                        mConvDesc,
                                        mFwdAlgo[k],
                                        mWorkspace[dev],
                                        mWorkspaceSize,
                                        &beta,
                                        diffOutput->getCudnnTensorDesc(),
                                        diffOutput->getDevicePtr()));

            mDiffOutputs[k].deviceTensor() = *diffOutput;
            mDiffOutputs[k].setValid();
        }

        mDiffOutputs.synchronizeDToHBased();
    }
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::update()
{
    int dev;
    CHECK_CUDA_STATUS(cudaGetDevice(&dev));

    for (unsigned int k = 0, size = mSharedSynapses.size(); k < size; ++k) {
        if (mDiffSharedSynapses[k].isValid()) {
            mDiffSharedSynapses[k].aggregateAllTo(dev, mDevices);
            mWeightsSolvers[k]->update(
                mSharedSynapses[k], mDiffSharedSynapses[k], mInputs.dimB());
            mSharedSynapses[k].broadcastAllFrom(dev, mDevices);
        }
    }

    if (!mNoBias && mDiffBias.isValid()) {
        mDiffBias.aggregateAllTo(dev, mDevices);
        mBiasSolver->update(*mBias, mDiffBias, mInputs.dimB());
        mBias->broadcastAllFrom(dev, mDevices);
    }
    
    Cell_Frame_CUDA<T>::update();
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::setWeights(unsigned int k,
                                                BaseInterface* weights,
                                                unsigned int offset)
{
    CudaInterface<T>* cudaWeightsInterface
        = dynamic_cast<CudaInterface<T>*>(weights);

    if (!cudaWeightsInterface) {
        throw std::runtime_error("DeconvCell_Frame_CUDA<T>::setWeights(): "
                                 "incompatible types.");
    }

    mExtSharedSynapses[k] = std::make_pair(cudaWeightsInterface, offset);
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::setBiases(
    const std::shared_ptr<BaseTensor>& biases)
{
    std::shared_ptr<CudaTensor<T> > cudaBiases
        = std::dynamic_pointer_cast<CudaTensor<T> >(biases);

    if (!cudaBiases) {
        throw std::runtime_error("DeconvCell_Frame_CUDA<T>::setBiases(): biases"
                                 " must be a CudaTensor");
    }

    mBias = cudaBiases;
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::checkGradient(double epsilon, double maxError)
{
    GradientCheck<T> gc(epsilon, maxError);
    gc.initialize(mInputs,
                  mOutputs,
                  mDiffInputs,
                  std::bind(&DeconvCell_Frame_CUDA<T>::propagate, this, false),
                  std::bind(&DeconvCell_Frame_CUDA<T>::backPropagate, this));

    for (unsigned int k = 0, size = mSharedSynapses.size(); k < size; ++k) {
        std::stringstream name;
        name << mName + "_mDiffSharedSynapses[" << k << "]";

        gc.check(name.str(), mSharedSynapses[k], mDiffSharedSynapses[k]);
    }

    if (!mNoBias)
        gc.check(mName + "_mDiffBias", (*mBias), mDiffBias);

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

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::logFreeParameters(const std::string& fileName,
                                                    unsigned int output,
                                                    unsigned int channel) const
{
    synchronizeToH(false);
    DeconvCell::logFreeParameters(fileName, output, channel);
    keepInSync(true);
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::logFreeParameters(const std::string& fileName,
                                                    unsigned int output) const
{
    synchronizeToH(false);
    DeconvCell::logFreeParameters(fileName, output);
    keepInSync(true);
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::logFreeParameters(const std::string
                                                    & dirName) const
{
    synchronizeToH(false);
    DeconvCell::logFreeParameters(dirName);
    keepInSync(true);
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::saveFreeParameters(const std::string
                                                     & fileName) const
{
    std::ofstream syn(fileName.c_str(), std::fstream::binary);

    if (!syn.good())
        throw std::runtime_error("Could not create synaptic file (.SYN): "
                                 + fileName);

    mSharedSynapses.synchronizeDToH();

    for (unsigned int k = 0; k < mSharedSynapses.size(); ++k)
        mSharedSynapses[k].save(syn);

    if (!mNoBias) {
        mBias->synchronizeDToH();
        mBias->save(syn);
    }

    if (!syn.good())
        throw std::runtime_error("Error writing synaptic file: " + fileName);
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::loadFreeParameters(const std::string
                                                     & fileName,
                                                     bool ignoreNotExists)
{
    std::ifstream syn(fileName.c_str(), std::fstream::binary);

    int dev;
    CHECK_CUDA_STATUS(cudaGetDevice(&dev));

    if (!syn.good()) {
        if (ignoreNotExists) {
            std::cout << Utils::cnotice
                      << "Notice: Could not open synaptic file (.SYN): "
                      << fileName << Utils::cdef << std::endl;
            return;
        } else
            throw std::runtime_error("Could not open synaptic file (.SYN): "
                                     + fileName);
    }

    for (unsigned int k = 0; k < mSharedSynapses.size(); ++k)
        mSharedSynapses[k].load(syn);

    mSharedSynapses.synchronizeHToD();
    mSharedSynapses.broadcastAllFrom(dev);

    if (!mNoBias) {
        mBias->load(syn);
        mBias->synchronizeHToD();
        mBias->broadcastAllFrom(dev);
    }

    if (syn.eof())
        throw std::runtime_error(
            "End-of-file reached prematurely in synaptic file (.SYN): "
            + fileName);
    else if (!syn.good())
        throw std::runtime_error("Error while reading synaptic file (.SYN): "
                                 + fileName);
    else if (syn.get() != std::fstream::traits_type::eof())
        throw std::runtime_error(
            "Synaptic file (.SYN) size larger than expected: " + fileName);
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::exportFreeParameters(const std::string
                                                       & fileName) const
{
    synchronizeToH(false);
    DeconvCell::exportFreeParameters(fileName);
    keepInSync(true);
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::importFreeParameters(const std::string
                                                       & fileName,
                                                       bool ignoreNotExists)
{
    keepInSync(false);
    DeconvCell::importFreeParameters(fileName, ignoreNotExists);
    synchronizeToD(true);
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::logFreeParametersDistrib(const std::string
                                                           & fileName) const
{
    synchronizeToH(false);
    DeconvCell::logFreeParametersDistrib(fileName);
    keepInSync(true);
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::synchronizeToH(bool keepInSync_) const
{
    mSharedSynapses.synchronizeDToH();
    mBias->synchronizeDToH();
    keepInSync(keepInSync_);
}

template <class T>
void N2D2::DeconvCell_Frame_CUDA<T>::synchronizeToD(bool keepInSync_)
{
    mSharedSynapses.synchronizeHToD();
    mBias->synchronizeHToD();
    keepInSync(keepInSync_);

    int dev;
    CHECK_CUDA_STATUS(cudaGetDevice(&dev));

    mSharedSynapses.broadcastAllFrom(dev);
    mBias->broadcastAllFrom(dev);
}

template <class T>
N2D2::DeconvCell_Frame_CUDA<T>::~DeconvCell_Frame_CUDA()
{
    for (unsigned int k = 0, size = mFilterDesc.size(); k < size; ++k)
        cudnnDestroyFilterDescriptor(mFilterDesc[k]);

    if (mWorkspaceSize > 0) {
        int currentDev;
        cudaGetDevice(&currentDev);

        for (size_t dev = 0; dev < mWorkspace.size(); ++dev) {
            if (mWorkspace[dev] != NULL) {
                cudaSetDevice(dev);
                cudaFree(mWorkspace[dev]);
            }
        }

        cudaSetDevice(currentDev);
    }

    cudnnDestroyConvolutionDescriptor(mConvDesc);
}

namespace N2D2 {
    template class DeconvCell_Frame_CUDA<half_float::half>;
    template class DeconvCell_Frame_CUDA<float>;
    template class DeconvCell_Frame_CUDA<double>;
}

#endif
